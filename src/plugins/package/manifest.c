// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include <ccord/plugins/manifest.h>

#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>
#include <ccord/core/config.h>

#include <stdio.h>
#include <string.h>

#define MANIFEST_MAX_BYTES (1u << 20)

static ccord_status bad(ccord_error *err, ccord_status code, const char *msg)
{
  ccord_error_set(err, code, "%s", msg);
  return code;
}

static bool is_lower_or_digit(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// ^[a-z0-9][a-z0-9._-]{0,63}$ and no ".."
static bool valid_id(const char *s, size_t n)
{
  if (n == 0 || n > 64 || !is_lower_or_digit(s[0])) return false;
  for (size_t i = 1; i < n; i++) {
    char c = s[i];
    if (!(is_lower_or_digit(c) || c == '.' || c == '_' || c == '-')) return false;
    if (c == '.' && i + 1 < n && s[i + 1] == '.') return false;
  }
  return true;
}

// MAJOR.MINOR.PATCH, decimal digits only.
static bool valid_version(const char *s, size_t n)
{
  if (n == 0 || n > 32) return false;
  int parts = 0;
  size_t seg = 0;
  for (size_t i = 0; i <= n; i++) {
    if (i == n || s[i] == '.') {
      if (seg == 0) return false;
      parts++;
      seg = 0;
      continue;
    }
    if (s[i] < '0' || s[i] > '9') return false;
    seg++;
  }
  return parts == 3;
}

// Printable text: valid UTF-8, no control characters.
static bool valid_text(const char *s, size_t n)
{
  if (!ccord_utf8_valid(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

// ^[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*\.lua$, <= 128 bytes
static bool valid_main(const char *s, size_t n)
{
  if (n < 5 || n > 128 || strcmp(s + n - 4, ".lua") != 0) return false;
  size_t body = n - 4;
  if (body == 0 || s[0] == '/') return false;
  for (size_t i = 0; i < body; i++) {
    char c = s[i];
    if (c == '/') {
      if (i == 0 || i + 1 >= body || s[i + 1] == '/') return false;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

static ccord_status get_string(const ccord_json *obj, const char *key, const char *max_key, char *dst, size_t dst_cap,
                               size_t max_len, bool required, size_t *out_len, ccord_error *err)
{
  const ccord_json *j = ccord_json_get(obj, key);
  if (!j) {
    if (required) {
      ccord_error_set(err, CCORD_ERR_PARSE, "plugin.json: missing '%s'", key);
      return CCORD_ERR_PARSE;
    }
    return CCORD_OK;
  }
  size_t n = 0;
  const char *s = ccord_json_string(j, &n);
  if (!s) {
    ccord_error_set(err, CCORD_ERR_PARSE, "plugin.json: '%s' must be a string", key);
    return CCORD_ERR_PARSE;
  }
  if (n > max_len || n >= dst_cap) {
    ccord_error_set(err, CCORD_ERR_INVALID, "plugin.json: '%s' longer than %s", key, max_key);
    return CCORD_ERR_INVALID;
  }
  memcpy(dst, s, n);
  dst[n] = '\0';
  if (out_len) *out_len = n;
  return CCORD_OK;
}

static ccord_status get_int(const ccord_json *obj, const char *key, int64_t *out, bool *present, ccord_error *err)
{
  const ccord_json *j = ccord_json_get(obj, key);
  *present = j != nullptr;
  if (!j) return CCORD_OK;
  if (!ccord_json_int64(j, out)) {
    ccord_error_set(err, CCORD_ERR_PARSE, "plugin.json: '%s' must be an integer", key);
    return CCORD_ERR_PARSE;
  }
  return CCORD_OK;
}

static bool key_in(const char *k, const char *const *list, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    if (strcmp(k, list[i]) == 0) return true;
  }
  return false;
}

static ccord_status parse_limits(const ccord_json *lim, ccord_manifest *out, ccord_error *err)
{
  if (ccord_json_type_of(lim) != CCORD_JSON_OBJECT) return bad(err, CCORD_ERR_PARSE, "plugin.json: 'limits' must be an object");
  // Limits are no longer enforced; kept for schema v1 compatibility.
  out->memory_bytes = 0;
  out->instructions = 0;
  return CCORD_OK;
}

static ccord_status parse_permissions(const ccord_json *arr, ccord_manifest *out, ccord_error *err)
{
  if (ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return bad(err, CCORD_ERR_PARSE, "plugin.json: 'permissions' must be an array");
  size_t n = ccord_json_array_len(arr);
  out->perms = 0;
  for (size_t i = 0; i < n; i++) {
    size_t len = 0;
    const char *s = ccord_json_string(ccord_json_array_at(arr, i), &len);
    if (!s) return bad(err, CCORD_ERR_PARSE, "plugin.json: permission must be a string");
    ccord_perm p;
    if (ccord_perm_from_name(s, &p)) {
      if (out->perms & (uint64_t)p) return bad(err, CCORD_ERR_INVALID, "plugin.json: duplicate permission");
      out->perms |= (uint64_t)p;
      continue;
    }
    ccord_error_set(err, CCORD_ERR_INVALID, "plugin.json: unknown permission '%.32s'", s);
    return CCORD_ERR_INVALID;
  }
  return CCORD_OK;
}

typedef struct cap_entry {
  const char *name;
  uint64_t bit;
} cap_entry;

static const cap_entry k_caps[] = {
  {"touch", CCORD_CAP_TOUCH},
  {"haptics", CCORD_CAP_HAPTICS},
  {"share", CCORD_CAP_SHARE},
  {"notifications", CCORD_CAP_NOTIFICATIONS},
  {"tray", CCORD_CAP_TRAY},
  {"global_shortcuts", CCORD_CAP_GLOBAL_SHORTCUTS},
  {"screen_capture", CCORD_CAP_SCREEN_CAPTURE},
  {"rpc", CCORD_CAP_RPC},
  {"hover", CCORD_CAP_HOVER},
  {"safe_area", CCORD_CAP_SAFE_AREA},
  {"keyboard_control", CCORD_CAP_KEYBOARD_CONTROL},
  {"orientation_control", CCORD_CAP_ORIENTATION_CONTROL},
  {"keep_screen_on", CCORD_CAP_KEEP_SCREEN_ON},
  {"panels", CCORD_CAP_PANELS},
};

uint32_t ccord_platform_current_bit(void)
{
#if CCORD_OS_WINDOWS
  return CCORD_PLAT_WINDOWS;
#elif CCORD_OS_MACOS
  return CCORD_PLAT_MACOS;
#elif CCORD_OS_ANDROID
  return CCORD_PLAT_ANDROID;
#else
  return CCORD_PLAT_LINUX;
#endif
}

bool ccord_cap_from_name(const char *name, uint64_t *out_bit)
{
  if (!name) return false;
  for (size_t i = 0; i < sizeof k_caps / sizeof k_caps[0]; i++) {
    if (strcmp(name, k_caps[i].name) == 0) {
      if (out_bit) *out_bit = k_caps[i].bit;
      return true;
    }
  }
  return false;
}

const char *ccord_cap_name(uint64_t bit)
{
  for (size_t i = 0; i < sizeof k_caps / sizeof k_caps[0]; i++) {
    if (k_caps[i].bit == bit) return k_caps[i].name;
  }
  return nullptr;
}

bool ccord_manifest_supports(const ccord_manifest *m, uint32_t plat_bit, uint64_t avail_caps,
                             char *reason_out, size_t reason_cap)
{
  if (reason_out && reason_cap > 0) reason_out[0] = '\0';
  if (!m) return false;
  uint32_t plats = m->platforms ? m->platforms : CCORD_PLAT_ALL;
  if ((plats & plat_bit) == 0) {
    if (reason_out && reason_cap > 0) {
      const char *pname = plat_bit == CCORD_PLAT_ANDROID ? "android"
                        : plat_bit == CCORD_PLAT_MACOS   ? "macos"
                        : plat_bit == CCORD_PLAT_WINDOWS ? "windows"
                        : plat_bit == CCORD_PLAT_LINUX   ? "linux"
                                                         : ccord_os_name();
      snprintf(reason_out, reason_cap, "not supported on platform '%s'", pname);
    }
    return false;
  }
  uint64_t missing = m->required_caps & ~avail_caps;
  if (missing != 0) {
    if (reason_out && reason_cap > 0) {
      const char *cname = "capability";
      for (size_t i = 0; i < sizeof k_caps / sizeof k_caps[0]; i++) {
        if (missing & k_caps[i].bit) { cname = k_caps[i].name; break; }
      }
      snprintf(reason_out, reason_cap, "requires unavailable capability '%s'", cname);
    }
    return false;
  }
  return true;
}

static ccord_status parse_platforms(const ccord_json *arr, ccord_manifest *out, ccord_error *err)
{
  if (ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    return bad(err, CCORD_ERR_PARSE, "plugin.json: 'platforms' must be an array");
  }
  size_t n = ccord_json_array_len(arr);
  if (n == 0) {
    out->platforms = CCORD_PLAT_ALL;
    return CCORD_OK;
  }
  uint32_t mask = 0;
  for (size_t i = 0; i < n; i++) {
    size_t sl = 0;
    const char *s = ccord_json_string(ccord_json_array_at(arr, i), &sl);
    if (!s || sl == 0) return bad(err, CCORD_ERR_PARSE, "plugin.json: platform entry must be a string");
    if (strcmp(s, "linux") == 0) mask |= CCORD_PLAT_LINUX;
    else if (strcmp(s, "macos") == 0 || strcmp(s, "darwin") == 0) mask |= CCORD_PLAT_MACOS;
    else if (strcmp(s, "windows") == 0) mask |= CCORD_PLAT_WINDOWS;
    else if (strcmp(s, "android") == 0) mask |= CCORD_PLAT_ANDROID;
    else if (strcmp(s, "desktop") == 0) mask |= CCORD_PLAT_DESKTOP;
    else if (strcmp(s, "mobile") == 0) mask |= CCORD_PLAT_MOBILE;
    else if (strcmp(s, "all") == 0) mask |= CCORD_PLAT_ALL;
    else {
      ccord_error_set(err, CCORD_ERR_INVALID, "plugin.json: unknown platform '%.32s'", s);
      return CCORD_ERR_INVALID;
    }
  }
  out->platforms = mask;
  return CCORD_OK;
}

static ccord_status parse_required_caps(const ccord_json *arr, ccord_manifest *out, ccord_error *err)
{
  if (ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    return bad(err, CCORD_ERR_PARSE, "plugin.json: 'requires_capabilities' must be an array");
  }
  size_t n = ccord_json_array_len(arr);
  uint64_t mask = 0;
  for (size_t i = 0; i < n; i++) {
    size_t sl = 0;
    const char *s = ccord_json_string(ccord_json_array_at(arr, i), &sl);
    uint64_t bit = 0;
    if (!s || sl == 0 || !ccord_cap_from_name(s, &bit)) {
      ccord_error_set(err, CCORD_ERR_INVALID, "plugin.json: unknown capability '%.32s'", s ? s : "");
      return CCORD_ERR_INVALID;
    }
    mask |= bit;
  }
  out->required_caps = mask;
  return CCORD_OK;
}

static ccord_status parse_allowed_hosts(const ccord_json *arr, ccord_manifest *out, ccord_error *err)
{
  if (ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    return bad(err, CCORD_ERR_PARSE, "plugin.json: 'allowed_hosts' must be an array");
  }
  size_t n = ccord_json_array_len(arr);
  if (n > CCORD_MANIFEST_MAX_HOSTS) {
    return bad(err, CCORD_ERR_INVALID, "plugin.json: 'allowed_hosts' exceeds 32 entries");
  }
  out->allowed_host_count = 0;
  for (size_t i = 0; i < n; i++) {
    size_t sl = 0;
    const char *s = ccord_json_string(ccord_json_array_at(arr, i), &sl);
    if (!s || sl == 0 || sl >= CCORD_MANIFEST_HOST_CAP) {
      return bad(err, CCORD_ERR_INVALID, "plugin.json: invalid entry in 'allowed_hosts'");
    }
    for (size_t k = 0; k < sl; k++) {
      char c = s[k];
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '-' || (c == '*' && k == 0 && sl > 2 && s[1] == '.');
      if (!ok) return bad(err, CCORD_ERR_INVALID, "plugin.json: invalid host pattern in 'allowed_hosts'");
    }
    memcpy(out->allowed_hosts[out->allowed_host_count], s, sl);
    out->allowed_hosts[out->allowed_host_count][sl] = '\0';
    out->allowed_host_count++;
  }
  return CCORD_OK;
}

static char host_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool host_ieq(const char *a, const char *b)
{
  while (*a && *b) {
    if (host_lower(*a++) != host_lower(*b++)) return false;
  }
  return *a == '\0' && *b == '\0';
}

bool ccord_manifest_host_allowed(const ccord_manifest *m, const char *host)
{
  if (!m || !host || !host[0]) return false;
  if (m->allowed_host_count == 0) return true;
  size_t hlen = strlen(host);
  for (size_t i = 0; i < m->allowed_host_count; i++) {
    const char *pat = m->allowed_hosts[i];
    if (pat[0] == '*' && pat[1] == '.') {
      const char *suf = pat + 1;   // ".example.com"
      size_t slen = strlen(suf);
      if (hlen > slen && host_ieq(host + hlen - slen, suf)) return true;
    } else if (host_ieq(host, pat)) {
      return true;
    }
  }
  return false;
}

ccord_status ccord_manifest_parse(const char *json, size_t len, ccord_manifest *out, ccord_error *err)
{
  if (!out) return bad(err, CCORD_ERR_INVALID, "plugin.json: null output");
  memset(out, 0, sizeof *out);
  out->platforms = CCORD_PLAT_ALL;
  if (!json || len == 0 || len > MANIFEST_MAX_BYTES) return bad(err, CCORD_ERR_RANGE, "plugin.json: size must be 1..16 KiB");

  static const char *const top_keys[] = {
    "schema", "id", "name", "version", "api", "main", "description", "license",
    "permissions", "limits", "platforms", "requires_capabilities", "allowed_hosts",
  };
  ccord_arena arena;
  ccord_arena_init(&arena, len < 2048u ? 2048u : 0u);
  ccord_error pe = {0};
  const ccord_json_limits lim = {16, 8192, 1u << 20};
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!root) {
    ccord_arena_free(&arena);
    return bad(err, pe.code == CCORD_ERR_NOMEM ? CCORD_ERR_NOMEM : CCORD_ERR_PARSE, "plugin.json: invalid JSON");
  }
  ccord_status st = CCORD_OK;
  if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    st = bad(err, CCORD_ERR_PARSE, "plugin.json: root must be an object");
    goto done;
  }
  for (size_t i = 0; i < ccord_json_object_len(root); i++) {
    const ccord_json *v = nullptr;
    const char *k = ccord_json_object_key(root, i, &v);
    if (!key_in(k, top_keys, sizeof top_keys / sizeof top_keys[0])) {
      st = bad(err, CCORD_ERR_PARSE, "plugin.json: unknown top-level key");
      goto done;
    }
  }

  int64_t iv = 0;
  bool present = false;
  if ((st = get_int(root, "schema", &iv, &present, err)) != CCORD_OK) goto done;
  if (!present || iv != 1) { st = bad(err, CCORD_ERR_PARSE, "plugin.json: schema must be 1"); goto done; }
  out->schema_version = (uint32_t)iv;
  if ((st = get_int(root, "api", &iv, &present, err)) != CCORD_OK) goto done;
  if (!present) { st = bad(err, CCORD_ERR_PARSE, "plugin.json: missing 'api'"); goto done; }
  if (iv != CCORD_PLUGIN_API_VERSION) {
    ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "plugin needs API %lld, host provides %d", (long long)iv, CCORD_PLUGIN_API_VERSION);
    st = CCORD_ERR_UNSUPPORTED;
    goto done;
  }
  out->api_version = (uint32_t)iv;

  // Strings: validated after copy-out so the lengths are known.
  size_t slen = 0;
  if ((st = get_string(root, "id", "64 bytes", out->id, sizeof out->id, 64, true, &slen, err)) != CCORD_OK) goto done;
  if (!valid_id(out->id, slen)) { st = bad(err, CCORD_ERR_INVALID, "plugin.json: invalid 'id'"); goto done; }
  slen = 0;
  if ((st = get_string(root, "name", "64 bytes", out->name, sizeof out->name, 64, true, &slen, err)) != CCORD_OK) goto done;
  if (!valid_text(out->name, slen) || slen == 0) {
    st = bad(err, CCORD_ERR_INVALID, "plugin.json: 'name' must be 1-64 bytes of text");
    goto done;
  }
  slen = 0;
  if ((st = get_string(root, "version", "32 bytes", out->version, sizeof out->version, 32, true, &slen, err)) != CCORD_OK) goto done;
  if (!valid_version(out->version, slen)) { st = bad(err, CCORD_ERR_INVALID, "plugin.json: 'version' must be MAJOR.MINOR.PATCH"); goto done; }
  slen = 0;
  if ((st = get_string(root, "description", "200 bytes", out->description, sizeof out->description, 200, false, &slen, err)) != CCORD_OK) goto done;
  if ((st = get_string(root, "license", "32 bytes", out->license, sizeof out->license, 32, false, nullptr, err)) != CCORD_OK) goto done;
  if (slen > 0 && !valid_text(out->description, slen)) {
    st = bad(err, CCORD_ERR_INVALID, "plugin.json: 'description' must be text");
    goto done;
  }
  strcpy(out->main, "main.lua");
  slen = sizeof("main.lua") - 1u;
  if ((st = get_string(root, "main", "128 bytes", out->main, sizeof out->main, 128, false, &slen, err)) != CCORD_OK) goto done;
  if (!valid_main(out->main, slen)) { st = bad(err, CCORD_ERR_INVALID, "plugin.json: invalid 'main'"); goto done; }

  const ccord_json *perms = ccord_json_get(root, "permissions");
  if (perms) {
    if ((st = parse_permissions(perms, out, err)) != CCORD_OK) goto done;
  } else {
    out->perms = 0;
  }

  const ccord_json *limits = ccord_json_get(root, "limits");
  if (limits) {
    if ((st = parse_limits(limits, out, err)) != CCORD_OK) goto done;
  }
  const ccord_json *plats = ccord_json_get(root, "platforms");
  if (plats) {
    if ((st = parse_platforms(plats, out, err)) != CCORD_OK) goto done;
  }
  const ccord_json *req_caps = ccord_json_get(root, "requires_capabilities");
  if (req_caps) {
    if ((st = parse_required_caps(req_caps, out, err)) != CCORD_OK) goto done;
  }
  const ccord_json *hosts = ccord_json_get(root, "allowed_hosts");
  if (hosts) {
    if ((st = parse_allowed_hosts(hosts, out, err)) != CCORD_OK) goto done;
  }
  out->memory_bytes = 0;
  out->instructions = 0;

done:
  ccord_arena_free(&arena);
  return st;
}
