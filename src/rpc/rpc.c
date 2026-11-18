// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE   // accept4, plus the POSIX socket/stat declarations that -std=c2x hides
#include <ccord/rpc/rpc.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

#define RPC_VERSION 1
#define RBUF_CAP (8u + CCORD_RPC_FRAME_MAX_PAYLOAD)   // one maximal frame always fits, so a full buffer is never stuck
#define READ_ROUNDS 8u                                // bounded work per connection per poll
#define ACCEPTS_PER_POLL 16u
#define CLIENT_ID_MAX 20u                             // a u64 snowflake has at most 20 digits
#define ECHO_MAX 64u                                  // longest cmd echoed back in an ERROR reply
#define ACTIVITY_JSON_CAP 32768u                      // 4 activities of 17 fields, worst-case escaped, fit well below this
#define PID_CHECK_INTERVAL_MS 1000u                   // how often to check tracked PIDs for exit while socket stays open

struct ccord_rpc_conn {
  int fd;
  uint32_t id;
  bool ready;                                         // handshake done and READY sent
  bool has_activity;                                  // true while an activity is active on this connection
  pid_t peer_pid;                                     // SO_PEERCRED pid at accept() (0 if unavailable)
  pid_t tracked_pid;                                  // live Linux PID monitored for exit (0 = none)
  uint64_t accepted_ms;
  uint64_t last_pid_check_ms;
  char client_id[CLIENT_ID_MAX + 4u];
  size_t rlen;
  char rbuf[RBUF_CAP];
};

struct ccord_rpc_server {
  ccord_rpc_hooks hooks;
  void *ctx;
  int fd;                                             // non-blocking listener
  char path[sizeof(((struct sockaddr_un *)nullptr)->sun_path)];
  ccord_rpc_conn *conns[CCORD_RPC_MAX_CONNECTIONS];
  size_t nconns;
  uint32_t next_id;
  size_t work;                                        // accepts + frames handled by the current poll
  uint64_t now_ms;                                    // clock of the current poll
  ccord_arena scratch;                                // JSON parse arena, reset after every frame
};

// ---------------------------------------------------------------- listener

static bool fill_addr(const char *path, struct sockaddr_un *out)
{
  if (strlen(path) >= sizeof out->sun_path) return false;
  memset(out, 0, sizeof *out);
  out->sun_family = AF_UNIX;
  memcpy(out->sun_path, path, strlen(path));
  return true;
}

// A live listener answers connect(); a stale socket file refuses it. The probe is non-blocking so a peer whose accept
// queue is full (EAGAIN) counts as alive instead of hanging us.
static bool probe_alive(const struct sockaddr_un *addr)
{
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return true;   // cannot tell: assume it is live rather than unlink someone's socket
  bool alive = connect(fd, (const struct sockaddr *)addr, sizeof *addr) == 0 || errno == EAGAIN || errno == EINPROGRESS;
  close(fd);
  return alive;
}

// Only a socket we own is ever unlinked; a regular file or another user's node in the slot is left alone.
static bool unlink_stale(const char *path)
{
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT;   // vanished meanwhile: the slot is free to bind
  if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) return false;
  return unlink(path) == 0;
}

typedef enum bind_result { BIND_OK, BIND_BUSY, BIND_FAILED } bind_result;

static bind_result bind_slot(const char *path, int *out_fd)
{
  struct sockaddr_un addr;
  if (!fill_addr(path, &addr)) return BIND_FAILED;
  for (int attempt = 0; attempt < 2; attempt++) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return BIND_FAILED;
    (void)fchmod(fd, 0600);
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    int bind_errno = errno;
    if (rc == 0) {
      if (listen(fd, (int)CCORD_RPC_MAX_CONNECTIONS) != 0) {
        close(fd);
        (void)unlink(path);
        return BIND_FAILED;
      }
      *out_fd = fd;
      return BIND_OK;
    }
    close(fd);
    if (bind_errno != EADDRINUSE) return BIND_FAILED;
    if (probe_alive(&addr) || !unlink_stale(path)) return BIND_BUSY;
    // stale socket removed: loop once more and bind it
  }
  return BIND_BUSY;
}

