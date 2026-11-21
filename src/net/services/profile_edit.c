// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Own-profile edits: validation, request bodies, answer parsing and the two REST backends. See profile_edit.h.
#include <ccord/net/profile_edit.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/base64.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>

#ifdef CCORD_PROFILE_LIVE
#include <ccord/net/live_transport.h>
#endif

// Largest request body: two 10 MiB images (avatar + banner) as base64 plus data-URI prefixes and surrounding JSON.
#define PROFILE_BODY_MAX (2u * (((size_t)CCORD_PROFILE_IMAGE_MAX_BYTES / 3u + 1u) * 4u) + 8192u)

// ---- validation helpers ---------------------------------------------------------------------------------------

// Number of characters (code points) in valid UTF-8 text, or -1 when the text has invalid bytes or control chars.
static long text_chars(const char *s, size_t len)
{
  if (!ccord_utf8_valid_text(s, len)) return -1;
  long n = 0;
  for (size_t i = 0; i < len; i = ccord_utf8_next(s, len, i)) n++;
  return n;
}

// ---- body building --------------------------------------------------------------------------------------------

typedef struct body_builder {
  ccord_buf buf;
  bool any;
  bool failed;
} body_builder;

static void bb_init(body_builder *b)
{
  ccord_buf_init(&b->buf, PROFILE_BODY_MAX);
  b->any = false;
  b->failed = ccord_buf_append_str(&b->buf, "{") != CCORD_OK;
}

static void bb_key(body_builder *b, const char *name)
{
  if (b->any && ccord_buf_append_str(&b->buf, ",") != CCORD_OK) b->failed = true;
  b->any = true;
  if (ccord_buf_appendf(&b->buf, "\"%s\":", name) != CCORD_OK) b->failed = true;
}

static void bb_null(body_builder *b, const char *name)
{
  bb_key(b, name);
  if (ccord_buf_append_str(&b->buf, "null") != CCORD_OK) b->failed = true;
}

static void bb_string(body_builder *b, const char *name, const char *s)
{
  bb_key(b, name);
  ccord_jw w;
  ccord_jw_init(&w, &b->buf);
  ccord_jw_string(&w, s, strlen(s));
  if (!ccord_jw_ok(&w)) b->failed = true;
}

static void bb_int(body_builder *b, const char *name, int64_t v)
{
  bb_key(b, name);
  if (ccord_buf_appendf(&b->buf, "%lld", (long long)v) != CCORD_OK) b->failed = true;
}

// "data:<mime>;base64,<...>" as a JSON string value.
static void bb_image(body_builder *b, const char *name, const char *mime, const uint8_t *d, size_t len)
{
  bb_key(b, name);
  if (ccord_buf_appendf(&b->buf, "\"data:%s;base64,", mime) != CCORD_OK) b->failed = true;
  char chunk[4097];  // 3072 raw bytes -> 4096 base64 chars + NUL
  size_t i = 0;
  while (i < len && !b->failed) {
    size_t take = len - i;
    if (take > 3072u) take = 3072u;
    size_t o = ccord_base64_encode(d + i, take, chunk, sizeof chunk);
    i += take;
    if (ccord_buf_append(&b->buf, chunk, o) != CCORD_OK) b->failed = true;
  }
  if (ccord_buf_append_str(&b->buf, "\"") != CCORD_OK) b->failed = true;
}

static ccord_status bb_finish(body_builder *b, ccord_profile_request *req, const char *path, uint32_t fields,
                              ccord_error *err)
{
  if (ccord_buf_append_str(&b->buf, "}") != CCORD_OK) b->failed = true;
  if (b->failed) {
    ccord_buf_free(&b->buf);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "profile: could not build the request");
  }
  size_t n = 0;
  req->body = ccord_buf_take(&b->buf, &n);
  if (!req->body) return ccord_error_set(err, CCORD_ERR_NOMEM, "profile: out of memory");
  req->body_len = n;
  req->path = path;
  req->fields = fields;
  return CCORD_OK;
}

static ccord_status check_image(const char *what, const uint8_t *d, size_t len, const char **mime, ccord_error *err)
{
  if (!d || len == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "profile: %s is empty", what);
  }
  if (len > CCORD_PROFILE_IMAGE_MAX_BYTES) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "profile: %s is larger than 10 MB", what);
  }
  *mime = ccord_profile_image_mime(d, len);
  if (!*mime) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "profile: %s must be a PNG, JPEG, GIF or WebP image", what);
  }
  return CCORD_OK;
}

