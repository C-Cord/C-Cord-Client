// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Self-excluding PipeWire loopback capture. See loopback_pw.h for the contract.
//
// Verified against a live PipeWire 1.6 + wireplumber daemon:
//  * A pw_stream with node.autoconnect=false is NOT auto-linked by the session
//    manager, so we own every link into it.
//  * Registry *global* props do not carry application.process.id; the owning pid
//    must be read from the bound node (Pulse clients) or the client's
//    pipewire.sec.pid (native PipeWire clients such as SDL). We bind candidate
//    nodes and track client globals to resolve it.
//  * A capture stream does NOT process when nothing is linked (it is a follower,
//    driven by the playing apps' graph), so read() synthesizes silence when idle.
//  * Linking several output ports into one input SUMS them. To downmix without
//    attenuating when idle apps are present, the capture is STEREO: each app's
//    ports are linked by channel position (FL->L, FR->R, MONO/FC->both) and the
//    process callback folds L/R to mono with (L+R)/2. Idle apps add silence rather
//    than inflating a divisor.
#define _POSIX_C_SOURCE 200809L
#include "loopback_pw.h"

#include <ccord/core/alloc.h>
#include <ccord/core/log.h>
#include <ccord/platform/capture.h>

#include <string.h>

// ---- pure node filter (always built; unit-tested without a daemon) ------------------------------

bool ccord_loopback_pw_node_allowed(const char *media_class, long pid, long self_pid)
{
  if (!media_class) return false;
  // Only audio playback streams. "Stream/Input/Audio" (mics), "Audio/Sink",
  // "Audio/Source" and every non-audio class are excluded.
  if (strcmp(media_class, "Stream/Output/Audio") != 0) return false;
  // Unknown owner: fail closed so we can never capture ourselves by accident.
  if (pid <= 0) return false;
  return pid != self_pid;
}

#if CCORD_HAVE_PIPEWIRE

#include <pipewire/pipewire.h>
#include <pipewire/client.h>
#include <pipewire/core.h>
#include <pipewire/node.h>
#include <pipewire/properties.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define LPW_MAX_NODES 128
#define LPW_MAX_PORTS 512
#define LPW_MAX_LINKS 512
#define LPW_MAX_CLIENTS 256
#define LPW_RING_CAP 16384u   // power of two, ~340 ms of mono at 48 kHz
#define LPW_RING_MASK (LPW_RING_CAP - 1u)
#define LPW_MAX_CH 8          // output ports we link per node

typedef struct lpw_node {
  bool used;
  uint32_t id;                 // registry global id
  long pid;                    // resolved owner pid (-1 unknown)
  long app_pid;                // application.process.id once seen (never downgraded)
  long client_id;
  bool allowed;
  struct pw_proxy *proxy;      // bound node
  struct spa_hook hook;
} lpw_node;

typedef struct lpw_port {
  bool used;
  uint32_t id;                 // registry global id
  uint32_t node_id;
  bool is_out;
  char channel[16];            // audio.channel (FL/FR/MONO/FC/...)
} lpw_port;

typedef struct lpw_client {
  bool used;
  uint32_t id;
  long sec_pid;                // pipewire.sec.pid from the client global
  long app_pid;                // application.process.id from the bound client
  bool is_pulse;               // client.api == pipewire-pulse (sec.pid is the daemon, not the app)
  struct pw_proxy *proxy;      // bound client
  struct spa_hook hook;
} lpw_client;

typedef struct lpw_link {
  bool used;
  struct pw_proxy *proxy;
  uint32_t node_id;            // source (app) node
  uint32_t out_port;           // source port global id
} lpw_link;

struct ccord_loopback_pw {
  struct pw_thread_loop *loop;
  struct pw_context *context;
  struct pw_core *core;
  struct pw_registry *registry;
  struct spa_hook registry_hook;
  struct spa_hook core_hook;
  struct pw_stream *stream;
  struct spa_hook stream_hook;
  bool inited;                 // pw_init() called here
  uint32_t cap_node_id;        // our capture node (PW_ID_ANY until exported)
  uint32_t cap_in_port[2];     // our capture input ports: [0]=FL(L) [1]=FR(R)
  long self_pid;

  lpw_node nodes[LPW_MAX_NODES];
  lpw_port ports[LPW_MAX_PORTS];
  lpw_client clients[LPW_MAX_CLIENTS];
  lpw_link links[LPW_MAX_LINKS];