static const char *env_dir(const char *name)
{
  const char *v = getenv(name);
  if (!v || !v[0]) return nullptr;
  if (strcmp(name, "TMPDIR") == 0) {
    struct stat st;
    if (lstat(v, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
      return nullptr;
    }
  }
  return v;
}

ccord_rpc_server *ccord_rpc_server_create(const ccord_rpc_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  const char *dir = (cfg && cfg->dir && cfg->dir[0]) ? cfg->dir : env_dir("XDG_RUNTIME_DIR");
  if (!dir) dir = env_dir("TMPDIR");
  if (!dir) {
    (void)ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "rpc: no socket directory (set XDG_RUNTIME_DIR or TMPDIR)");
    return nullptr;
  }
  ccord_rpc_server *s = ccord_calloc(1, sizeof *s);
  if (!s) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "rpc: out of memory");
    return nullptr;
  }
  s->fd = -1;
  for (unsigned slot = 0; slot < CCORD_RPC_SLOTS; slot++) {
    char path[sizeof s->path];
    int n = snprintf(path, sizeof path, "%s/discord-ipc-%u", dir, slot);
    if (n < 0 || (size_t)n >= sizeof path) {
      ccord_free(s);
      (void)ccord_error_set(err, CCORD_ERR_INVALID, "rpc: socket path too long");
      return nullptr;
    }
    bind_result r = bind_slot(path, &s->fd);
    if (r == BIND_OK) {
      memcpy(s->path, path, (size_t)n + 1u);
      break;
    }
    if (r == BIND_FAILED) {
      ccord_free(s);
      (void)ccord_error_set(err, CCORD_ERR_IO, "rpc: cannot bind a discord-ipc socket in the socket directory");
      return nullptr;
    }
  }
  if (s->fd < 0) {
    ccord_free(s);
    (void)ccord_error_set(err, CCORD_ERR_IO, "rpc: all discord-ipc-0..9 slots are in use");
    return nullptr;
  }
  if (cfg) {
    s->hooks = cfg->hooks;
    s->ctx = cfg->ctx;
  }
  ccord_arena_init(&s->scratch, 0);
  return s;
}

const char *ccord_rpc_server_path(const ccord_rpc_server *s)
{
  return s ? s->path : "";
}

size_t ccord_rpc_server_conn_count(const ccord_rpc_server *s)
{
  return s ? s->nconns : 0;
}

uint32_t ccord_rpc_conn_id(const ccord_rpc_conn *conn)
{
  return conn ? conn->id : 0;
}

const char *ccord_rpc_conn_client_id(const ccord_rpc_conn *conn)
{
  return conn ? conn->client_id : "";
}

// ---------------------------------------------------------------- writing

// Replies are tiny next to the socket buffer, so a write that cannot complete means the client is not reading: the
// caller drops the connection instead of queueing unbounded output.
static bool send_frame(int fd, uint32_t op, const char *payload, size_t len)
{
  if (len > CCORD_RPC_FRAME_MAX_PAYLOAD) return false;
  size_t total = 8u + len;
  char *frame = ccord_malloc(total);
  if (!frame) return false;
  bool ok = ccord_rpc_frame_encode(frame, total, op, payload, len) == CCORD_OK;
  size_t off = 0;
  while (ok && off < total) {
    ssize_t n = send(fd, frame + off, total - off, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      off += (size_t)n;
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      ok = false;
    }
  }
  ccord_free(frame);
  return ok;
}

static bool send_close(int fd, int code, const char *message)
{
  char json[160];
  int n = snprintf(json, sizeof json, "{\"code\":%d,\"message\":\"%s\"}", code, message);
  return n > 0 && (size_t)n < sizeof json && send_frame(fd, OP_CLOSE, json, (size_t)n);
}

static bool append_lit(ccord_buf *b, const char *s)
{
  return ccord_buf_append_str(b, s) == CCORD_OK;
}

// Appends `s` as a JSON string, or `null` for a null pointer.
static bool append_jstr(ccord_buf *b, const char *s)
{
  if (!s) return append_lit(b, "null");
  ccord_jw w;
  ccord_jw_init(&w, b);
  ccord_jw_string(&w, s, strlen(s));
  return ccord_jw_ok(&w);
}