static ccord_status check_text(const char *what, const char *s, size_t max_chars, ccord_error *err)
{
  size_t n = strlen(s);
  if (n > max_chars * 4u) return ccord_error_set(err, CCORD_ERR_RANGE, "profile: %s is too long (max %zu)", what, max_chars);
  long c = text_chars(s, n);
  if (c < 0) return ccord_error_set(err, CCORD_ERR_INVALID, "profile: %s has invalid characters", what);
  if ((size_t)c > max_chars) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "profile: %s is too long (max %zu characters)", what, max_chars);
  }
  return CCORD_OK;
}

ccord_status ccord_profile_edit_plan(const ccord_profile_edit *e, ccord_profile_plan *out, ccord_error *err)
{
  ccord_error_clear(err);
  if (out) memset(out, 0, sizeof *out);
  if (!e || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "profile: null argument");
  if (!e->has_display_name && !e->has_avatar && !e->has_banner && !e->has_bio && !e->has_pronouns && !e->has_accent) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "profile: nothing to change");
  }

  // Validate everything first so a bad field never leaves a half-built plan behind.
  const char *name = e->display_name ? e->display_name : "";
  const char *bio = e->bio ? e->bio : "";
  const char *pron = e->pronouns ? e->pronouns : "";
  const char *av_mime = nullptr, *bn_mime = nullptr;
  ccord_status st;
  if (e->has_display_name && (st = check_text("display name", name, CCORD_PROFILE_DISPLAY_NAME_MAX, err)) != CCORD_OK) return st;
  if (e->has_bio && (st = check_text("bio", bio, CCORD_PROFILE_BIO_MAX, err)) != CCORD_OK) return st;
  if (e->has_pronouns && (st = check_text("pronouns", pron, CCORD_PROFILE_PRONOUNS_MAX, err)) != CCORD_OK) return st;
  if (e->has_avatar && (e->avatar || e->avatar_len > 0) &&
      (st = check_image("avatar", e->avatar, e->avatar_len, &av_mime, err)) != CCORD_OK) {
    return st;
  }
  if (e->has_banner && (e->banner || e->banner_len > 0) &&
      (st = check_image("banner", e->banner, e->banner_len, &bn_mime, err)) != CCORD_OK) {
    return st;
  }
  if (e->has_accent && e->accent_color > (int32_t)CCORD_PROFILE_ACCENT_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "profile: accent colour must be 0..0xFFFFFF");
  }

  if (e->has_display_name || e->has_avatar || e->has_banner) {
    body_builder b;
    bb_init(&b);
    uint32_t fields = 0;
    if (e->has_display_name) {
      fields |= 1u << CCORD_PROFILE_FIELD_DISPLAY_NAME;
      if (name[0] == '\0') bb_null(&b, "global_name");
      else bb_string(&b, "global_name", name);
    }
    if (e->has_avatar) {
      fields |= 1u << CCORD_PROFILE_FIELD_AVATAR;
      if (av_mime) bb_image(&b, "avatar", av_mime, e->avatar, e->avatar_len);
      else bb_null(&b, "avatar");
    }
    if (e->has_banner) {
      fields |= 1u << CCORD_PROFILE_FIELD_BANNER;
      if (bn_mime) bb_image(&b, "banner", bn_mime, e->banner, e->banner_len);
      else bb_null(&b, "banner");
    }
    st = bb_finish(&b, &out->req[out->count], "/users/@me", fields, err);
    if (st != CCORD_OK) {
      ccord_profile_plan_free(out);
      return st;
    }
    out->count++;
  }
  if (e->has_bio || e->has_pronouns || e->has_accent) {
    body_builder b;
    bb_init(&b);
    uint32_t fields = 0;
    if (e->has_bio) {
      fields |= 1u << CCORD_PROFILE_FIELD_BIO;
      bb_string(&b, "bio", bio);
    }
    if (e->has_pronouns) {
      fields |= 1u << CCORD_PROFILE_FIELD_PRONOUNS;
      bb_string(&b, "pronouns", pron);
    }
    if (e->has_accent) {
      fields |= 1u << CCORD_PROFILE_FIELD_ACCENT;
      if (e->accent_color < 0) bb_null(&b, "accent_color");
      else bb_int(&b, "accent_color", e->accent_color);
    }
    st = bb_finish(&b, &out->req[out->count], "/users/@me/profile", fields, err);
    if (st != CCORD_OK) {
      ccord_profile_plan_free(out);
      return st;
    }
    out->count++;
  }
  return CCORD_OK;
}