  // SPSC ring of mono samples: graph thread produces, read() consumes.
  int16_t ring[LPW_RING_CAP];
  _Atomic size_t head;
  _Atomic size_t tail;
  _Atomic bool failed;
};

// ---- small lookup helpers (loop thread only) ----------------------------------------------------

static lpw_node *node_find(ccord_loopback_pw *p, uint32_t id)
{
  for (int i = 0; i < LPW_MAX_NODES; i++)
    if (p->nodes[i].used && p->nodes[i].id == id) return &p->nodes[i];
  return nullptr;
}
static lpw_node *node_alloc(ccord_loopback_pw *p, uint32_t id)
{
  for (int i = 0; i < LPW_MAX_NODES; i++) {
    if (!p->nodes[i].used) {
      memset(&p->nodes[i], 0, sizeof p->nodes[i]);
      p->nodes[i].used = true;
      p->nodes[i].id = id;
      p->nodes[i].pid = -1;
      p->nodes[i].app_pid = -1;
      p->nodes[i].client_id = -1;
      return &p->nodes[i];
    }
  }
  return nullptr;
}
static lpw_client *client_find(ccord_loopback_pw *p, uint32_t id)
{
  for (int i = 0; i < LPW_MAX_CLIENTS; i++)
    if (p->clients[i].used && p->clients[i].id == id) return &p->clients[i];
  return nullptr;
}

static long dict_pid(const struct spa_dict *d, const char *key)
{
  const char *s = spa_dict_lookup(d, key);
  if (!s || *s == '\0') return -1;
  char *end = nullptr;
  long v = strtol(s, &end, 10);
  if (end == s || v <= 0) return -1;
  return v;
}

// Which of our stereo input ports an app channel feeds: bit0 = L(FL), bit1 = R(FR).
// Stereo maps straight across; mono/center feed both so they survive the (L+R)/2
// fold at full level. Unknown/surround channels are dropped (front image is kept).
static unsigned channel_mask(const char *ch)
{
  if (!ch) return 0;
  if (strcmp(ch, "FL") == 0) return 1u;
  if (strcmp(ch, "FR") == 0) return 2u;
  if (strcmp(ch, "MONO") == 0 || strcmp(ch, "FC") == 0) return 3u;
  return 0;
}

// ---- linking ------------------------------------------------------------------------------------

static void link_destroy(lpw_link *l)
{
  if (!l->used) return;
  if (l->proxy) pw_proxy_destroy(l->proxy);
  l->used = false;
  l->proxy = nullptr;
}

static void node_unlink(ccord_loopback_pw *p, lpw_node *n)
{
  for (int i = 0; i < LPW_MAX_LINKS; i++)
    if (p->links[i].used && p->links[i].node_id == n->id) link_destroy(&p->links[i]);
}

