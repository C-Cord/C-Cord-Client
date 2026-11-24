// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "workers_internal.h"

void pw_b64_encode(const uint8_t *in, size_t n, ccord_buf *out)
{
  if (n == 0) return;
  size_t elen = 0;
  char *enc = ccord_base64_encode_alloc(in, n, &elen);
  if (enc) {
    (void)ccord_buf_append(out, enc, elen);
    ccord_free(enc);
  }
}

static void pw_event_free_fields(pw_event *ev)
{
  if (!ev) return;
  ccord_free(ev->json);
  ccord_free(ev->raw_body);
  ccord_free(ev->final_url);
  ev->json = nullptr;
  ev->raw_body = nullptr;
  ev->final_url = nullptr;
}

void pw_push_ev(plugin_workers *w, const pw_event *ev)
{
  if (!w || !ev) return;
  pthread_mutex_lock(&w->ev_mu);
  if (w->nevents < PW_MAX_EVENTS) {
    if (w->nevents == w->capevents) {
      size_t nc = w->capevents ? w->capevents * 2u : 32u;
      pw_event *g = ccord_realloc_array(w->events, nc, sizeof *g);
      if (g) { w->events = g; w->capevents = nc; }
    }
    if (w->nevents < w->capevents) {
      w->events[w->nevents++] = *ev;
      atomic_store_explicit(&w->ev_pending, true, memory_order_release);
    } else {
      pw_event tmp = *ev;
      pw_event_free_fields(&tmp);
    }
  } else {
    pw_event tmp = *ev;
    pw_event_free_fields(&tmp);
  }
  pthread_mutex_unlock(&w->ev_mu);
}