void ccord_profile_plan_free(ccord_profile_plan *plan)
{
  if (!plan) return;
  for (size_t i = 0; i < 2; i++) ccord_free(plan->req[i].body);
  memset(plan, 0, sizeof *plan);
}

bool ccord_profile_cdn_url(const char *kind, ccord_id user_id, const char *hash, char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!kind || user_id == CCORD_ID_NONE || !hash || hash[0] == '\0') return false;
  size_t hlen = strlen(hash);
  if (hlen > 128u || !ccord_utf8_valid_text(hash, hlen)) return false;
  for (size_t i = 0; i < hlen; i++) {
    if (hash[i] == '/' || hash[i] == '?' || hash[i] == '#' || hash[i] == '.' || (unsigned char)hash[i] <= 0x20) return false;
  }
  char idbuf[21];
  (void)ccord_id_format(user_id, idbuf);
  bool anim = hlen > 2u && hash[0] == 'a' && hash[1] == '_';
  int n = snprintf(out, cap, "https://cdn.discordapp.com/%s/%s/%s%s", kind, idbuf, hash,
                   anim ? ".gif?size=64" : ".png?size=64");
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return false;
  }
  return true;
}

// ---- answer parsing -------------------------------------------------------------------------------------------

static void copy_str(char *dst, size_t cap, const char *src, size_t len)
{
  if (cap == 0) return;
  if (len >= cap) len = cap - 1u;
  // Never cut a UTF-8 sequence in half.
  while (len > 0 && (((unsigned char)src[len] & 0xC0u) == 0x80u)) len--;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static const char *field_json_name(ccord_profile_field f)
{
  switch (f) {
    case CCORD_PROFILE_FIELD_DISPLAY_NAME: return "global_name";
    case CCORD_PROFILE_FIELD_AVATAR: return "avatar";
    case CCORD_PROFILE_FIELD_BANNER: return "banner";
    case CCORD_PROFILE_FIELD_BIO: return "bio";
    case CCORD_PROFILE_FIELD_PRONOUNS: return "pronouns";
    case CCORD_PROFILE_FIELD_ACCENT: return "accent_color";
    default: return "";
  }
}

static ccord_profile_field field_from_name(const char *n)
{
  for (unsigned f = 0; f < CCORD_PROFILE_FIELD_COUNT; f++) {
    if (strcmp(n, field_json_name((ccord_profile_field)f)) == 0) return (ccord_profile_field)f;
  }
  return CCORD_PROFILE_FIELD_UNKNOWN;
}

// Reads a string/null member. Returns 0 = member absent or not string/null, 1 = string (copied), 2 = null.
static int member_string(const ccord_json *obj, const char *key, char *dst, size_t cap)
{
  const ccord_json *v = ccord_json_get(obj, key);
  if (!v) return 0;
  if (ccord_json_type_of(v) == CCORD_JSON_NULL) {
    dst[0] = '\0';
    return 2;
  }
  size_t n = 0;
  const char *s = ccord_json_string(v, &n);
  if (!s) return 0;
  copy_str(dst, cap, s, n);
  return 1;
}

static void add_field_error(ccord_profile_edit_result *res, const char *name, const char *code, const char *msg)
{
  if (res->field_error_count >= CCORD_PROFILE_MAX_FIELD_ERRORS) return;
  ccord_profile_field_error *fe = &res->field_errors[res->field_error_count++];
  memset(fe, 0, sizeof *fe);
  fe->field = field_from_name(name);
  copy_str(fe->name, sizeof fe->name, name, strlen(name));
  if (code) copy_str(fe->code, sizeof fe->code, code, strlen(code));
  copy_str(fe->message, sizeof fe->message, msg && msg[0] ? msg : "Invalid value", strlen(msg && msg[0] ? msg : "Invalid value"));
  if (strcmp(name, "password") == 0 || strcmp(name, "new_password") == 0) res->password_required = true;
}

// Walks Discord's `errors` tree: {"banner":{"_errors":[{"code":..,"message":..}]}} (nested objects join with '.').
static void collect_errors(const ccord_json *node, const char *path, unsigned depth, ccord_profile_edit_result *res)
{
  if (!node || depth > 4u || ccord_json_type_of(node) != CCORD_JSON_OBJECT) return;
  const ccord_json *list = ccord_json_get(node, "_errors");
  if (list && path[0] != '\0') {
    size_t n = ccord_json_array_len(list);
    for (size_t i = 0; i < n; i++) {
      const ccord_json *item = ccord_json_array_at(list, i);
      char code[48] = "", msg[200] = "";
      (void)member_string(item, "code", code, sizeof code);
      (void)member_string(item, "message", msg, sizeof msg);
      add_field_error(res, path, code, msg);
    }
  }
  size_t count = ccord_json_object_len(node);
  for (size_t i = 0; i < count; i++) {
    const ccord_json *child = nullptr;
    const char *key = ccord_json_object_key(node, i, &child);
    if (!key || strcmp(key, "_errors") == 0) continue;
    char sub[64];
    if (path[0] == '\0') (void)snprintf(sub, sizeof sub, "%s", key);
    else (void)snprintf(sub, sizeof sub, "%s.%s", path, key);
    collect_errors(child, sub, depth + 1u, res);
  }
}

static ccord_status status_for_http(long http)
{
  if (http == 429) return CCORD_ERR_RANGE;
  if (http == 401 || http == 403) return CCORD_ERR_PERMISSION;
  if (http >= 500) return CCORD_ERR_IO;
  return CCORD_ERR_INVALID;
}

bool ccord_profile_edit_parse_answer(uint32_t fields, long http, const char *body, size_t len,
                                     ccord_profile_edit_result *res)
{
  if (!res) return false;
  res->http_status = http;
  ccord_arena arena;
  ccord_arena_init(&arena, 4096);
  ccord_error jerr;
  const ccord_json *j = (body && len > 0) ? ccord_json_parse(&arena, body, len, nullptr, &jerr) : nullptr;
  bool is_obj = j && ccord_json_type_of(j) == CCORD_JSON_OBJECT;
  bool ok = false;

  if (http >= 200 && http < 300) {
    if (!is_obj) {
      res->status = CCORD_ERR_PARSE;
      (void)snprintf(res->message, sizeof res->message, "Discord's answer could not be read");
    } else {
      ccord_profile_applied *a = &res->applied;
      if (fields & (1u << CCORD_PROFILE_FIELD_DISPLAY_NAME)) {
        char tmp[sizeof a->display_name];
        if (member_string(j, "global_name", tmp, sizeof tmp) != 0) {
          memcpy(a->display_name, tmp, sizeof tmp);
          a->fields |= 1u << CCORD_PROFILE_FIELD_DISPLAY_NAME;
        }
      }
      if ((fields & (1u << CCORD_PROFILE_FIELD_AVATAR)) && member_string(j, "avatar", a->avatar_hash, sizeof a->avatar_hash)) {
        a->fields |= 1u << CCORD_PROFILE_FIELD_AVATAR;
      }
      if ((fields & (1u << CCORD_PROFILE_FIELD_BANNER)) && member_string(j, "banner", a->banner_hash, sizeof a->banner_hash)) {
        a->fields |= 1u << CCORD_PROFILE_FIELD_BANNER;
      }
      if ((fields & (1u << CCORD_PROFILE_FIELD_BIO)) && member_string(j, "bio", a->bio, sizeof a->bio)) {
        a->fields |= 1u << CCORD_PROFILE_FIELD_BIO;
      }
      if ((fields & (1u << CCORD_PROFILE_FIELD_PRONOUNS)) && member_string(j, "pronouns", a->pronouns, sizeof a->pronouns)) {
        a->fields |= 1u << CCORD_PROFILE_FIELD_PRONOUNS;
      }
      if (fields & (1u << CCORD_PROFILE_FIELD_ACCENT)) {
        const ccord_json *v = ccord_json_get(j, "accent_color");
        int64_t n = 0;
        if (v && ccord_json_type_of(v) == CCORD_JSON_NULL) {
          a->accent_color = -1;
          a->fields |= 1u << CCORD_PROFILE_FIELD_ACCENT;
        } else if (v && ccord_json_int64(v, &n) && n >= 0 && n <= (int64_t)CCORD_PROFILE_ACCENT_MAX) {
          a->accent_color = (int32_t)n;
          a->fields |= 1u << CCORD_PROFILE_FIELD_ACCENT;
        }
      }
      res->status = CCORD_OK;
      ok = true;
    }
    ccord_arena_free(&arena);
    return ok;
  }

  // A refusal: keep Discord's own words so the UI can show them.
  res->status = status_for_http(http);
  res->rate_limited = http == 429;
  if (is_obj) {
    char msg[200] = "";
    (void)member_string(j, "message", msg, sizeof msg);
    copy_str(res->message, sizeof res->message, msg, strlen(msg));
    int64_t code = 0;
    const ccord_json *cv = ccord_json_get(j, "code");
    if (cv && ccord_json_int64(cv, &code) && code >= 0 && code <= INT32_MAX) res->code = (int32_t)code;
    const ccord_json *ra = ccord_json_get(j, "retry_after");
    if (ra) {
      double d = ccord_json_number(ra, 0.0);
      if (isfinite(d) && d > 0.0) res->retry_after_s = d;
    }
    if (ccord_json_get(j, "captcha_key") || ccord_json_get(j, "captcha_sitekey")) res->captcha_required = true;
    if (res->code == 60003 || ccord_json_get(j, "mfa")) res->mfa_required = true;
    if (res->code == 50018) res->password_required = true;
    collect_errors(ccord_json_get(j, "errors"), "", 0, res);
    if (res->captcha_required && res->message[0] == '\0') {
      (void)snprintf(res->message, sizeof res->message, "Discord needs a captcha to be solved");
    }
  }
  if (res->message[0] == '\0') {
    if (res->rate_limited) (void)snprintf(res->message, sizeof res->message, "You are being rate limited.");
    else (void)snprintf(res->message, sizeof res->message, "Discord refused the change (HTTP %ld)", http);
  }
  ccord_arena_free(&arena);
  return false;
}

// ---- backends ---------------------------------------------------------------------------------------------------

typedef void (*http_done)(void *ud, ccord_status st, long http, const char *body, size_t len);

#define MOCK_MAX_PENDING 8u

typedef struct mock_pending {
  bool used;
  long http;
  char *body;
  http_done cb;
  void *ud;
} mock_pending;

typedef struct mock_state {
  ccord_id self_id;
  char global_name[CCORD_NAME_MAX_BYTES + 1];
  char avatar[128], banner[128];
  char bio[CCORD_PROFILE_BIO_MAX * 4u + 1u];
  char pronouns[CCORD_PROFILE_PRONOUNS_MAX * 4u + 1u];
  int32_t accent;
  unsigned image_seq;
  size_t count;
  char last_path[64];
  char *last_body;
  bool fail_armed;
  long fail_http;
  char *fail_body;
  size_t pending_count;
  mock_pending pending[MOCK_MAX_PENDING];
} mock_state;

struct ccord_profile_rest {
  bool is_mock;
  mock_state *mock;
#ifdef CCORD_PROFILE_LIVE
  ccord_rest_queue *queue;
#endif
};

#ifdef CCORD_PROFILE_LIVE
ccord_profile_rest *ccord_profile_rest_create_live(const char *base_url, const char *token, ccord_error *err)
{
  ccord_error_clear(err);
  if (!token || token[0] == '\0') {
    ccord_error_set(err, CCORD_ERR_INVALID, "profile: no token");
    return nullptr;
  }
  ccord_profile_rest *r = ccord_calloc(1, sizeof *r);
  if (!r) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "profile: out of memory");
    return nullptr;
  }
  // A 10 MiB image upload needs more than the 15 s API default on a slow uplink.
  ccord_rest_config cfg = {.base_url = base_url, .token = token, .timeout_ms = 90000u};
  r->queue = ccord_rest_queue_create(&cfg, err);
  if (!r->queue) {
    ccord_free(r);
    return nullptr;
  }
  return r;
}
#else
ccord_profile_rest *ccord_profile_rest_create_live(const char *base_url, const char *token, ccord_error *err)
{
  (void)base_url;
  (void)token;
  ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "profile: this build has no live transport");
  return nullptr;
}
#endif