// {"cmd":..,"data":<raw JSON>,"evt":..,"nonce":..} as an op FRAME. `data` is already-valid JSON text.
static bool send_envelope(ccord_rpc_conn *c, const char *cmd, const char *nonce, const char *evt, const char *data)
{
  ccord_buf b;
  ccord_buf_init(&b, CCORD_RPC_FRAME_MAX_PAYLOAD);
  bool ok = append_lit(&b, "{\"cmd\":") && append_jstr(&b, cmd) && append_lit(&b, ",\"data\":") &&
            append_lit(&b, data) && append_lit(&b, ",\"evt\":") && append_jstr(&b, evt) &&
            append_lit(&b, ",\"nonce\":") && append_jstr(&b, nonce) && append_lit(&b, "}") && !b.failed;
  ok = ok && send_frame(c->fd, OP_FRAME, b.data, b.len);
  ccord_buf_free(&b);
  return ok;
}

// evt ERROR with data {code: 4000, message}. `cmd` is echoed only when short enough to be a command name.
static bool send_error(ccord_rpc_conn *c, const char *cmd, const char *nonce, const char *message)
{
  if (cmd && strlen(cmd) > ECHO_MAX) cmd = nullptr;
  ccord_buf d;
  ccord_buf_init(&d, 1024);
  ccord_jw w;
  ccord_jw_init(&w, &d);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "code");
  ccord_jw_int(&w, CCORD_RPC_ERROR_CODE);
  ccord_jw_key(&w, "message");
  ccord_jw_cstr(&w, message);
  ccord_jw_end_object(&w);
  bool ok = ccord_jw_ok(&w) && send_envelope(c, cmd, nonce, "ERROR", ccord_buf_cstr(&d));
  ccord_buf_free(&d);
  return ok;
}

// ---------------------------------------------------------------- frame handlers

static const char *json_string_field(const ccord_json *obj, const char *key)
{
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_string(v, nullptr) : nullptr;
}

static bool valid_client_id(const char *s)
{
  if (!s) return false;
  size_t n = strlen(s);
  if (n == 0 || n > CLIENT_ID_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

static void jw_optional(ccord_jw *w, const char *key, const char *s)
{
  ccord_jw_key(w, key);
  if (s && s[0]) ccord_jw_cstr(w, s); else ccord_jw_null(w);
}

// data of the READY dispatch: {v, config, user}. `user` null renders as `user: null`.
static bool build_ready_data(ccord_buf *b, const ccord_rpc_user *user)
{
  ccord_jw w;
  ccord_jw_init(&w, b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "v");
  ccord_jw_int(&w, RPC_VERSION);
  ccord_jw_key(&w, "config");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "cdn_host");
  ccord_jw_cstr(&w, "cdn.discordapp.com");
  ccord_jw_key(&w, "api_endpoint");
  ccord_jw_cstr(&w, "//discord.com/api");
  ccord_jw_key(&w, "environment");
  ccord_jw_cstr(&w, "production");
  ccord_jw_end_object(&w);
  ccord_jw_key(&w, "user");
  if (user) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id");
    ccord_jw_cstr(&w, user->id);
    ccord_jw_key(&w, "username");
    ccord_jw_cstr(&w, user->username);
    ccord_jw_key(&w, "discriminator");
    ccord_jw_cstr(&w, user->discriminator[0] ? user->discriminator : "0");
    jw_optional(&w, "global_name", user->global_name);
    jw_optional(&w, "avatar", user->avatar);
    ccord_jw_key(&w, "bot");
    ccord_jw_bool(&w, user->bot);
    ccord_jw_end_object(&w);
  } else {
    ccord_jw_null(&w);
  }
  ccord_jw_end_object(&w);
  return ccord_jw_ok(&w);
}