static bool link_create(ccord_loopback_pw *p, uint32_t out_node, uint32_t out_port, uint32_t in_port)
{
  for (int i = 0; i < LPW_MAX_LINKS; i++) {
    if (p->links[i].used) continue;
    char a[24], b[24], c[24], d[24];
    snprintf(a, sizeof a, "%u", out_node);
    snprintf(b, sizeof b, "%u", out_port);
    snprintf(c, sizeof c, "%u", p->cap_node_id);
    snprintf(d, sizeof d, "%u", in_port);
    struct pw_properties *props = pw_properties_new(PW_KEY_LINK_OUTPUT_NODE, a, PW_KEY_LINK_OUTPUT_PORT, b,
                                                    PW_KEY_LINK_INPUT_NODE, c, PW_KEY_LINK_INPUT_PORT, d, nullptr);
    if (!props) return false;
    struct pw_proxy *pr =
        pw_core_create_object(p->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
    pw_properties_free(props);
    if (!pr) return false;
    p->links[i].used = true;
    p->links[i].proxy = pr;
    p->links[i].node_id = out_node;
    p->links[i].out_port = out_port;
    return true;
  }
  return false;
}

static bool cap_ready(const ccord_loopback_pw *p)
{
  return p->cap_node_id != PW_ID_ANY && p->cap_in_port[0] != PW_ID_ANY && p->cap_in_port[1] != PW_ID_ANY;
}

static bool link_exists(const ccord_loopback_pw *p, uint32_t node_id, uint32_t out_port)
{
  for (int i = 0; i < LPW_MAX_LINKS; i++)
    if (p->links[i].used && p->links[i].node_id == node_id && p->links[i].out_port == out_port) return true;
  return false;
}

// Link every output port of an allowed node into the matching stereo input(s).
// Idempotent per port: ports can appear incrementally, so re-running only adds
// links for ports we have not linked yet.
static void node_try_link(ccord_loopback_pw *p, lpw_node *n)
{
  if (!n->used || !n->allowed) return;
  if (!cap_ready(p)) return;
  for (int i = 0; i < LPW_MAX_PORTS; i++) {
    lpw_port *pt = &p->ports[i];
    if (!pt->used || pt->node_id != n->id || !pt->is_out) continue;
    if (link_exists(p, n->id, pt->id)) continue;
    unsigned mask = channel_mask(pt->channel);
    if (mask == 0) continue;
    if (mask & 1u) link_create(p, n->id, pt->id, p->cap_in_port[0]);
    if (mask & 2u) link_create(p, n->id, pt->id, p->cap_in_port[1]);
  }
}

static void nodes_try_link_all(ccord_loopback_pw *p)
{
  for (int i = 0; i < LPW_MAX_NODES; i++)
    if (p->nodes[i].used) node_try_link(p, &p->nodes[i]);
}

// Our capture's input ports may be registered before pw_stream_get_node_id() reports
// the node id (or vice versa). Scan the stored ports once the node id is known.
static void cap_port_scan(ccord_loopback_pw *p)
{
  if (p->cap_node_id == PW_ID_ANY) return;
  for (int i = 0; i < LPW_MAX_PORTS; i++) {
    lpw_port *pt = &p->ports[i];
    if (!pt->used || pt->node_id != p->cap_node_id || pt->is_out) continue;
    if (strcmp(pt->channel, "FL") == 0) p->cap_in_port[0] = pt->id;
    else if (strcmp(pt->channel, "FR") == 0) p->cap_in_port[1] = pt->id;
  }
  if (cap_ready(p)) nodes_try_link_all(p);
}

// ---- node listener (bound candidate nodes) ------------------------------------------------------

// Resolve a node's owning pid and (un)link it. Info events can carry partial
// property sets, so the authoritative application.process.id is remembered on the
// node/client and never downgraded. For pipewire-pulse clients pipewire.sec.pid is
// the *daemon*, not the app, so it is only a fallback for native clients.
static void node_resolve(ccord_loopback_pw *p, lpw_node *n)
{
  long pid = n->app_pid;
  if (pid <= 0 && n->client_id >= 0) {
    lpw_client *c = client_find(p, (uint32_t)n->client_id);
    if (c) {
      if (c->app_pid > 0) pid = c->app_pid;
      else if (!c->is_pulse) pid = c->sec_pid;
    }
  }
  if (pid <= 0) return;   // no authoritative owner yet; keep the previous decision
  n->pid = pid;
  n->allowed = ccord_loopback_pw_node_allowed("Stream/Output/Audio", pid, p->self_pid);
  if (n->allowed) {
    node_try_link(p, n);
  } else {
    node_unlink(p, n);   // owned by us (or unknown): never capture it
  }
}

static void node_info(void *ud, const struct pw_node_info *info)
{
  ccord_loopback_pw *p = ud;
  lpw_node *n = node_find(p, info->id);
  if (!n || !info->props) return;
  const char *cl = spa_dict_lookup(info->props, "client.id");
  if (cl) n->client_id = strtol(cl, nullptr, 10);
  long app_pid = dict_pid(info->props, "application.process.id");
  if (app_pid > 0) n->app_pid = app_pid;   // remember; partial updates must not erase it
  node_resolve(p, n);
}

static const struct pw_node_events k_node_events = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = node_info,
};

static void client_info(void *ud, const struct pw_client_info *info)
{
  ccord_loopback_pw *p = ud;
  lpw_client *c = client_find(p, info->id);
  if (!c || !info->props) return;
  long app_pid = dict_pid(info->props, "application.process.id");
  if (app_pid > 0) c->app_pid = app_pid;
  // Re-resolve any node owned by this client now that its pid is known.
  for (int i = 0; i < LPW_MAX_NODES; i++)
    if (p->nodes[i].used && p->nodes[i].client_id == (long)info->id) node_resolve(p, &p->nodes[i]);
}