ccord_profile_rest *ccord_profile_rest_create_mock(ccord_id self_id, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_profile_rest *r = ccord_calloc(1, sizeof *r);
  mock_state *m = ccord_calloc(1, sizeof *m);
  if (!r || !m) {
    ccord_free(r);
    ccord_free(m);
    ccord_error_set(err, CCORD_ERR_NOMEM, "profile: out of memory");
    return nullptr;
  }
  m->self_id = self_id;
  m->accent = -1;
  r->is_mock = true;
  r->mock = m;
  return r;
}

void ccord_profile_rest_mock_fail_next(ccord_profile_rest *r, long http_status, const char *json_body)
{
  if (!r || !r->is_mock) return;
  ccord_free(r->mock->fail_body);
  r->mock->fail_body = ccord_strdup(json_body ? json_body : "{}");
  r->mock->fail_http = http_status;
  r->mock->fail_armed = r->mock->fail_body != nullptr;
}

size_t ccord_profile_rest_mock_count(const ccord_profile_rest *r) { return (r && r->is_mock) ? r->mock->count : 0; }
const char *ccord_profile_rest_mock_last_path(const ccord_profile_rest *r)
{
  return (r && r->is_mock) ? r->mock->last_path : "";
}
const char *ccord_profile_rest_mock_last_body(const ccord_profile_rest *r)
{
  return (r && r->is_mock && r->mock->last_body) ? r->mock->last_body : "";
}