static bool handle_handshake(ccord_rpc_server *s, ccord_rpc_conn *c, const char *payload, size_t plen)
{
  ccord_error perr;
  const ccord_json *j = ccord_json_parse(&s->scratch, payload, plen, nullptr, &perr);
  if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT) {
    (void)send_close(c->fd, CCORD_RPC_CLOSE_INVALID_ENCODING, "Invalid encoding");
    return false;
  }
  int64_t version = 0;
  const ccord_json *v = ccord_json_get(j, "v");
  if (!v || !ccord_json_int64(v, &version) || version != RPC_VERSION) {
    (void)send_close(c->fd, CCORD_RPC_CLOSE_INVALID_VERSION, "Invalid version");
    return false;
  }
  const char *client_id = json_string_field(j, "client_id");
  if (!valid_client_id(client_id)) {
    (void)send_close(c->fd, CCORD_RPC_CLOSE_INVALID_CLIENT_ID, "Invalid client id");
    return false;
  }
  memcpy(c->client_id, client_id, strlen(client_id) + 1u);

  ccord_rpc_user user = {0};
  bool have_user = s->hooks.get_user && s->hooks.get_user(s->ctx, &user) && user.id[0];
  ccord_buf data;
  ccord_buf_init(&data, CCORD_RPC_FRAME_MAX_PAYLOAD);
  bool ok = build_ready_data(&data, have_user ? &user : nullptr) &&
            send_envelope(c, "DISPATCH", nullptr, "READY", ccord_buf_cstr(&data));
  ccord_buf_free(&data);
  if (!ok) return false;
  c->ready = true;   // set only after READY went out, so on_ready/on_disconnect stay paired
  if (s->hooks.on_ready) s->hooks.on_ready(s->ctx, c);
  return true;
}

static bool is_pid_alive(pid_t pid)
{
  if (pid <= 0) return false;
  if (kill(pid, 0) < 0 && errno == ESRCH) return false;
  char path[64];
  snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno != ENOENT && errno != ESRCH;
  char buf[512];
  size_t got = 0;
  while (got < sizeof buf - 1) {
    ssize_t r = read(fd, buf + got, sizeof buf - 1 - got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    got += (size_t)r;
  }
  close(fd);
  buf[got] = '\0';
  const char *p = strrchr(buf, ')');
  if (!p) return true;
  p++;
  while (*p == ' ') p++;
  return *p != 'Z' && *p != 'X' && *p != 'x';
}

static bool cmd_set_activity(ccord_rpc_server *s, ccord_rpc_conn *c, const ccord_json *j, const char *cmd,
                             const char *nonce)
{
  const ccord_json *args = ccord_json_get(j, "args");
  if (!args || ccord_json_type_of(args) != CCORD_JSON_OBJECT) return send_error(c, cmd, nonce, "Invalid arguments");
  const ccord_json *activity = ccord_json_get(args, "activity");
  if (!activity || ccord_json_type_of(activity) == CCORD_JSON_NULL) {
    c->has_activity = false;
    c->tracked_pid = 0;
    if (s->hooks.on_activity) s->hooks.on_activity(s->ctx, c, nullptr);
    return send_envelope(c, cmd, nonce, nullptr, "null");
  }
  ccord_rpc_activity act;
  ccord_error aerr;
  if (ccord_rpc_activity_parse(activity, &act, &aerr) != CCORD_OK) return send_error(c, cmd, nonce, aerr.message);

  // The reply echoes the activity object. The gateway serializer writes a one-element array; drop its brackets.
  char *echo = ccord_malloc(ACTIVITY_JSON_CAP);
  if (!echo) return false;
  size_t n = 0;
  bool ok = false;
  if (ccord_rpc_activity_to_gateway_json(&act, 1, echo, ACTIVITY_JSON_CAP) == CCORD_OK) {
    n = strlen(echo);
    ok = n >= 2 && echo[0] == '[' && echo[n - 1] == ']';
  }
  if (!ok) {
    ccord_free(echo);
    return send_error(c, cmd, nonce, "Activity too large");
  }
  echo[n - 1] = '\0';

  // Resolve PID for liveness tracking: prefer args.pid if > 0, else SO_PEERCRED fallback. Only arm /proc/<pid>
  // monitoring if the candidate PID actually belongs to a live Linux process right now (e.g. skip Windows PIDs).
  int64_t arg_pid = 0;
  const ccord_json *pid_val = ccord_json_get(args, "pid");
  if (pid_val) (void)ccord_json_int64(pid_val, &arg_pid);
  pid_t candidate = 0;
  if (arg_pid > 0 && arg_pid <= INT32_MAX) {
    candidate = (pid_t)arg_pid;
  } else if (arg_pid <= 0 && c->peer_pid > 0) {
    candidate = c->peer_pid;
  }
  c->tracked_pid = (candidate > 0 && is_pid_alive(candidate)) ? candidate : 0;
  c->has_activity = true;
  c->last_pid_check_ms = s->now_ms;

  if (s->hooks.on_activity) s->hooks.on_activity(s->ctx, c, &act);
  ok = send_envelope(c, cmd, nonce, nullptr, echo + 1);
  ccord_free(echo);
  return ok;
}

// SUBSCRIBE / UNSUBSCRIBE are acked with the requested event name; no event is ever dispatched.
static bool cmd_subscription(ccord_rpc_conn *c, const ccord_json *j, const char *cmd, const char *nonce)
{
  const char *evt = json_string_field(j, "evt");
  ccord_buf d;
  ccord_buf_init(&d, 1024);
  ccord_jw w;
  ccord_jw_init(&w, &d);
  ccord_jw_begin_object(&w);
  if (evt && strlen(evt) <= ECHO_MAX) {
    ccord_jw_key(&w, "evt");
    ccord_jw_cstr(&w, evt);
  }
  ccord_jw_end_object(&w);
  bool ok = ccord_jw_ok(&w) && send_envelope(c, cmd, nonce, nullptr, ccord_buf_cstr(&d));
  ccord_buf_free(&d);
  return ok;
}

static bool handle_command(ccord_rpc_server *s, ccord_rpc_conn *c, const char *payload, size_t plen)
{
  ccord_error perr;
  const ccord_json *j = ccord_json_parse(&s->scratch, payload, plen, nullptr, &perr);
  if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT) return send_error(c, nullptr, nullptr, "Invalid payload");
  const char *cmd = json_string_field(j, "cmd");
  const char *nonce = json_string_field(j, "nonce");
  if (!cmd) return send_error(c, nullptr, nonce, "Invalid payload");
  if (strcmp(cmd, "SET_ACTIVITY") == 0) return cmd_set_activity(s, c, j, cmd, nonce);
  if (strcmp(cmd, "SUBSCRIBE") == 0 || strcmp(cmd, "UNSUBSCRIBE") == 0) return cmd_subscription(c, j, cmd, nonce);
  return send_error(c, cmd, nonce, "Unsupported command");
}

