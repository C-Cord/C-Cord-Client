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
    } else {
      ccord_free(ev->json);
    }
  } else {
    ccord_free(ev->json);
  }
  pthread_mutex_unlock(&w->ev_mu);
}

bool pw_extract_url_host_port(const char *url, char *scheme_out, size_t scap, char *host_out, size_t hcap,
                                  int *port_out, char *path_out, size_t pcap)
{
  if (!url) return false;
  const char *sep = strstr(url, "://");
  if (!sep) return false;
  size_t sl = (size_t)(sep - url);
  if (sl == 0 || sl >= scap) return false;
  for (size_t i = 0; i < sl; i++) scheme_out[i] = (char)tolower((unsigned char)url[i]);
  scheme_out[sl] = '\0';
  const char *auth = sep + 3;
  const char *p = auth;
  while (*p && *p != '/' && *p != '?') p++;
  const char *host_start = auth, *host_end = p;
  int port = (strcmp(scheme_out, "https") == 0 || strcmp(scheme_out, "wss") == 0) ? 443 : 80;
  if (host_start < host_end && *host_start == '[') {
    const char *rb = memchr(host_start, ']', (size_t)(host_end - host_start));
    if (!rb) return false;
    host_start++;
    if (rb + 1 < host_end && rb[1] == ':') port = atoi(rb + 2);
    host_end = rb;
  } else {
    const char *col = memchr(host_start, ':', (size_t)(host_end - host_start));
    if (col) {
      port = atoi(col + 1);
      host_end = col;
    }
  }
  size_t hl = (size_t)(host_end - host_start);
  if (hl == 0 || hl >= hcap || port <= 0 || port > 65535) return false;
  memcpy(host_out, host_start, hl);
  host_out[hl] = '\0';
  *port_out = port;
  if (*p) snprintf(path_out, pcap, "%s", p);
  else snprintf(path_out, pcap, "/");
  return true;
}

bool pw_is_localhost_host(const char *host)
{
  return host && (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0 || strcmp(host, "::1") == 0);
}

// ---------------------------------------------------------------- lifecycle

plugin_workers *pw_create(const ccord_plugin_platform_ops *ops)
{
  plugin_workers *w = ccord_calloc(1, sizeof *w);
  if (!w) return nullptr;
  if (ops) w->ops = *ops;
  pthread_mutex_init(&w->ev_mu, nullptr);
  pthread_mutex_init(&w->slot_mu, nullptr);
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
      pw_inotify_rm_watch(w->inotify_fd, wt->wd);
#endif
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
#endif
    }
  }
  for (size_t i = 0; i < PW_MAX_PROCS; i++) {
    if (w->procs[i] && strcmp(w->procs[i]->owner, owner) == 0) {
      atomic_store(&w->procs[i]->stop, true);
#if !CCORD_OS_WINDOWS
      if (w->procs[i]->pid > 0) kill(w->procs[i]->pid, SIGKILL);
#endif
    }
  }
  pthread_mutex_unlock(&w->slot_mu);
}

void pw_destroy(plugin_workers *w)
{
  if (!w) return;
  atomic_store(&w->shutting_down, true);

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
#endif
      pr_join[i] = w->procs[i];
      w->procs[i] = nullptr;
    }
  }
  pthread_mutex_unlock(&w->slot_mu);

  for (size_t i = 0; i < PW_MAX_WS; i++) {
    if (ws_join[i]) {
      pthread_join(ws_join[i]->thr, nullptr);
      pthread_mutex_destroy(&ws_join[i]->send_mu);
      ccord_free(ws_join[i]->headers_json);
      ccord_free(ws_join[i]);
    }
  }
  for (size_t i = 0; i < PW_MAX_PROCS; i++) {
    if (pr_join[i]) {
      pthread_join(pr_join[i]->thr, nullptr);
      pthread_mutex_destroy(&pr_join[i]->io_mu);
      ccord_free(pr_join[i]);
    }
  }

  // Wait briefly for any one-shot detached HTTP/exec jobs to wind down.
  for (int i = 0; i < 100 && atomic_load(&w->active_jobs) > 0; i++) {
    ccord_sleep_ms(10);
  }
#if defined(__linux__) && !CCORD_OS_WINDOWS
  if (w->inotify_fd >= 0) close(w->inotify_fd);
#endif
  for (size_t i = 0; i < PW_MAX_WATCHES; i++) {
    ccord_free(w->watches[i]);
    w->watches[i] = nullptr;
  }
  pthread_mutex_lock(&w->ev_mu);
  for (size_t i = 0; i < w->nevents; i++) ccord_free(w->events[i].json);
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
  pw_poll_watches(w);
  pw_event *batch = nullptr;
  size_t n = 0;
  pthread_mutex_lock(&w->ev_mu);
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
    ccord_free(batch[i].json);
  }
  ccord_free(batch);
}