// Applies one request body to the mock's account and builds the JSON Discord would answer with.
static char *mock_apply(mock_state *m, const char *path, const char *body, size_t len)
{
  ccord_arena arena;
  ccord_arena_init(&arena, 4096);
  ccord_error jerr;
  const ccord_json *j = ccord_json_parse(&arena, body, len, nullptr, &jerr);
  bool profile = strcmp(path, "/users/@me/profile") == 0;
  if (j && ccord_json_type_of(j) == CCORD_JSON_OBJECT) {
    if (!profile) {
      (void)member_string(j, "global_name", m->global_name, sizeof m->global_name);
      for (int k = 0; k < 2; k++) {
        const char *key = k == 0 ? "avatar" : "banner";
        char *dst = k == 0 ? m->avatar : m->banner;
        const ccord_json *v = ccord_json_get(j, key);
        if (!v) continue;
        if (ccord_json_type_of(v) == CCORD_JSON_NULL) dst[0] = '\0';
        else (void)snprintf(dst, 128, "mock%s%u", k == 0 ? "av" : "bn", ++m->image_seq);
      }
    } else {
      (void)member_string(j, "bio", m->bio, sizeof m->bio);
      (void)member_string(j, "pronouns", m->pronouns, sizeof m->pronouns);
      const ccord_json *v = ccord_json_get(j, "accent_color");
      int64_t n = 0;
      if (v && ccord_json_type_of(v) == CCORD_JSON_NULL) m->accent = -1;
      else if (v && ccord_json_int64(v, &n)) m->accent = (int32_t)n;
    }
  }
  ccord_arena_free(&arena);

  ccord_buf out;
  ccord_buf_init(&out, 0);
  ccord_jw w;
  ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  if (!profile) {
    ccord_jw_key(&w, "id");
    ccord_jw_id(&w, m->self_id);
    ccord_jw_key(&w, "username");
    ccord_jw_cstr(&w, "mock");
    ccord_jw_key(&w, "global_name");
    if (m->global_name[0]) ccord_jw_cstr(&w, m->global_name);
    else ccord_jw_null(&w);
    ccord_jw_key(&w, "avatar");
    if (m->avatar[0]) ccord_jw_cstr(&w, m->avatar);
    else ccord_jw_null(&w);
    ccord_jw_key(&w, "banner");
    if (m->banner[0]) ccord_jw_cstr(&w, m->banner);
    else ccord_jw_null(&w);
  } else {
    ccord_jw_key(&w, "bio");
    ccord_jw_cstr(&w, m->bio);
    ccord_jw_key(&w, "pronouns");
    ccord_jw_cstr(&w, m->pronouns);
  }
  ccord_jw_key(&w, "accent_color");
  if (m->accent >= 0) ccord_jw_int(&w, m->accent);
  else ccord_jw_null(&w);
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&out);
    return nullptr;
  }
  size_t n = 0;
  return ccord_buf_take(&out, &n);
}

