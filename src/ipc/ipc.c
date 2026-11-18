// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include <ccord/ipc/ipc.h>

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IPC_MAX_PAYLOAD ((size_t)1u << 20)
#define IPC_READ_CHUNK ((size_t)64u << 10)
#define IPC_OUT_CAP ((size_t)1u << 20)
#define IPC_MAX_METHOD 64u

struct ccord_ipc {
  int fd;
  char *in;
  size_t in_len, in_cap, in_off;
  bool eof;
  char *out;
  size_t out_len, out_off, out_cap;
};

static const ccord_json_limits k_limits = {16, 8192, (size_t)1u << 20};

static const char *const k_error_codes[] = {
  "permission_denied", "invalid_argument", "limit_exceeded", "not_found", "unsupported", "timeout", "internal",
};

static void fail(ccord_error *e, ccord_status code, const char *msg)
{
  ccord_error_set(e, code, "%s", msg);
}

static uint32_t le32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(char *p, uint32_t v)
{
  p[0] = (char)(v & 0xffu);
  p[1] = (char)((v >> 8) & 0xffu);
  p[2] = (char)((v >> 16) & 0xffu);
  p[3] = (char)((v >> 24) & 0xffu);
}

ccord_ipc *ccord_ipc_open(int fd, ccord_error *err)
{
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    ccord_error_set(err, CCORD_ERR_IO, "ipc: fcntl failed: %s", strerror(errno));
    close(fd);
    return nullptr;
  }
  ccord_ipc *ipc = ccord_calloc(1, sizeof *ipc);
  if (!ipc) {
    fail(err, CCORD_ERR_NOMEM, "ipc: out of memory");
    close(fd);
    return nullptr;
  }
  ipc->fd = fd;
  return ipc;
}

void ccord_ipc_close(ccord_ipc *ipc)
{
  if (!ipc) return;
  if (ipc->fd >= 0) close(ipc->fd);
  ccord_free(ipc->in);
  ccord_free(ipc->out);
  ccord_free(ipc);
}

int ccord_ipc_fd(const ccord_ipc *ipc)
{
  return ipc ? ipc->fd : -1;
}

bool ccord_ipc_has_pending_output(const ccord_ipc *ipc)
{
  return ipc && ipc->out_len > ipc->out_off;
}

// ---------------------------------------------------------------- reading