bool pw_extract_url_host_port(const char *url, char *scheme_out, size_t scap, char *host_out, size_t hcap,
                              int *port_out, char *path_out, size_t pcap)
{
  if (!url || !scheme_out || scap == 0 || !host_out || hcap == 0 || !port_out || !path_out || pcap == 0) {
    return false;
  }
  for (const unsigned char *q = (const unsigned char *)url; *q; q++) {
    if (*q < 0x20u || *q == 0x7fu) return false;
  }
  const char *sep = strstr(url, "://");
  if (!sep) return false;
  size_t sl = (size_t)(sep - url);
  if (sl == 0 || sl >= scap) return false;
  for (size_t i = 0; i < sl; i++) {
    unsigned char c = (unsigned char)url[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
    if (!ok) return false;
    scheme_out[i] = (char)tolower(c);
  }
  scheme_out[sl] = '\0';

  const char *auth = sep + 3;
  const char *p = auth;
  while (*p && *p != '/' && *p != '?' && *p != '#') {
    unsigned char c = (unsigned char)*p;
    if (c == '@' || c == '\\' || c <= 0x20u || c >= 0x7fu) return false;
    p++;
  }

  const char *host_start = auth;
  const char *host_end = p;
  const char *port_start = nullptr;
  bool is_bracketed_v6 = false;
  int port = (strcmp(scheme_out, "https") == 0 || strcmp(scheme_out, "wss") == 0) ? 443 : 80;

  if (host_start < p && *host_start == '[') {
    const char *rb = memchr(host_start, ']', (size_t)(p - host_start));
    if (!rb) return false;
    is_bracketed_v6 = true;
    host_start++;
    host_end = rb;
    if (rb + 1 < p) {
      if (rb[1] != ':') return false;
      port_start = rb + 2;
    }
  } else {
    const char *col = memchr(host_start, ':', (size_t)(p - host_start));
    if (col) {
      if (memchr(col + 1, ':', (size_t)(p - (col + 1))) != nullptr) return false;
      host_end = col;
      port_start = col + 1;
    }
  }

  if (port_start) {
    if (port_start >= p) return false;
    unsigned long pval = 0;
    for (const char *q = port_start; q < p; q++) {
      if (*q < '0' || *q > '9') return false;
      pval = pval * 10ul + (unsigned long)(*q - '0');
      if (pval > 65535ul) return false;
    }
    if (pval == 0ul) return false;
    port = (int)pval;
  }

  size_t hl = (size_t)(host_end - host_start);
  if (hl == 0 || hl >= hcap) return false;
  for (size_t i = 0; i < hl; i++) {
    unsigned char c = (unsigned char)host_start[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              (is_bracketed_v6 && c == ':');
    if (!ok) return false;
  }
  memcpy(host_out, host_start, hl);
  host_out[hl] = '\0';
  *port_out = port;

  const char *path_end = p;
  while (*path_end && *path_end != '#') path_end++;
  size_t raw_plen = (size_t)(path_end - p);
  if (raw_plen == 0) {
    if (pcap < 2u) return false;
    path_out[0] = '/';
    path_out[1] = '\0';
  } else if (*p == '?') {
    if (raw_plen + 2u > pcap) return false;
    path_out[0] = '/';
    memcpy(path_out + 1, p, raw_plen);
    path_out[raw_plen + 1u] = '\0';
  } else {
    if (raw_plen >= pcap) return false;
    memcpy(path_out, p, raw_plen);
    path_out[raw_plen] = '\0';
  }
  return true;
}

static bool is_all_ipv4_chars(const char *s)
{
  if (!s || !s[0]) return false;
  for (const char *p = s; *p; p++) {
    if (!((*p >= '0' && *p <= '9') || *p == '.')) return false;
  }
  return true;
}

bool pw_is_localhost_host(const char *host)
{
  if (!host || !host[0]) return false;
  size_t raw_len = strlen(host);
  if (raw_len >= 256u) return false;
  char h[256];
  size_t start = 0;
  size_t end = raw_len;
  if (raw_len >= 2u && host[0] == '[' && host[raw_len - 1u] == ']') {
    start = 1u;
    end = raw_len - 1u;
  }
  size_t len = end - start;
  if (len == 0) return false;
  for (size_t i = 0; i < len; i++) {
    h[i] = (char)tolower((unsigned char)host[start + i]);
  }
  if (len > 1u && h[len - 1u] == '.') len--;
  h[len] = '\0';

  if (strcmp(h, "localhost") == 0) return true;
  if (len > 10u && strcmp(h + len - 10u, ".localhost") == 0) return true;
  if (strcmp(h, "::1") == 0 || strcmp(h, "::") == 0) return true;
  if (strcmp(h, "0.0.0.0") == 0 || strcmp(h, "0") == 0) return true;
  if (strncmp(h, "127.", 4) == 0 && is_all_ipv4_chars(h)) return true;
  if (strncmp(h, "0.", 2) == 0 && is_all_ipv4_chars(h)) return true;
  if (strncmp(h, "::ffff:127.", 11) == 0 || strncmp(h, "::ffff:0.", 9) == 0) return true;

#if !CCORD_OS_WINDOWS
  struct in_addr a4;
  if (inet_pton(AF_INET, h, &a4) == 1) {
    uint32_t ip = ntohl(a4.s_addr);
    if ((ip & 0xFF000000u) == 0x7F000000u || (ip & 0xFF000000u) == 0x00000000u) return true;
  }
  struct in6_addr a6;
  if (inet_pton(AF_INET6, h, &a6) == 1) {
    const uint8_t *b = a6.s6_addr;
    bool z12 = true;
    for (int i = 0; i < 12; i++) {
      if (b[i] != 0) { z12 = false; break; }
    }
    if (z12 && b[12] == 0 && b[13] == 0 && b[14] == 0 && (b[15] == 0 || b[15] == 1)) return true;
    bool v4m = true;
    for (int i = 0; i < 10; i++) {
      if (b[i] != 0) { v4m = false; break; }
    }
    if (v4m && b[10] == 0xff && b[11] == 0xff) {
      if (b[12] == 127 || b[12] == 0) return true;
    }
  }
#endif
  return false;
}

// ---------------------------------------------------------------- lifecycle

plugin_workers *pw_create(const ccord_plugin_platform_ops *ops)
{
  plugin_workers *w = ccord_calloc(1, sizeof *w);
  if (!w) return nullptr;
  if (ops) w->ops = *ops;
  atomic_init(&w->ev_pending, false);
  pthread_mutex_init(&w->ev_mu, nullptr);
  pthread_mutex_init(&w->slot_mu, nullptr);
#if !CCORD_OS_WINDOWS
  pw_make_wake_pipe(&w->shutdown_wake_r, &w->shutdown_wake_w);
#endif
#if defined(__linux__) && !CCORD_OS_WINDOWS
  w->inotify_fd = -1;
#endif
  return w;
}

void pw_remove_owner(plugin_workers *w, const char *owner)
{
  if (!w || !owner) return;
  for (size_t i = 0; i < PW_MAX_WATCHES; i++) {
    pw_watch *wt = w->watches[i];
    if (wt && wt->active && strcmp(wt->owner, owner) == 0) {
#if defined(__linux__) && !CCORD_OS_WINDOWS
      if (wt->wd < 0 && w->fallback_watch_count > 0) w->fallback_watch_count--;
      pw_inotify_rm_watch(w->inotify_fd, wt->wd);
#else
      if (w->fallback_watch_count > 0) w->fallback_watch_count--;
#endif
      if (w->watch_count > 0) w->watch_count--;
      ccord_free(wt);
      w->watches[i] = nullptr;
    }
  }
  pthread_mutex_lock(&w->slot_mu);
  for (size_t i = 0; i < PW_MAX_WS; i++) {
    if (w->ws[i] && strcmp(w->ws[i]->owner, owner) == 0) {
      atomic_store(&w->ws[i]->stop, true);
#if !CCORD_OS_WINDOWS
      pthread_mutex_lock(&w->ws[i]->send_mu);
      if (w->ws[i]->tcp_fd >= 0) shutdown(w->ws[i]->tcp_fd, SHUT_RDWR);
      pthread_mutex_unlock(&w->ws[i]->send_mu);
      pw_signal_wake(w->ws[i]->wake_w);
#endif
    }
  }
  for (size_t i = 0; i < PW_MAX_PROCS; i++) {
    if (w->procs[i] && strcmp(w->procs[i]->owner, owner) == 0) {
      atomic_store(&w->procs[i]->stop, true);
#if !CCORD_OS_WINDOWS
      if (w->procs[i]->pid > 0) kill(w->procs[i]->pid, SIGKILL);
      pw_signal_wake(w->procs[i]->wake_w);
#endif
    }
  }
  pthread_mutex_unlock(&w->slot_mu);
}

void pw_destroy(plugin_workers *w)
{
  if (!w) return;
  atomic_store(&w->shutting_down, true);
#if !CCORD_OS_WINDOWS
  pw_signal_wake(w->shutdown_wake_w);
#endif

  // Stop all websockets and spawned processes and join their threads.
  pw_ws_slot *ws_join[PW_MAX_WS] = {0};
  pw_proc_slot *pr_join[PW_MAX_PROCS] = {0};
  pthread_mutex_lock(&w->slot_mu);
  for (size_t i = 0; i < PW_MAX_WS; i++) {
    if (w->ws[i]) {
      atomic_store(&w->ws[i]->stop, true);
#if !CCORD_OS_WINDOWS
      pthread_mutex_lock(&w->ws[i]->send_mu);
      if (w->ws[i]->tcp_fd >= 0) shutdown(w->ws[i]->tcp_fd, SHUT_RDWR);
      pthread_mutex_unlock(&w->ws[i]->send_mu);
      pw_signal_wake(w->ws[i]->wake_w);
#endif
      ws_join[i] = w->ws[i];
      w->ws[i] = nullptr;
    }
  }
  for (size_t i = 0; i < PW_MAX_PROCS; i++) {
    if (w->procs[i]) {
      atomic_store(&w->procs[i]->stop, true);
#if !CCORD_OS_WINDOWS
      if (w->procs[i]->pid > 0) kill(w->procs[i]->pid, SIGKILL);
      pw_signal_wake(w->procs[i]->wake_w);
#endif
      pr_join[i] = w->procs[i];
      w->procs[i] = nullptr;
    }
  }
  pthread_mutex_unlock(&w->slot_mu);

  for (size_t i = 0; i < PW_MAX_WS; i++) {
    if (ws_join[i]) {
      pthread_join(ws_join[i]->thr, nullptr);
#if !CCORD_OS_WINDOWS
      pw_close_wake_pipe(&ws_join[i]->wake_r, &ws_join[i]->wake_w);
#endif
      pthread_mutex_destroy(&ws_join[i]->send_mu);
      ccord_free(ws_join[i]->headers_json);
      ccord_free(ws_join[i]);
    }
  }
  for (size_t i = 0; i < PW_MAX_PROCS; i++) {
    if (pr_join[i]) {
      pthread_join(pr_join[i]->thr, nullptr);
#if !CCORD_OS_WINDOWS
      pw_close_wake_pipe(&pr_join[i]->wake_r, &pr_join[i]->wake_w);
#endif
      pthread_mutex_destroy(&pr_join[i]->io_mu);
      ccord_free(pr_join[i]);
    }
  }

  // Wait briefly for any one-shot detached HTTP/exec jobs to wind down.
  for (int i = 0; i < 100 && atomic_load(&w->active_jobs) > 0; i++) {
    ccord_sleep_ms(10);
  }
#if !CCORD_OS_WINDOWS
  pw_close_wake_pipe(&w->shutdown_wake_r, &w->shutdown_wake_w);
#endif
#if defined(__linux__) && !CCORD_OS_WINDOWS
  if (w->inotify_fd >= 0) close(w->inotify_fd);
#endif
  for (size_t i = 0; i < PW_MAX_WATCHES; i++) {
    ccord_free(w->watches[i]);
    w->watches[i] = nullptr;
  }
  w->watch_count = 0;
  w->fallback_watch_count = 0;
  pthread_mutex_lock(&w->ev_mu);
  for (size_t i = 0; i < w->nevents; i++) pw_event_free_fields(&w->events[i]);
  ccord_free(w->events);
  pthread_mutex_unlock(&w->ev_mu);
  pthread_mutex_destroy(&w->ev_mu);
  pthread_mutex_destroy(&w->slot_mu);
  ccord_free(w);
}

// ---------------------------------------------------------------- Pump

void pw_pump(plugin_workers *w, void (*on_event)(void *ud, const pw_event *ev), void *ud)
{
  if (!w) return;
  if (w->watch_count > 0) pw_poll_watches(w);
  if (!atomic_load_explicit(&w->ev_pending, memory_order_acquire)) return;
  pw_event *batch = nullptr;
  size_t n = 0;
  pthread_mutex_lock(&w->ev_mu);
  atomic_store_explicit(&w->ev_pending, false, memory_order_relaxed);
  if (w->nevents > 0) {
    batch = w->events;
    n = w->nevents;
    w->events = nullptr;
    w->nevents = 0;
    w->capevents = 0;
  }
  pthread_mutex_unlock(&w->ev_mu);
  for (size_t i = 0; i < n; i++) {
    if (on_event) on_event(ud, &batch[i]);
    pw_event_free_fields(&batch[i]);
  }
  ccord_free(batch);
}

size_t pw_wait_info(const plugin_workers *w, ccord_socket_t *out_fds, size_t cap, uint32_t *out_timeout_ms)
{
  if (out_timeout_ms) *out_timeout_ms = UINT32_MAX;
  if (!w) return 0;
  size_t n = 0;
  uint32_t timeout = UINT32_MAX;
#if defined(__linux__) && !CCORD_OS_WINDOWS
  if (w->watch_count > 0 && w->inotify_fd >= 0 && out_fds && n < cap) {
    out_fds[n++] = (ccord_socket_t)w->inotify_fd;
  }
#else
  (void)out_fds;
  (void)cap;
#endif
  if (atomic_load_explicit(&w->ev_pending, memory_order_acquire)) {
    timeout = 0u;
  } else if (w->fallback_watch_count > 0) {
    uint64_t now = ccord_clock_ms();
    uint64_t due = w->last_watch_poll_ms + 250u;
    uint64_t rem = due > now ? (due - now) : 0u;
    if (rem < (uint64_t)timeout) timeout = (uint32_t)rem;
  }
  if (out_timeout_ms) *out_timeout_ms = timeout;
  return n;
}