static ccord_status rest_patch(ccord_profile_rest *r, const char *path, const char *body, size_t len, http_done cb,
                               void *ud, ccord_error *err)
{
  if (!r->is_mock) {
#ifdef CCORD_PROFILE_LIVE
    ccord_rest_request req = ccord_rest_request_make(CCORD_REST_PATCH, path, CCORD_GOV_USER);
    req.body = body;
    req.body_len = len;
    req.content_type = "application/json";
    req.cb = cb;
    req.ud = ud;
    return ccord_rest_queue_submit(r->queue, &req, err);
#else
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "profile: this build has no live transport");
#endif
  }
  mock_state *m = r->mock;
  mock_pending *slot = nullptr;
  for (size_t i = 0; i < MOCK_MAX_PENDING && !slot; i++) {
    if (!m->pending[i].used) slot = &m->pending[i];
  }
  if (!slot) return ccord_error_set(err, CCORD_ERR_RANGE, "profile: too many requests in flight");
  m->count++;
  (void)snprintf(m->last_path, sizeof m->last_path, "%s", path);
  ccord_free(m->last_body);
  m->last_body = ccord_strdup(body);
  slot->used = true;
  m->pending_count++;
  slot->cb = cb;
  slot->ud = ud;
  if (m->fail_armed) {
    m->fail_armed = false;
    slot->http = m->fail_http;
    slot->body = m->fail_body;
    m->fail_body = nullptr;
  } else {
    slot->http = 200;
    slot->body = mock_apply(m, path, body, len);
    if (!slot->body) slot->http = 500;
  }
  return CCORD_OK;
}