static const struct pw_client_events k_client_events = {
    .version = PW_VERSION_CLIENT_EVENTS,
    .info = client_info,
};

// ---- registry listener --------------------------------------------------------------------------

static void reg_global(void *ud, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
                       const struct spa_dict *props)
{
  ccord_loopback_pw *p = ud;
  (void)permissions;
  (void)version;
  if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    const char *cls = spa_dict_lookup(props, "media.class");
    if (!cls || strcmp(cls, "Stream/Output/Audio") != 0) return;   // only playback streams
    if (node_find(p, id)) return;
    lpw_node *n = node_alloc(p, id);
    if (!n) return;
    n->proxy = pw_registry_bind(p->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
    if (n->proxy) pw_node_add_listener((struct pw_node *)n->proxy, &n->hook, &k_node_events, p);
  } else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
    const char *nid = spa_dict_lookup(props, "node.id");
    const char *dir = spa_dict_lookup(props, "port.direction");
    if (!nid || !dir) return;
    uint32_t node_id = (uint32_t)strtoul(nid, nullptr, 10);
    bool is_out = strcmp(dir, "out") == 0;
    for (int i = 0; i < LPW_MAX_PORTS; i++) {
      if (p->ports[i].used) continue;
      memset(&p->ports[i], 0, sizeof p->ports[i]);
      p->ports[i].used = true;
      p->ports[i].id = id;
      p->ports[i].node_id = node_id;
      p->ports[i].is_out = is_out;
      const char *ch = spa_dict_lookup(props, "audio.channel");
      if (ch) snprintf(p->ports[i].channel, sizeof p->ports[i].channel, "%s", ch);
      break;
    }
    cap_port_scan(p);   // may be one of our own capture input ports
    lpw_node *n = node_find(p, node_id);
    if (n && is_out) node_try_link(p, n);
  } else if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
    for (int i = 0; i < LPW_MAX_CLIENTS; i++) {
      if (p->clients[i].used) continue;
      p->clients[i].used = true;
      p->clients[i].id = id;
      p->clients[i].sec_pid = dict_pid(props, "pipewire.sec.pid");
      p->clients[i].app_pid = -1;
      const char *api = spa_dict_lookup(props, "client.api");
      p->clients[i].is_pulse = api && strcmp(api, "pipewire-pulse") == 0;
      p->clients[i].proxy = pw_registry_bind(p->registry, id, PW_TYPE_INTERFACE_Client, PW_VERSION_CLIENT, 0);
      if (p->clients[i].proxy)
        pw_client_add_listener((struct pw_client *)p->clients[i].proxy, &p->clients[i].hook, &k_client_events, p);
      break;
    }
  }
}

static void reg_global_remove(void *ud, uint32_t id)
{
  ccord_loopback_pw *p = ud;
  lpw_node *n = node_find(p, id);
  if (n) {
    node_unlink(p, n);
    if (n->proxy) {
      pw_proxy_destroy(n->proxy);
      n->proxy = nullptr;
    }
    n->used = false;
    return;
  }
  for (int i = 0; i < LPW_MAX_PORTS; i++) {
    if (p->ports[i].used && p->ports[i].id == id) {
      uint32_t node_id = p->ports[i].node_id;
      p->ports[i].used = false;
      for (int j = 0; j < LPW_MAX_LINKS; j++)
        if (p->links[j].used && p->links[j].node_id == node_id && p->links[j].out_port == id)
          link_destroy(&p->links[j]);
      return;
    }
  }
  for (int i = 0; i < LPW_MAX_CLIENTS; i++) {
    if (p->clients[i].used && p->clients[i].id == id) {
      if (p->clients[i].proxy) pw_proxy_destroy(p->clients[i].proxy);
      p->clients[i].used = false;
      return;
    }
  }
}

static const struct pw_registry_events k_registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = reg_global,
    .global_remove = reg_global_remove,
};

// ---- core / stream listeners --------------------------------------------------------------------

static void core_error(void *ud, uint32_t id, int seq, int res, const char *message)
{
  ccord_loopback_pw *p = ud;
  (void)id;
  (void)seq;
  (void)res;
  CCORD_LOG_D("loopback-pw: core error: %s", message ? message : "?");
  atomic_store(&p->failed, true);
}
static void core_done(void *ud, uint32_t id, int seq)
{
  ccord_loopback_pw *p = ud;
  (void)id;
  (void)seq;
  pw_thread_loop_signal(p->loop, false);
}
static const struct pw_core_events k_core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .error = core_error,
    .done = core_done,
};