// Returns false when the connection must be dropped.
static bool handle_frame(ccord_rpc_server *s, ccord_rpc_conn *c, uint32_t op, const char *payload, size_t plen)
{
  switch (op) {
    case OP_HANDSHAKE:
      if (c->ready) {
        (void)send_close(c->fd, CCORD_RPC_CLOSE_ABNORMAL, "Already handshaken");
        return false;
      }
      return handle_handshake(s, c, payload, plen);
    case OP_FRAME:
      if (!c->ready) {
        (void)send_close(c->fd, CCORD_RPC_CLOSE_ABNORMAL, "Handshake required");
        return false;
      }
      return handle_command(s, c, payload, plen);
    case OP_PING:
      return send_frame(c->fd, OP_PONG, payload, plen);
    case OP_PONG:
      return true;
    case OP_CLOSE:
    default:
      return false;
  }
}

// Decodes and handles every complete frame in the read buffer. Returns false when the connection must be dropped.
static bool drain_frames(ccord_rpc_server *s, ccord_rpc_conn *c)
{
  size_t off = 0;
  bool alive = true;
  while (alive && off < c->rlen) {
    uint32_t op = 0;
    const char *payload = nullptr;
    size_t plen = 0, used = 0;
    ccord_rpc_frame_result r = ccord_rpc_frame_decode(c->rbuf + off, c->rlen - off, &op, &payload, &plen, &used);
    if (r == CCORD_RPC_FRAME_NEED_MORE) break;
    if (r == CCORD_RPC_FRAME_BAD) {
      (void)send_close(c->fd, CCORD_RPC_CLOSE_INVALID_ENCODING, "Invalid frame");
      return false;
    }
    s->work++;
    alive = handle_frame(s, c, op, payload, plen);
    ccord_arena_reset(&s->scratch);
    off += used;
  }
  if (!alive) return false;
  if (off > 0) {
    memmove(c->rbuf, c->rbuf + off, c->rlen - off);
    c->rlen -= off;
  }
  return true;
}