static void mock_deliver(mock_state *m, mock_pending *p, ccord_status st)
{
  mock_pending done = *p;
  memset(p, 0, sizeof *p);
  if (m->pending_count > 0) m->pending_count--;
  if (done.cb) done.cb(done.ud, st, st == CCORD_OK ? done.http : 0, done.body ? done.body : "", done.body ? strlen(done.body) : 0);
  ccord_free(done.body);
}

void ccord_profile_rest_pump(ccord_profile_rest *r, uint64_t now_ms)
{
  if (!r) return;
  if (!r->is_mock) {
#ifdef CCORD_PROFILE_LIVE
    if (!r->queue || ccord_rest_queue_pending(r->queue) == 0) return;
    ccord_rest_queue_pump(r->queue, now_ms);
#else
    (void)now_ms;
#endif
    return;
  }
  if (!r->mock || r->mock->pending_count == 0) return;
  for (size_t i = 0; i < MOCK_MAX_PENDING && r->mock->pending_count > 0; i++) {
    if (r->mock->pending[i].used) mock_deliver(r->mock, &r->mock->pending[i], CCORD_OK);
  }
}

bool ccord_profile_rest_pending(const ccord_profile_rest *r)
{
  if (!r) return false;
  if (r->is_mock) return r->mock && r->mock->pending_count > 0;
#ifdef CCORD_PROFILE_LIVE
  return r->queue && ccord_rest_queue_pending(r->queue) > 0;
#else
  return false;
#endif
}

void ccord_profile_rest_destroy(ccord_profile_rest *r)
{
  if (!r) return;
  if (r->is_mock) {
    for (size_t i = 0; i < MOCK_MAX_PENDING && r->mock->pending_count > 0; i++) {
      if (r->mock->pending[i].used) mock_deliver(r->mock, &r->mock->pending[i], CCORD_ERR_IO);
    }
    ccord_free(r->mock->last_body);
    ccord_free(r->mock->fail_body);
    ccord_free(r->mock);
  }
#ifdef CCORD_PROFILE_LIVE
  if (r->queue) ccord_rest_queue_destroy(r->queue);   // answers every queued request with CCORD_ERR_IO
#endif
  ccord_free(r);
}

// ---- the edit job -----------------------------------------------------------------------------------------------

typedef struct profile_job {
  ccord_profile_rest *rest;
  ccord_profile_plan plan;
  size_t idx;                          // request currently in flight
  ccord_profile_applied sent;          // what we asked for (text fields), used when Discord's answer omits a key
  ccord_profile_edit_result res;
  ccord_profile_edit_cb cb;
  void *ud;
} profile_job;

static void job_finish(profile_job *job)
{
  if (job->cb) job->cb(job->ud, &job->res);
  ccord_profile_plan_free(&job->plan);
  ccord_free(job);
}