static ccord_status decode_envelope(const char *text, size_t len, ccord_arena *arena, ccord_ipc_msg *msg, ccord_error *err)
{
  ccord_error scratch = {0};
  const ccord_json *root = ccord_json_parse(arena, text, len, &k_limits, &scratch);
  if (!root) {
    if (scratch.code == CCORD_ERR_NOMEM) {
      fail(err, CCORD_ERR_NOMEM, "ipc: out of memory");
      return CCORD_ERR_NOMEM;
    }
    fail(err, CCORD_ERR_PROTOCOL, "ipc: invalid JSON frame");
    return CCORD_ERR_PROTOCOL;
  }
#define PROTO(m) do { fail(err, CCORD_ERR_PROTOCOL, m); return CCORD_ERR_PROTOCOL; } while (0)
  if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) PROTO("ipc: frame is not an object");
  size_t tlen = 0;
  const ccord_json *tj = ccord_json_get(root, "t");
  const char *t = tj ? ccord_json_string(tj, &tlen) : nullptr;
  if (!t) PROTO("ipc: missing envelope type");

  const char *allowed[5];
  size_t nallowed = 0;
  if (strcmp(t, "req") == 0) {
    msg->kind = CCORD_IPC_REQ;
    allowed[0] = "t"; allowed[1] = "id"; allowed[2] = "m"; allowed[3] = "p"; nallowed = 4;
  } else if (strcmp(t, "res") == 0) {
    msg->kind = CCORD_IPC_RES;
    allowed[0] = "t"; allowed[1] = "id"; allowed[2] = "ok"; allowed[3] = "r"; allowed[4] = "e"; nallowed = 5;
  } else if (strcmp(t, "ntf") == 0) {
    msg->kind = CCORD_IPC_NTF;
    allowed[0] = "t"; allowed[1] = "m"; allowed[2] = "p"; nallowed = 3;
  } else {
    PROTO("ipc: unknown envelope type");
  }
  for (size_t i = 0; i < ccord_json_object_len(root); i++) {
    const ccord_json *v = nullptr;
    const char *k = ccord_json_object_key(root, i, &v);
    bool ok = false;
    for (size_t j = 0; j < nallowed; j++) ok = ok || strcmp(k, allowed[j]) == 0;
    if (!ok) PROTO("ipc: unexpected envelope key");
  }

  if (msg->kind != CCORD_IPC_NTF) {
    int64_t v = 0;
    const ccord_json *idj = ccord_json_get(root, "id");
    if (!idj || !ccord_json_int64(idj, &v) || v < 1 || v > (int64_t)UINT32_MAX) PROTO("ipc: bad id");
    msg->id = (uint32_t)v;
  }
  if (msg->kind != CCORD_IPC_RES) {
    size_t mlen = 0;
    const ccord_json *mj = ccord_json_get(root, "m");
    const char *m = mj ? ccord_json_string(mj, &mlen) : nullptr;
    if (!m || mlen == 0 || mlen > IPC_MAX_METHOD) PROTO("ipc: bad method");
    msg->method = m;
    const ccord_json *pj = ccord_json_get(root, "p");
    if (pj && ccord_json_type_of(pj) != CCORD_JSON_OBJECT) PROTO("ipc: params must be an object");
    msg->params = pj;
    return CCORD_OK;
  }
  const ccord_json *okj = ccord_json_get(root, "ok");
  if (!okj || ccord_json_type_of(okj) != CCORD_JSON_BOOL) PROTO("ipc: bad ok flag");
  msg->ok = ccord_json_bool(okj, false);
  const ccord_json *rj = ccord_json_get(root, "r");
  const ccord_json *ej = ccord_json_get(root, "e");
  if (msg->ok) {
    if (ej) PROTO("ipc: ok response has error");
    msg->result = rj;
    return CCORD_OK;
  }
  if (rj || !ej || ccord_json_type_of(ej) != CCORD_JSON_OBJECT) PROTO("ipc: error response needs e object");
  size_t clen = 0, mlen = 0;
  const ccord_json *cj = ccord_json_get(ej, "code");
  const ccord_json *mj = ccord_json_get(ej, "message");
  const char *code = cj ? ccord_json_string(cj, &clen) : nullptr;
  const char *emsg = mj ? ccord_json_string(mj, &mlen) : nullptr;
  if (!code || !emsg) PROTO("ipc: error needs code and message");
  bool known = false;
  for (size_t i = 0; i < sizeof k_error_codes / sizeof k_error_codes[0]; i++) known = known || strcmp(code, k_error_codes[i]) == 0;
  if (!known) PROTO("ipc: unknown error code");
  msg->err_code = code;
  msg->err_message = emsg;
  return CCORD_OK;
#undef PROTO
}

// Decodes one complete frame already in the input buffer. EMPTY when the frame is incomplete.
static ccord_ipc_read parse_buffered(ccord_ipc *ipc, ccord_arena *arena, ccord_ipc_msg *msg, ccord_error *err)
{
  size_t avail = ipc->in_len - ipc->in_off;
  if (avail < 4) return CCORD_IPC_READ_EMPTY;
  const unsigned char *p = (const unsigned char *)ipc->in + ipc->in_off;
  uint32_t n = le32(p);
  if (n == 0 || n > IPC_MAX_PAYLOAD) {
    fail(err, CCORD_ERR_PROTOCOL, "ipc: frame size out of range");
    return CCORD_IPC_READ_ERROR;
  }
  if (avail - 4 < (size_t)n) return CCORD_IPC_READ_EMPTY;
  const char *payload = ipc->in + ipc->in_off + 4;
  ipc->in_off += 4 + (size_t)n;
  memset(msg, 0, sizeof *msg);
  if (decode_envelope(payload, n, arena, msg, err) != CCORD_OK) return CCORD_IPC_READ_ERROR;
  return CCORD_IPC_READ_MSG;
}