// Reads what the client sent and handles it, then applies the handshake timeout. False = drop the connection.
static bool service_conn(ccord_rpc_server *s, ccord_rpc_conn *c, uint64_t now_ms)
{
  for (unsigned round = 0; round < READ_ROUNDS; round++) {
    size_t room = sizeof c->rbuf - c->rlen;   // never 0: complete frames are consumed, and a frame fits the buffer
    ssize_t n = recv(c->fd, c->rbuf + c->rlen, room, MSG_DONTWAIT);
    if (n > 0) {
      c->rlen += (size_t)n;
      if (!drain_frames(s, c)) return false;
      if ((size_t)n < room) break;
      continue;
    }
    if (n == 0) return false;                 // EOF
    if (errno == EINTR) continue;
    if (errno == EAGAIN) break;   // EWOULDBLOCK is the same value on Linux
    return false;
  }
  if (!c->ready && now_ms > c->accepted_ms && now_ms - c->accepted_ms >= CCORD_RPC_HANDSHAKE_TIMEOUT_MS) {
    (void)send_close(c->fd, CCORD_RPC_CLOSE_ABNORMAL, "Handshake timeout");
    return false;
  }
  return true;
}

static void release_conn(ccord_rpc_server *s, ccord_rpc_conn *c)
{
  if (c->ready && s->hooks.on_disconnect) s->hooks.on_disconnect(s->ctx, c);
  close(c->fd);
  ccord_free(c);
}

static void accept_clients(ccord_rpc_server *s, uint64_t now_ms)
{
  for (unsigned i = 0; i < ACCEPTS_PER_POLL; i++) {
    int fd = accept4(s->fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      return;   // EAGAIN: nothing pending
    }
    s->work++;
    ccord_rpc_conn *c = s->nconns < CCORD_RPC_MAX_CONNECTIONS ? ccord_calloc(1, sizeof *c) : nullptr;
    if (!c) {
      (void)send_close(fd, CCORD_RPC_CLOSE_ABNORMAL, s->nconns < CCORD_RPC_MAX_CONNECTIONS ? "Out of memory"
                                                                                           : "Too many connections");
      close(fd);
      continue;
    }
    c->fd = fd;
    c->id = ++s->next_id;
    c->accepted_ms = now_ms;
#ifdef SO_PEERCRED
    struct ucred cred = {0};
    socklen_t clen = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 || cred.uid != geteuid()) {
      close(fd);
      continue;
    }
    if (cred.pid > 0) c->peer_pid = cred.pid;
#else
    close(fd);
    continue;
#endif
    s->conns[s->nconns++] = c;
  }
}

size_t ccord_rpc_server_poll(ccord_rpc_server *s, uint64_t now_ms)
{
  if (!s) return 0;
  s->work = 0;
  s->now_ms = now_ms;
  accept_clients(s, now_ms);
  for (size_t i = 0; i < s->nconns;) {
    ccord_rpc_conn *c = s->conns[i];
    if (service_conn(s, c, now_ms)) {
      if (c->has_activity && c->tracked_pid > 0 && now_ms >= c->last_pid_check_ms &&
          now_ms - c->last_pid_check_ms >= PID_CHECK_INTERVAL_MS) {
        c->last_pid_check_ms = now_ms;
        if (!is_pid_alive(c->tracked_pid)) {
          c->has_activity = false;
          c->tracked_pid = 0;
          if (s->hooks.on_activity) s->hooks.on_activity(s->ctx, c, nullptr);
          s->work++;
        }
      }
      i++;
      continue;
    }
    memmove(&s->conns[i], &s->conns[i + 1], (s->nconns - i - 1u) * sizeof s->conns[0]);
    s->conns[--s->nconns] = nullptr;
    release_conn(s, c);
  }
  return s->work;
}

void ccord_rpc_server_destroy(ccord_rpc_server *s)
{
  if (!s) return;
  while (s->nconns > 0) {
    ccord_rpc_conn *c = s->conns[--s->nconns];
    s->conns[s->nconns] = nullptr;
    release_conn(s, c);
  }
  if (s->fd >= 0) close(s->fd);
  if (s->path[0]) (void)unlink(s->path);
  ccord_arena_free(&s->scratch);
  ccord_free(s);
}