static void stream_state(void *ud, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
  ccord_loopback_pw *p = ud;
  (void)old;
  if (state == PW_STREAM_STATE_ERROR) {
    CCORD_LOG_D("loopback-pw: stream error: %s", error ? error : "?");
    atomic_store(&p->failed, true);
  }
}

static void stream_process(void *ud)
{
  ccord_loopback_pw *p = ud;
  struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
  if (!b) return;
  struct spa_buffer *buf = b->buffer;
  if (buf->n_datas >= 1 && buf->datas[0].data) {
    struct spa_data *d = &buf->datas[0];
    struct spa_chunk *ch = d->chunk;
    if (ch->size >= 4 && ch->offset <= d->maxsize && ch->size <= d->maxsize - ch->offset) {
      const int16_t *s = (const int16_t *)(const void *)((const uint8_t *)d->data + ch->offset);
      size_t frames = ch->size / 4u;   // interleaved stereo S16 = 4 bytes/frame
      size_t h = atomic_load_explicit(&p->head, memory_order_relaxed);
      size_t t = atomic_load_explicit(&p->tail, memory_order_acquire);
      for (size_t i = 0; i < frames; i++) {
        size_t next = (h + 1u) & LPW_RING_MASK;
        if (next == t) break;   // full: drop newest
        int32_t l = s[2 * i], r = s[2 * i + 1];
        p->ring[h] = (int16_t)((l + r) / 2);
        h = next;
      }
      atomic_store_explicit(&p->head, h, memory_order_release);
    }
  }
  pw_stream_queue_buffer(p->stream, b);
}

static const struct pw_stream_events k_stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state,
    .process = stream_process,
};

// ---- open / teardown ----------------------------------------------------------------------------

static void lpw_teardown(ccord_loopback_pw *p)
{
  if (p->loop) pw_thread_loop_lock(p->loop);
  if (p->stream) {
    pw_stream_disconnect(p->stream);
    pw_stream_destroy(p->stream);
    p->stream = nullptr;
  }
  for (int i = 0; i < LPW_MAX_LINKS; i++)
    if (p->links[i].used && p->links[i].proxy) pw_proxy_destroy(p->links[i].proxy);
  for (int i = 0; i < LPW_MAX_NODES; i++)
    if (p->nodes[i].used && p->nodes[i].proxy) pw_proxy_destroy(p->nodes[i].proxy);
  for (int i = 0; i < LPW_MAX_CLIENTS; i++)
    if (p->clients[i].used && p->clients[i].proxy) pw_proxy_destroy(p->clients[i].proxy);
  if (p->core) {
    pw_core_disconnect(p->core);
    p->core = nullptr;
  }
  if (p->context) {
    pw_context_destroy(p->context);
    p->context = nullptr;
  }
  if (p->loop) {
    pw_thread_loop_unlock(p->loop);
    pw_thread_loop_stop(p->loop);
    pw_thread_loop_destroy(p->loop);
    p->loop = nullptr;
  }
  if (p->inited) {
    pw_deinit();
    p->inited = false;
  }
}