// Compacts consumed bytes and reads one chunk. Sets ipc->eof on peer close.
static ccord_status fill(ccord_ipc *ipc, ccord_error *err)
{
  if (ipc->in_off > 0) {
    size_t rest = ipc->in_len - ipc->in_off;
    if (rest) memmove(ipc->in, ipc->in + ipc->in_off, rest);
    ipc->in_len = rest;
    ipc->in_off = 0;
  }
  if (ipc->in_cap - ipc->in_len < IPC_READ_CHUNK) {
    size_t need = ipc->in_len + IPC_READ_CHUNK;
    size_t cap = ipc->in_cap ? ipc->in_cap : 2 * IPC_READ_CHUNK;
    while (cap < need) cap *= 2;
    char *np = ccord_realloc(ipc->in, cap);
    if (!np) {
      fail(err, CCORD_ERR_NOMEM, "ipc: out of memory");
      return CCORD_ERR_NOMEM;
    }
    ipc->in = np;
    ipc->in_cap = cap;
  }
  for (;;) {
    ssize_t n = read(ipc->fd, ipc->in + ipc->in_len, IPC_READ_CHUNK);
    if (n > 0) {
      ipc->in_len += (size_t)n;
      return CCORD_OK;
    }
    if (n == 0) {
      ipc->eof = true;
      return CCORD_OK;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN) return CCORD_OK;
    ccord_error_set(err, CCORD_ERR_IO, "ipc: read failed: %s", strerror(errno));
    return CCORD_ERR_IO;
  }
}

ccord_ipc_read ccord_ipc_try_read(ccord_ipc *ipc, ccord_arena *arena, ccord_ipc_msg *msg, ccord_error *err)
{
  ccord_error scratch = {0};
  ccord_error *e = err ? err : &scratch;
  if (!ipc || !arena || !msg) {
    fail(e, CCORD_ERR_INVALID, "ipc: null argument");
    return CCORD_IPC_READ_ERROR;
  }
  ccord_ipc_read r = parse_buffered(ipc, arena, msg, e);
  if (r != CCORD_IPC_READ_EMPTY) return r;
  if (!ipc->eof && fill(ipc, e) != CCORD_OK) return CCORD_IPC_READ_ERROR;
  r = parse_buffered(ipc, arena, msg, e);
  if (r == CCORD_IPC_READ_EMPTY && ipc->eof) {
    fail(e, CCORD_ERR_IO, "ipc: peer closed");
    return CCORD_IPC_READ_EOF;
  }
  return r;
}

ccord_status ccord_ipc_wait(ccord_ipc *ipc, ccord_arena *arena, ccord_ipc_msg *msg, int timeout_ms, ccord_error *err)
{
  ccord_error scratch = {0};
  ccord_error *e = err ? err : &scratch;
  uint64_t deadline = ccord_clock_ms() + (timeout_ms > 0 ? (uint64_t)timeout_ms : 0);
  for (;;) {
    ccord_ipc_read r = ccord_ipc_try_read(ipc, arena, msg, e);
    if (r == CCORD_IPC_READ_MSG) return CCORD_OK;
    if (r != CCORD_IPC_READ_EMPTY) return e->code;
    uint64_t now = ccord_clock_ms();
    if (now >= deadline) {
      fail(e, CCORD_ERR_TIMEOUT, "ipc: timeout");
      return CCORD_ERR_TIMEOUT;
    }
    (void)ccord_ipc_flush(ipc, 0, e);
    uint64_t left = deadline - now;
    int ms = left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
    short events = POLLIN;
    if (ccord_ipc_has_pending_output(ipc)) events = (short)(events | POLLOUT);
    struct pollfd pfd = {.fd = ipc->fd, .events = events};
    if (poll(&pfd, 1, ms) < 0 && errno != EINTR) {
      ccord_error_set(e, CCORD_ERR_IO, "ipc: poll failed: %s", strerror(errno));
      return CCORD_ERR_IO;
    }
  }
}