static void job_send(profile_job *job);

static void job_on_http(void *ud, ccord_status st, long http, const char *body, size_t len)
{
  profile_job *job = ud;
  const ccord_profile_request *rq = &job->plan.req[job->idx];
  job->res.requests_sent++;
  if (st != CCORD_OK && !(http >= 100)) {
    // The request never got an answer (connection, timeout, queue destroyed) or 429 retries ran out.
    job->res.status = st;
    job->res.http_status = http;
    (void)snprintf(job->res.message, sizeof job->res.message, "%s",
                   st == CCORD_ERR_TIMEOUT ? "Discord did not answer in time" : "Could not reach Discord");
    job_finish(job);
    return;
  }
  bool ok = ccord_profile_edit_parse_answer(rq->fields, http, body, len, &job->res);
  if (!ok) {
    if (st != CCORD_OK && job->res.status == CCORD_OK) job->res.status = st;
    job_finish(job);
    return;
  }
  // Text fields Discord left out of its answer: what we sent is what it stored.
  ccord_profile_applied *a = &job->res.applied;
  uint32_t missing = rq->fields & ~a->fields;
  if (missing & (1u << CCORD_PROFILE_FIELD_DISPLAY_NAME)) memcpy(a->display_name, job->sent.display_name, sizeof a->display_name);
  if (missing & (1u << CCORD_PROFILE_FIELD_BIO)) memcpy(a->bio, job->sent.bio, sizeof a->bio);
  if (missing & (1u << CCORD_PROFILE_FIELD_PRONOUNS)) memcpy(a->pronouns, job->sent.pronouns, sizeof a->pronouns);
  if (missing & (1u << CCORD_PROFILE_FIELD_ACCENT)) a->accent_color = job->sent.accent_color;
  a->fields |= missing & ((1u << CCORD_PROFILE_FIELD_DISPLAY_NAME) | (1u << CCORD_PROFILE_FIELD_BIO) |
                          (1u << CCORD_PROFILE_FIELD_PRONOUNS) | (1u << CCORD_PROFILE_FIELD_ACCENT));
  if (job->idx + 1u < job->plan.count) {
    job->idx++;
    job_send(job);
    return;
  }
  job_finish(job);
}

static void job_send(profile_job *job)
{
  const ccord_profile_request *rq = &job->plan.req[job->idx];
  ccord_error err;
  ccord_status st = rest_patch(job->rest, rq->path, rq->body, rq->body_len, job_on_http, job, &err);
  if (st != CCORD_OK) {
    // Only reachable for the follow-up request (the first is checked in ccord_profile_edit_start).
    job->res.status = st;
    (void)snprintf(job->res.message, sizeof job->res.message, "%s", err.message);
    job_finish(job);
  }
}

ccord_status ccord_profile_edit_start(ccord_profile_rest *r, const ccord_profile_edit *edit, ccord_profile_edit_cb cb,
                                      void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!r) return ccord_error_set(err, CCORD_ERR_INVALID, "profile: no REST backend");
  profile_job *job = ccord_calloc(1, sizeof *job);
  if (!job) return ccord_error_set(err, CCORD_ERR_NOMEM, "profile: out of memory");
  ccord_status st = ccord_profile_edit_plan(edit, &job->plan, err);
  if (st != CCORD_OK) {
    ccord_free(job);
    return st;
  }
  job->rest = r;
  job->cb = cb;
  job->ud = ud;
  job->res.applied.accent_color = -1;
  job->sent.accent_color = edit->has_accent ? edit->accent_color : -1;
  if (edit->display_name) copy_str(job->sent.display_name, sizeof job->sent.display_name, edit->display_name, strlen(edit->display_name));
  if (edit->bio) copy_str(job->sent.bio, sizeof job->sent.bio, edit->bio, strlen(edit->bio));
  if (edit->pronouns) copy_str(job->sent.pronouns, sizeof job->sent.pronouns, edit->pronouns, strlen(edit->pronouns));
  const ccord_profile_request *rq = &job->plan.req[0];
  st = rest_patch(r, rq->path, rq->body, rq->body_len, job_on_http, job, err);
  if (st != CCORD_OK) {
    ccord_profile_plan_free(&job->plan);
    ccord_free(job);
    return st;
  }
  return CCORD_OK;
}