ccord_loopback_pw *ccord_loopback_pw_open(void)
{
  ccord_loopback_pw *p = ccord_calloc(1, sizeof *p);
  if (!p) return nullptr;
  p->cap_node_id = PW_ID_ANY;
  p->cap_in_port[0] = PW_ID_ANY;
  p->cap_in_port[1] = PW_ID_ANY;
  p->self_pid = (long)getpid();
  atomic_init(&p->head, 0);
  atomic_init(&p->tail, 0);
  atomic_init(&p->failed, false);

  pw_init(nullptr, nullptr);
  p->inited = true;
  p->loop = pw_thread_loop_new("ccord-loopback", nullptr);
  if (!p->loop) goto fail;
  p->context = pw_context_new(pw_thread_loop_get_loop(p->loop), nullptr, 0);
  if (!p->context) goto fail;
  if (pw_thread_loop_start(p->loop) < 0) goto fail;

  pw_thread_loop_lock(p->loop);
  p->core = pw_context_connect(p->context, nullptr, 0);
  if (!p->core) {
    pw_thread_loop_unlock(p->loop);
    goto fail;
  }
  pw_core_add_listener(p->core, &p->core_hook, &k_core_events, p);
  p->registry = pw_core_get_registry(p->core, PW_VERSION_REGISTRY, 0);
  if (!p->registry) {
    pw_thread_loop_unlock(p->loop);
    goto fail;
  }
  pw_registry_add_listener(p->registry, &p->registry_hook, &k_registry_events, p);

  struct pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
      PW_KEY_NODE_NAME, "ccord-loopback", PW_KEY_NODE_DESCRIPTION, "C-Cord Go Live audio",
      PW_KEY_NODE_AUTOCONNECT, "false", PW_KEY_NODE_LATENCY, "960/48000", nullptr);
  p->stream = pw_stream_new(p->core, "ccord-loopback", props);
  if (!p->stream) {
    pw_thread_loop_unlock(p->loop);
    goto fail;
  }
  pw_stream_add_listener(p->stream, &p->stream_hook, &k_stream_events, p);
  uint8_t pod_buf[512];
  struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(pod_buf, sizeof pod_buf);
  struct spa_audio_info_raw raw = {.format = SPA_AUDIO_FORMAT_S16_LE,
                                   .rate = CCORD_LOOPBACK_RATE,
                                   .channels = 2,
                                   .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}};
  const struct spa_pod *params[1] = {spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &raw)};
  if (pw_stream_connect(p->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                        PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
    pw_thread_loop_unlock(p->loop);
    goto fail;
  }
  // Give the registry a moment to populate (clients, nodes, our own capture ports).
  // Linking continues asynchronously via the registry events after we return. Each
  // sync is woken by the core done event, so this settles in a few milliseconds.
  for (int i = 0; i < 40 && !cap_ready(p); i++) {
    p->cap_node_id = pw_stream_get_node_id(p->stream);
    cap_port_scan(p);
    if (cap_ready(p)) break;
    pw_core_sync(p->core, PW_ID_CORE, 0);
    pw_thread_loop_timed_wait(p->loop, 1);
  }
  p->cap_node_id = pw_stream_get_node_id(p->stream);
  cap_port_scan(p);
  pw_thread_loop_unlock(p->loop);
  return p;

fail:
  lpw_teardown(p);
  ccord_free(p);
  return nullptr;
}

size_t ccord_loopback_pw_read(ccord_loopback_pw *p, int16_t *pcm, size_t max_samples)
{
  if (!p || !pcm || max_samples == 0) return 0;
  if (atomic_load(&p->failed)) return 0;
  size_t t = atomic_load_explicit(&p->tail, memory_order_relaxed);
  size_t h = atomic_load_explicit(&p->head, memory_order_acquire);
  size_t avail = (h - t) & LPW_RING_MASK;
  size_t n = avail < max_samples ? avail : max_samples;
  for (size_t i = 0; i < n; i++) pcm[i] = p->ring[(t + i) & LPW_RING_MASK];
  if (n > 0) atomic_store_explicit(&p->tail, (t + n) & LPW_RING_MASK, memory_order_release);
  if (n == 0) {
    // Nothing linked / idle: the follower stream is not running. Emit silence so
    // the outbound Go Live audio keeps ticking instead of stalling.
    n = max_samples < CCORD_LOOPBACK_FRAME_SAMPLES ? max_samples : CCORD_LOOPBACK_FRAME_SAMPLES;
    memset(pcm, 0, n * sizeof(int16_t));
  }
  return n;
}

bool ccord_loopback_pw_failed(const ccord_loopback_pw *p)
{
  return !p || atomic_load(&p->failed);
}

void ccord_loopback_pw_close(ccord_loopback_pw *p)
{
  if (!p) return;
  lpw_teardown(p);
  ccord_free(p);
}

#else   // !CCORD_HAVE_PIPEWIRE

ccord_loopback_pw *ccord_loopback_pw_open(void) { return nullptr; }
size_t ccord_loopback_pw_read(ccord_loopback_pw *p, int16_t *pcm, size_t max_samples)
{
  (void)p;
  (void)pcm;
  (void)max_samples;
  return 0;
}
bool ccord_loopback_pw_failed(const ccord_loopback_pw *p) { return !p; }
void ccord_loopback_pw_close(ccord_loopback_pw *p) { (void)p; }

#endif   // CCORD_HAVE_PIPEWIRE