// ---------------------------------------------------------------- writing

static bool app_raw(ccord_buf *b, const char *s)
{
  return ccord_buf_append_str(b, s) == CCORD_OK;
}

// Appends a JSON string literal (escaped by the core writer).
static bool app_str(ccord_buf *b, const char *s)
{
  ccord_jw w;
  ccord_jw_init(&w, b);
  ccord_jw_cstr(&w, s);
  return ccord_jw_ok(&w);
}

static ccord_status queue_frame(ccord_ipc *ipc, ccord_buf *b, ccord_error *err)
{
  size_t n = b->len;
  if (b->failed || n == 0 || n > IPC_MAX_PAYLOAD) {
    fail(err, CCORD_ERR_RANGE, "ipc: frame too large or malformed");
    return CCORD_ERR_RANGE;
  }
  size_t pending = ipc->out_len - ipc->out_off;
  if (pending + 4 + n > IPC_OUT_CAP) {
    fail(err, CCORD_ERR_RANGE, "ipc: output queue full");
    return CCORD_ERR_RANGE;
  }
  if (ipc->out_off > 0) {
    if (pending) memmove(ipc->out, ipc->out + ipc->out_off, pending);
    ipc->out_len = pending;
    ipc->out_off = 0;
  }
  size_t need = ipc->out_len + 4 + n;
  if (need > ipc->out_cap) {
    size_t cap = ipc->out_cap ? ipc->out_cap : 4096;
    while (cap < need) cap = cap * 2 > IPC_OUT_CAP ? IPC_OUT_CAP : cap * 2;
    char *np = ccord_realloc(ipc->out, cap);
    if (!np) {
      fail(err, CCORD_ERR_NOMEM, "ipc: out of memory");
      return CCORD_ERR_NOMEM;
    }
    ipc->out = np;
    ipc->out_cap = cap;
  }
  put_le32(ipc->out + ipc->out_len, (uint32_t)n);
  memcpy(ipc->out + ipc->out_len + 4, b->data, n);
  ipc->out_len = need;
  return CCORD_OK;
}

ccord_status ccord_ipc_send_request(ccord_ipc *ipc, uint32_t id, const char *method, const char *params_json, ccord_error *err)
{
  if (!ipc || !method || id == 0) {
    fail(err, CCORD_ERR_INVALID, "ipc: bad request");
    return CCORD_ERR_INVALID;
  }
  ccord_buf b;
  ccord_buf_init(&b, IPC_MAX_PAYLOAD);
  bool ok = app_raw(&b, "{\"t\":\"req\",\"id\":") && ccord_buf_appendf(&b, "%u", id) == CCORD_OK &&
            app_raw(&b, ",\"m\":") && app_str(&b, method);
  if (ok && params_json) ok = app_raw(&b, ",\"p\":") && app_raw(&b, params_json);
  ok = ok && app_raw(&b, "}");
  ccord_status st = ok ? queue_frame(ipc, &b, err) : CCORD_ERR_INVALID;
  if (!ok) fail(err, CCORD_ERR_INVALID, "ipc: malformed request");
  ccord_buf_free(&b);
  return st;
}

ccord_status ccord_ipc_send_notify(ccord_ipc *ipc, const char *method, const char *params_json, ccord_error *err)
{
  if (!ipc || !method) {
    fail(err, CCORD_ERR_INVALID, "ipc: bad notification");
    return CCORD_ERR_INVALID;
  }
  ccord_buf b;
  ccord_buf_init(&b, IPC_MAX_PAYLOAD);
  bool ok = app_raw(&b, "{\"t\":\"ntf\",\"m\":") && app_str(&b, method);
  if (ok && params_json) ok = app_raw(&b, ",\"p\":") && app_raw(&b, params_json);
  ok = ok && app_raw(&b, "}");
  ccord_status st = ok ? queue_frame(ipc, &b, err) : CCORD_ERR_INVALID;
  if (!ok) fail(err, CCORD_ERR_INVALID, "ipc: malformed notification");
  ccord_buf_free(&b);
  return st;
}

ccord_status ccord_ipc_send_ok(ccord_ipc *ipc, uint32_t id, const char *result_json, ccord_error *err)
{
  if (!ipc || id == 0) {
    fail(err, CCORD_ERR_INVALID, "ipc: bad response");
    return CCORD_ERR_INVALID;
  }
  ccord_buf b;
  ccord_buf_init(&b, IPC_MAX_PAYLOAD);
  bool ok = ccord_buf_appendf(&b, "{\"t\":\"res\",\"id\":%u,\"ok\":true,\"r\":", id) == CCORD_OK &&
            app_raw(&b, result_json ? result_json : "{}") && app_raw(&b, "}");
  ccord_status st = ok ? queue_frame(ipc, &b, err) : CCORD_ERR_INVALID;
  if (!ok) fail(err, CCORD_ERR_INVALID, "ipc: malformed response");
  ccord_buf_free(&b);
  return st;
}

ccord_status ccord_ipc_send_error(ccord_ipc *ipc, uint32_t id, const char *code, const char *message, ccord_error *err)
{
  if (!ipc || id == 0 || !code || !message) {
    fail(err, CCORD_ERR_INVALID, "ipc: bad error response");
    return CCORD_ERR_INVALID;
  }
  ccord_buf b;
  ccord_buf_init(&b, IPC_MAX_PAYLOAD);
  bool ok = ccord_buf_appendf(&b, "{\"t\":\"res\",\"id\":%u,\"ok\":false,\"e\":{\"code\":", id) == CCORD_OK &&
            app_str(&b, code) && app_raw(&b, ",\"message\":") && app_str(&b, message) && app_raw(&b, "}}");
  ccord_status st = ok ? queue_frame(ipc, &b, err) : CCORD_ERR_INVALID;
  if (!ok) fail(err, CCORD_ERR_INVALID, "ipc: malformed error response");
  ccord_buf_free(&b);
  return st;
}

ccord_status ccord_ipc_flush(ccord_ipc *ipc, int timeout_ms, ccord_error *err)
{
  if (!ipc) return CCORD_ERR_INVALID;
  uint64_t deadline = ccord_clock_ms() + (timeout_ms > 0 ? (uint64_t)timeout_ms : 0);
  while (ipc->out_len > ipc->out_off) {
    ssize_t n = send(ipc->fd, ipc->out + ipc->out_off, ipc->out_len - ipc->out_off, MSG_NOSIGNAL);
    if (n > 0) {
      ipc->out_off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN)) {
      uint64_t now = ccord_clock_ms();
      if (timeout_ms <= 0 || now >= deadline) return timeout_ms <= 0 ? CCORD_OK : CCORD_ERR_TIMEOUT;
      uint64_t left = deadline - now;
      int ms = left > (uint64_t)INT_MAX ? INT_MAX : (int)left;
      struct pollfd pfd = {.fd = ipc->fd, .events = POLLOUT};   // POLLOUT is a short-sized constant
      (void)poll(&pfd, 1, ms);
      continue;
    }
    ccord_error_set(err, CCORD_ERR_IO, "ipc: send failed: %s", strerror(errno));
    return CCORD_ERR_IO;
  }
  ipc->out_len = 0;
  ipc->out_off = 0;
  return CCORD_OK;
}
