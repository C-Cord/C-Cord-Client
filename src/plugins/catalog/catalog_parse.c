// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "catalog_internal.h"

static bool parse_semver_triplet(const char *s, uint64_t out[3])
{
  if (!s || !s[0]) return false;
  out[0] = out[1] = out[2] = 0;
  int idx = 0;
  const char *p = s;
  while (*p && idx < 3) {
    if (!isdigit((unsigned char)*p)) return false;
    uint64_t v = 0;
    while (isdigit((unsigned char)*p)) {
      v = v * 10u + (uint64_t)(*p - '0');
      p++;
    }
    out[idx++] = v;
    if (*p == '.') {
      p++;
      if (!*p) return false;
    } else if (*p != '\0') {
      return false;
    }
  }
  return idx == 3 && *p == '\0';
}

int ccord_catalog_semver_cmp(const char *a, const char *b)
{
  uint64_t va[3], vb[3];
  if (!parse_semver_triplet(a, va) || !parse_semver_triplet(b, vb)) {
    return strcmp(a ? a : "", b ? b : "");
  }
  for (int i = 0; i < 3; i++) {
    if (va[i] < vb[i]) return -1;
    if (va[i] > vb[i]) return 1;
  }
  return 0;
}

bool ccord_catalog_sha256_file_hex(const char *path, char hex_out[65])
{
  if (!path || !hex_out) return false;
  FILE *fp = fopen(path, "rb");
  if (!fp) return false;
  ccord_sha256_ctx ctx;
  ccord_sha256_init(&ctx);
  unsigned char buf[16384];
  size_t rd = 0;
  while ((rd = fread(buf, 1, sizeof buf, fp)) > 0) {
    ccord_sha256_update(&ctx, buf, rd);
  }
  bool err = ferror(fp) != 0;
  fclose(fp);
  if (err) return false;
  unsigned char digest[32];
  ccord_sha256_final(&ctx, digest);
  static const char k_hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; i++) {
    hex_out[i * 2] = k_hex[digest[i] >> 4];
    hex_out[i * 2 + 1] = k_hex[digest[i] & 0x0f];
  }
  hex_out[64] = '\0';
  return true;
}

bool cat_hex_equal_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  if (strlen(a) != 64 || strlen(b) != 64) return false;
  for (size_t i = 0; i < 64; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}

static bool host_csv_contains_ci(const char *csv, const char *tok, size_t tok_len)
{
  if (!csv || !tok || tok_len == 0) return false;
  const char *p = csv;
  while (*p) {
    while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
    const char *s = p;
    while (*p && *p != ',') p++;
    const char *e = p;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    size_t len = (size_t)(e - s);
    if (len == tok_len) {
      bool eq = true;
      for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)tok[i])) {
          eq = false;
          break;
        }
      }
      if (eq) return true;
    }
  }
  return false;
}

static size_t host_csv_count(const char *csv)
{
  if (!csv) return 0;
  size_t n = 0;
  const char *p = csv;
  while (*p) {
    while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
    const char *s = p;
    while (*p && *p != ',') p++;
    const char *e = p;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    if (e > s) n++;
  }
  return n;
}

bool ccord_hosts_expanded(const char *consented_csv, bool had_consented, const char *new_csv, uint64_t new_perms)
{
  const uint64_t net_mask = (uint64_t)CCORD_PERM_NETWORK_HTTP | (uint64_t)CCORD_PERM_NETWORK_WS;
  if ((new_perms & net_mask) == 0) return false;
  if (!had_consented) return false;
  if (host_csv_count(consented_csv) == 0) return false; // previously unrestricted
  if (host_csv_count(new_csv) == 0) return true;        // became unrestricted!

  const char *p = new_csv;
  while (*p) {
    while (*p && (*p == ',' || isspace((unsigned char)*p))) p++;
    const char *s = p;
    while (*p && *p != ',') p++;
    const char *e = p;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    if (e > s && !host_csv_contains_ci(consented_csv, s, (size_t)(e - s))) {
      return true;
    }
  }
  return false;
}

static bool json_bool_or_int(const ccord_json *v)
{
  if (!v) return false;
  if (ccord_json_type_of(v) == CCORD_JSON_BOOL) return ccord_json_bool(v, false);
  int64_t n = 0;
  if (ccord_json_int64(v, &n)) return n != 0;
  return false;
}

static void parse_signature_obj(const ccord_json *obj, cat_item *out)
{
  if (!obj || !out) return;
  const ccord_json *sig = ccord_json_get(obj, "signature");
  if (!sig || ccord_json_type_of(sig) != CCORD_JSON_OBJECT) return;
  cat_json_str_copy(sig, "alg", out->sig_alg, sizeof out->sig_alg);
  cat_json_str_copy(sig, "key_id", out->sig_key_id, sizeof out->sig_key_id);
  cat_json_str_copy(sig, "sig", out->sig_hex, sizeof out->sig_hex);
  out->has_signature = (out->sig_alg[0] != '\0' && out->sig_hex[0] != '\0');
}

static int cmp_cstr_ptr(const void *a, const void *b)
{
  const char *const *sa = a;
  const char *const *sb = b;
  return strcmp(*sa, *sb);
}

static bool append_sorted_unique_json_csv(ccord_buf *buf, const ccord_json *arr)
{
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return true;
  size_t n = ccord_json_array_len(arr);
  if (n == 0) return true;
  const char **items = ccord_alloc_array(n, sizeof *items);
  if (!items) return false;
  size_t cnt = 0;
  for (size_t i = 0; i < n; i++) {
    size_t sl = 0;
    const char *s = ccord_json_string(ccord_json_array_at(arr, i), &sl);
    if (!s || sl == 0) continue;
    items[cnt++] = s;
  }
  if (cnt > 1) qsort(items, cnt, sizeof *items, cmp_cstr_ptr);
  bool ok = true;
  bool first = true;
  for (size_t i = 0; i < cnt && ok; i++) {
    if (i > 0 && strcmp(items[i], items[i - 1]) == 0) continue;
    if (!first) ok = ccord_buf_append(buf, ",", 1) == CCORD_OK;
    if (ok) ok = ccord_buf_append_str(buf, items[i]) == CCORD_OK;
    first = false;
  }
  ccord_free(items);
  return ok;
}

static bool verify_plugin_item_signature(const ccord_catalog *cat, const cat_item *it,
                                         const ccord_json *perms_arr, const ccord_json *hosts_arr)
{
  if (!cat || cat->npubkeys == 0) return true;
  if (!it || !it->has_signature || strlen(it->sha256) != 64) return false;
  char sha_lo[65];
  for (size_t i = 0; i < 64; i++) {
    sha_lo[i] = (char)tolower((unsigned char)it->sha256[i]);
  }
  sha_lo[64] = '\0';

  ccord_buf b;
  ccord_buf_init(&b, CATALOG_MAX_API_BYTES);
  bool ok = ccord_buf_appendf(&b, "ccord-plugin-v2\n%s\n%d\n%s\n%s\n%s\n",
                              it->id, (int)it->tier, it->version, sha_lo, it->download_url) == CCORD_OK;
  if (ok) ok = append_sorted_unique_json_csv(&b, perms_arr);
  if (ok) ok = ccord_buf_append(&b, "\n", 1) == CCORD_OK;
  if (ok) ok = append_sorted_unique_json_csv(&b, hosts_arr);
  if (ok) {
    ok = ccord_buf_appendf(&b, "\n%d\n%d\n%" PRId64 "\n%" PRId64 "\n",
                           (int)it->warning_sev, it->needs_tier_review ? 1 : 0,
                           it->author_github_id, it->repo_id) == CCORD_OK;
  }
  bool verified = false;
  if (ok) {
    verified = cat_verify_ed25519(cat, it->sig_alg, it->sig_key_id, it->sig_hex,
                                  (const uint8_t *)b.data, b.len);
  }
  ccord_buf_free(&b);
  return verified;
}

// ---------------------------------------------------------------- API v2 JSON parsers

static void parse_permissions_array(const ccord_json *perms, cat_item *it)
{
  it->perms_mask = 0;
  it->permissions[0] = '\0';
  if (!perms || ccord_json_type_of(perms) != CCORD_JSON_ARRAY) return;
  size_t pc = ccord_json_array_len(perms);
  size_t off = 0;
  for (size_t k = 0; k < pc; k++) {
    size_t sl = 0;
    const char *ps = ccord_json_string(ccord_json_array_at(perms, k), &sl);
    if (!ps || sl == 0) continue;
    ccord_perm perm_bit = 0;
    if (ccord_perm_from_name(ps, &perm_bit)) {
      it->perms_mask |= (uint64_t)perm_bit;
    }
    if (off > 0 && off + 2 < sizeof it->permissions) {
      it->permissions[off++] = ',';
      it->permissions[off++] = ' ';
    }
    if (off + sl < sizeof it->permissions) {
      memcpy(it->permissions + off, ps, sl);
      off += sl;
      it->permissions[off] = '\0';
    }
  }
}

static uint32_t parse_platforms_array(const ccord_json *arr)
{
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return CCORD_PLAT_ALL;
  size_t n = ccord_json_array_len(arr);
  if (n == 0) return CCORD_PLAT_ALL;
  uint32_t mask = 0;
  for (size_t i = 0; i < n; i++) {
    size_t sl = 0;
    const char *s = ccord_json_string(ccord_json_array_at(arr, i), &sl);
    if (!s || sl == 0) continue;
    if (strcmp(s, "linux") == 0) mask |= CCORD_PLAT_LINUX;
    else if (strcmp(s, "macos") == 0 || strcmp(s, "darwin") == 0) mask |= CCORD_PLAT_MACOS;
    else if (strcmp(s, "windows") == 0) mask |= CCORD_PLAT_WINDOWS;
    else if (strcmp(s, "android") == 0) mask |= CCORD_PLAT_ANDROID;
    else if (strcmp(s, "desktop") == 0) mask |= CCORD_PLAT_DESKTOP;
    else if (strcmp(s, "mobile") == 0) mask |= CCORD_PLAT_MOBILE;
    else if (strcmp(s, "all") == 0) mask |= CCORD_PLAT_ALL;
  }
  return mask ? mask : CCORD_PLAT_ALL;
}

static void parse_hosts_array(const ccord_json *arr, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return;
  size_t n = ccord_json_array_len(arr);
  size_t off = 0;
  for (size_t i = 0; i < n; i++) {
    size_t sl = 0;
    const char *s = ccord_json_string(ccord_json_array_at(arr, i), &sl);
    if (!s || sl == 0) continue;
    if (off > 0 && off + 2 < cap) {
      out[off++] = ',';
      out[off++] = ' ';
    }
    if (off + sl < cap) {
      memcpy(out + off, s, sl);
      off += sl;
      out[off] = '\0';
    }
  }
}

bool cat_extract_icon_filename(const char *icon_url, char out[32])
{
  if (!out) return false;
  out[0] = '\0';
  if (!icon_url || !icon_url[0]) return false;
  if (strncmp(icon_url, "https://", 8) != 0 && strncmp(icon_url, "http://", 7) != 0) return false;
  const char *slash = strrchr(icon_url, '/');
  if (!slash || !slash[1]) return false;
  const char *name = slash + 1;
  size_t n = strlen(name);
  if (n != 20 && n != 21) return false;
  for (size_t i = 0; i < 16; i++) {
    char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  if (n == 20 && strcmp(name + 16, ".png") != 0) return false;
  if (n == 21 && strcmp(name + 16, ".webp") != 0) return false;
  memcpy(out, name, n + 1);
  return true;
}

bool cat_resolve_cached_icon_path(const ccord_catalog *cat, const char *icon_url, char *out, size_t cap)
{
  if (out && cap > 0) out[0] = '\0';
  if (!cat || !cat->has_data_dir || !icon_url || !icon_url[0] || !out || cap == 0) return false;
  char fname[32];
  if (!cat_extract_icon_filename(icon_url, fname)) return false;
  int n = snprintf(out, cap, "%s/plugins/icon_cache/%s", cat->data_dir, fname);
  if (n < 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return false;
  }
  uint64_t fsz = 0;
  if (ccord_path_is_file(out) && ccord_file_size(out, &fsz) && fsz > 0 && fsz <= CCORD_MANIFEST_MAX_ICON_BYTES) {
    return true;
  }
  out[0] = '\0';
  return false;
}

static void parse_warnings_array(const ccord_json *warns, cat_item *it)
{
  it->warning_sev = CCORD_WARN_NONE;
  it->warning_title[0] = '\0';
  it->warning_message[0] = '\0';
  if (!warns || ccord_json_type_of(warns) != CCORD_JSON_ARRAY) return;
  size_t wc = ccord_json_array_len(warns);
  for (size_t k = 0; k < wc; k++) {
    const ccord_json *wo = ccord_json_array_at(warns, k);
    if (!wo || ccord_json_type_of(wo) != CCORD_JSON_OBJECT) continue;
    char sev_s[32];
    cat_json_str_copy(wo, "severity", sev_s, sizeof sev_s);
    ccord_catalog_warn_sev sev = CCORD_WARN_CAUTION;
    if (strcmp(sev_s, "danger") == 0) sev = CCORD_WARN_DANGER;
    else if (strcmp(sev_s, "info") == 0) sev = CCORD_WARN_INFO;
    if (sev >= it->warning_sev) {
      it->warning_sev = sev;
      cat_json_str_copy(wo, "title", it->warning_title, sizeof it->warning_title);
      cat_json_str_copy(wo, "message", it->warning_message, sizeof it->warning_message);
    }
  }
}

bool cat_valid_catalog_id(const char *s)
{
  if (!s || !s[0]) return false;
  size_t n = strlen(s);
  if (n > 64 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return false;
  if (strstr(s, "..")) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool cat_valid_catalog_version(const char *s)
{
  if (!s || !s[0]) return false;
  size_t n = strlen(s);
  if (n > 32 || s[0] == '.' || strstr(s, "..")) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_' || c == '+';
    if (!ok) return false;
  }
  return true;
}

// Parse GET /api/v2/plugins slim card response
bool cat_parse_discover_page_json(ccord_catalog *cat, const char *json, size_t len, bool append)
{
  if (!cat || !json || len == 0) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {12, 16384, CATALOG_MAX_API_BYTES};
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }

  int64_t total = 0;
  (void)ccord_json_int64(ccord_json_get(root, "total"), &total);
  const ccord_json *hm = ccord_json_get(root, "has_more");
  bool has_more = hm ? ccord_json_bool(hm, false) : false;
  char next_cur[256];
  cat_json_str_copy(root, "next_cursor", next_cur, sizeof next_cur);

  const ccord_json *arr = ccord_json_get(root, "plugins");
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    ccord_arena_free(&arena);
    return false;
  }
  size_t pn = ccord_json_array_len(arr);
  if (!append) {
    cat->ndiscover = 0;
  }
  if (pn > 0 && cat->ndiscover + pn > cat->capdiscover) {
    size_t ncap = cat->ndiscover + pn;
    cat_item *g = ccord_realloc_array(cat->discover, ncap, sizeof *g);
    if (!g) {
      ccord_arena_free(&arena);
      return false;
    }
    cat->discover = g;
    cat->capdiscover = ncap;
  }

  for (size_t i = 0; i < pn; i++) {
    const ccord_json *o = ccord_json_array_at(arr, i);
    if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) continue;
    cat_item it = {0};
    cat_json_str_copy(o, "id", it.id, sizeof it.id);
    cat_json_str_copy(o, "name", it.name, sizeof it.name);
    cat_json_str_copy(o, "description", it.description, sizeof it.description);
    cat_json_str_copy(o, "license", it.license, sizeof it.license);
    cat_json_str_copy(o, "author_login", it.author, sizeof it.author);
    cat_json_str_copy(o, "author_repo", it.repo, sizeof it.repo);
    cat_json_str_copy(o, "author_repo_url", it.repo_url, sizeof it.repo_url);
    cat_json_str_copy(o, "version", it.version, sizeof it.version);
    cat_json_str_copy(o, "warning_title", it.warning_title, sizeof it.warning_title);
    cat_json_str_copy(o, "icon_url", it.icon_url, sizeof it.icon_url);
    it.platforms = parse_platforms_array(ccord_json_get(o, "platforms"));
    if (!cat_valid_catalog_id(it.id) || !it.name[0] || !cat_valid_catalog_version(it.version)) continue;

    int64_t tv = 3;
    if (ccord_json_int64(ccord_json_get(o, "tier"), &tv) && tv >= 1 && tv <= 3) it.tier = (ccord_plugin_tier)tv;
    else it.tier = CCORD_TIER_UNVERIFIED;

    int64_t ac = 1;
    if (ccord_json_int64(ccord_json_get(o, "api_class"), &ac) && ac >= 1 && ac <= 3) it.api_class = (uint8_t)ac;
    else it.api_class = 1;

    int64_t sz = 0;
    if (ccord_json_int64(ccord_json_get(o, "size_bytes"), &sz) && sz > 0) it.size_bytes = (uint64_t)sz;
    int64_t dls = 0;
    if (ccord_json_int64(ccord_json_get(o, "downloads"), &dls) && dls > 0) it.downloads = (uint64_t)dls;
    int64_t ws = 0;
    if (ccord_json_int64(ccord_json_get(o, "warning_sev"), &ws) && ws >= 0 && ws <= 3) {
      it.warning_sev = (ccord_catalog_warn_sev)ws;
    }
    it.needs_tier_review = json_bool_or_int(ccord_json_get(o, "needs_tier_review"));
    it.has_detail = false;

    // Avoid duplicates when appending cursor pages
    bool dup = false;
    for (size_t k = 0; k < cat->ndiscover; k++) {
      if (strcmp(cat->discover[k].id, it.id) == 0) {
        cat->discover[k] = it;
        dup = true;
        break;
      }
    }
    if (!dup) cat->discover[cat->ndiscover++] = it;
    if (it.icon_url[0]) cat_maybe_queue_icon(cat, it.icon_url);
  }

  cat->status.total_count = total > 0 ? (size_t)total : cat->ndiscover;
  cat->status.has_more = has_more && next_cur[0] != '\0';
  cat_copy_str(cat->next_cursor, sizeof cat->next_cursor, next_cur);
  cat->status.has_loaded = true;
  ccord_arena_free(&arena);
  return true;
}

// Parse GET /api/v2/plugins/{id} full detail response
bool cat_parse_detail_json(const ccord_catalog *cat, const char *json, size_t len, cat_item *out,
                           bool *out_sig_failed)
{
  if (out_sig_failed) *out_sig_failed = false;
  if (!json || len == 0 || !out) return false;
  memset(out, 0, sizeof *out);
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {12, 8192, CATALOG_MAX_API_BYTES};
  const ccord_json *o = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }
  const ccord_json *latest = ccord_json_get(o, "latest");
  const ccord_json *author = ccord_json_get(o, "author");
  if (!latest || ccord_json_type_of(latest) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }

  cat_json_str_copy(o, "id", out->id, sizeof out->id);
  cat_json_str_copy(o, "name", out->name, sizeof out->name);
  cat_json_str_copy(o, "description", out->description, sizeof out->description);
  cat_json_str_copy(o, "license", out->license, sizeof out->license);
  cat_json_str_copy(o, "icon_url", out->icon_url, sizeof out->icon_url);
  if (author && ccord_json_type_of(author) == CCORD_JSON_OBJECT) {
    cat_json_str_copy(author, "login", out->author, sizeof out->author);
    cat_json_str_copy(author, "repo", out->repo, sizeof out->repo);
    cat_json_str_copy(author, "repo_url", out->repo_url, sizeof out->repo_url);
    (void)ccord_json_int64(ccord_json_get(author, "github_user_id"), &out->author_github_id);
    (void)ccord_json_int64(ccord_json_get(author, "repo_id"), &out->repo_id);
  }
  if (out->author_github_id == 0) {
    (void)ccord_json_int64(ccord_json_get(o, "author_github_id"), &out->author_github_id);
  }
  if (out->repo_id == 0) {
    (void)ccord_json_int64(ccord_json_get(o, "repo_id"), &out->repo_id);
  }
  out->needs_tier_review = json_bool_or_int(ccord_json_get(o, "needs_tier_review"));
  cat_json_str_copy(latest, "version", out->version, sizeof out->version);
  cat_json_str_copy(latest, "download_url", out->download_url, sizeof out->download_url);
  cat_json_str_copy(latest, "sha256", out->sha256, sizeof out->sha256);
  cat_json_str_copy(latest, "changelog", out->changelog, sizeof out->changelog);
  out->platforms = parse_platforms_array(ccord_json_get(latest, "platforms"));
  const ccord_json *perms_arr = ccord_json_get(latest, "permissions");
  const ccord_json *hosts_arr = ccord_json_get(latest, "allowed_hosts");
  parse_hosts_array(hosts_arr, out->allowed_hosts, sizeof out->allowed_hosts);
  if (!cat_valid_catalog_id(out->id) || !out->name[0] || !cat_valid_catalog_version(out->version) ||
      !out->download_url[0] || strlen(out->sha256) != 64) {
    ccord_arena_free(&arena);
    return false;
  }
  int64_t tv = 3;
  if (ccord_json_int64(ccord_json_get(o, "tier"), &tv) && tv >= 1 && tv <= 3) out->tier = (ccord_plugin_tier)tv;
  else out->tier = CCORD_TIER_UNVERIFIED;
  int64_t sz = 0;
  if (ccord_json_int64(ccord_json_get(latest, "size_bytes"), &sz) && sz > 0) out->size_bytes = (uint64_t)sz;
  int64_t dls = 0;
  if (ccord_json_int64(ccord_json_get(o, "downloads"), &dls) && dls > 0) out->downloads = (uint64_t)dls;

  parse_permissions_array(perms_arr, out);
  int64_t ac = 0;
  if (ccord_json_int64(ccord_json_get(latest, "api_class"), &ac) && ac >= 1 && ac <= 3) out->api_class = (uint8_t)ac;
  else out->api_class = (uint8_t)(ccord_perm_max_class(out->perms_mask) + 1);
  parse_warnings_array(ccord_json_get(o, "warnings"), out);
  if (out->warning_sev == CCORD_WARN_NONE) {
    int64_t ws = 0;
    if (ccord_json_int64(ccord_json_get(o, "warning_sev"), &ws) && ws >= 0 && ws <= 3) {
      out->warning_sev = (ccord_catalog_warn_sev)ws;
    }
  }
  parse_signature_obj(o, out);
  if (!out->has_signature) parse_signature_obj(latest, out);
  if (!verify_plugin_item_signature(cat, out, perms_arr, hosts_arr)) {
    if (out_sig_failed) *out_sig_failed = true;
    CCORD_LOG_W("catalog: rejected plugin detail for %s v%s (invalid or missing Ed25519 signature)",
                out->id, out->version);
    ccord_arena_free(&arena);
    memset(out, 0, sizeof *out);
    return false;
  }
  out->has_detail = true;

  ccord_arena_free(&arena);
  return true;
}

// Parse POST /api/v2/updates/check response
bool cat_parse_updates_check_json(ccord_catalog *cat, const char *json, size_t len,
                                     bool *out_rev_stale, int64_t *out_rev_seq)
{
  if (!cat || !json || len == 0) return false;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error pe = {0};
  const ccord_json_limits lim = {12, 16384, CATALOG_MAX_API_BYTES};
  const ccord_json *root = ccord_json_parse(&arena, json, len, &lim, &pe);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&arena);
    return false;
  }
  int64_t schema = 0;
  if (!ccord_json_int64(ccord_json_get(root, "schema"), &schema) || schema != 2) {
    ccord_arena_free(&arena);
    return false;
  }
  int64_t rseq = 0;
  (void)ccord_json_int64(ccord_json_get(root, "rev_seq"), &rseq);
  const ccord_json *rs = ccord_json_get(root, "revocations_stale");
  if (out_rev_stale) *out_rev_stale = (rs && ccord_json_bool(rs, false)) || rseq > cat->rev_seq;
  if (out_rev_seq) *out_rev_seq = rseq;

  const ccord_json *arr = ccord_json_get(root, "plugins");
  cat_item *new_upd = nullptr;
  size_t new_n = 0;
  if (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) {
    size_t pn = ccord_json_array_len(arr);
    if (pn > 0 && pn <= CATALOG_MAX_BATCH_IDS) {
      new_upd = ccord_calloc(pn, sizeof *new_upd);
      if (new_upd) {
        for (size_t i = 0; i < pn; i++) {
          const ccord_json *o = ccord_json_array_at(arr, i);
          if (!o || ccord_json_type_of(o) != CCORD_JSON_OBJECT) continue;
          cat_item *it = &new_upd[new_n];
          memset(it, 0, sizeof *it);
          cat_json_str_copy(o, "id", it->id, sizeof it->id);
          cat_json_str_copy(o, "name", it->name, sizeof it->name);
          cat_json_str_copy(o, "description", it->description, sizeof it->description);
          cat_json_str_copy(o, "license", it->license, sizeof it->license);
          cat_json_str_copy(o, "author_login", it->author, sizeof it->author);
          cat_json_str_copy(o, "author_repo", it->repo, sizeof it->repo);
          cat_json_str_copy(o, "author_repo_url", it->repo_url, sizeof it->repo_url);
          (void)ccord_json_int64(ccord_json_get(o, "author_github_id"), &it->author_github_id);
          (void)ccord_json_int64(ccord_json_get(o, "repo_id"), &it->repo_id);
          const ccord_json *author = ccord_json_get(o, "author");
          if (author && ccord_json_type_of(author) == CCORD_JSON_OBJECT) {
            if (it->author_github_id == 0) {
              (void)ccord_json_int64(ccord_json_get(author, "github_user_id"), &it->author_github_id);
            }
            if (it->repo_id == 0) {
              (void)ccord_json_int64(ccord_json_get(author, "repo_id"), &it->repo_id);
            }
          }
          it->needs_tier_review = json_bool_or_int(ccord_json_get(o, "needs_tier_review"));
          cat_json_str_copy(o, "version", it->version, sizeof it->version);
          cat_json_str_copy(o, "download_url", it->download_url, sizeof it->download_url);
          cat_json_str_copy(o, "sha256", it->sha256, sizeof it->sha256);
          cat_json_str_copy(o, "changelog", it->changelog, sizeof it->changelog);
          cat_json_str_copy(o, "icon_url", it->icon_url, sizeof it->icon_url);
          it->platforms = parse_platforms_array(ccord_json_get(o, "platforms"));
          const ccord_json *perms_arr = ccord_json_get(o, "permissions");
          const ccord_json *hosts_arr = ccord_json_get(o, "allowed_hosts");
          parse_hosts_array(hosts_arr, it->allowed_hosts, sizeof it->allowed_hosts);
          if (!cat_valid_catalog_id(it->id) || !it->name[0] || !cat_valid_catalog_version(it->version) ||
              !it->download_url[0] || strlen(it->sha256) != 64) {
            memset(it, 0, sizeof *it);
            continue;
          }
          int64_t tv = 3;
          if (ccord_json_int64(ccord_json_get(o, "tier"), &tv) && tv >= 1 && tv <= 3) it->tier = (ccord_plugin_tier)tv;
          else it->tier = CCORD_TIER_UNVERIFIED;
          int64_t sz = 0;
          if (ccord_json_int64(ccord_json_get(o, "size_bytes"), &sz) && sz > 0) it->size_bytes = (uint64_t)sz;
          parse_permissions_array(perms_arr, it);
          int64_t ac = 0;
          if (ccord_json_int64(ccord_json_get(o, "api_class"), &ac) && ac >= 1 && ac <= 3) it->api_class = (uint8_t)ac;
          else it->api_class = (uint8_t)(ccord_perm_max_class(it->perms_mask) + 1);
          parse_warnings_array(ccord_json_get(o, "warnings"), it);
          if (it->warning_sev == CCORD_WARN_NONE) {
            int64_t ws = 0;
            if (ccord_json_int64(ccord_json_get(o, "warning_sev"), &ws) && ws >= 0 && ws <= 3) {
              it->warning_sev = (ccord_catalog_warn_sev)ws;
            }
          }
          parse_signature_obj(o, it);
          if (!verify_plugin_item_signature(cat, it, perms_arr, hosts_arr)) {
            CCORD_LOG_W("catalog: skipping update for %s v%s (invalid or missing Ed25519 signature)",
                        it->id, it->version);
            memset(it, 0, sizeof *it);
            continue;
          }
          it->has_detail = true;
          if (it->icon_url[0]) cat_maybe_queue_icon(cat, it->icon_url);
          new_n++;
        }
      }
    }
  }
  ccord_arena_free(&arena);

  ccord_free(cat->updates);
  cat->updates = new_upd;
  cat->nupdates = new_n;
  return true;
}

// ---------------------------------------------------------------- lookup & publish to ccord_contrib

const cat_item *cat_find_update_item(const ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  for (size_t i = 0; i < cat->nupdates; i++) {
    if (strcmp(cat->updates[i].id, id) == 0) return &cat->updates[i];
  }
  return nullptr;
}

const cat_item *cat_find_best_item(const ccord_catalog *cat, const char *id)
{
  if (!cat || !id) return nullptr;
  if (cat->has_detail && cat->detail && strcmp(cat->detail->id, id) == 0) return cat->detail;
  const cat_item *u = cat_find_update_item(cat, id);
  if (u) return u;
  for (size_t i = 0; i < cat->ndiscover; i++) {
    if (strcmp(cat->discover[i].id, id) == 0) return &cat->discover[i];
  }
  return nullptr;
}

bool cat_compute_update_info(const cat_item *it, const char *local_ver, uint64_t consented_perms,
                             const char *consented_hosts_csv, bool had_consented_hosts,
                             bool *out_avail, bool *out_needs_review)
{
  *out_avail = false;
  *out_needs_review = false;
  if (!it || !local_ver || !local_ver[0]) return false;
  if (ccord_catalog_semver_cmp(local_ver, it->version) < 0) {
    *out_avail = true;
    bool new_restricted_perms = false;
    uint64_t added = it->perms_mask & ~consented_perms;
    if (added != 0 && ccord_perm_max_class(added) > CCORD_PERM_CLASS_SAFE) {
      new_restricted_perms = true;
    }
    bool hosts_expanded = ccord_hosts_expanded(consented_hosts_csv, had_consented_hosts,
                                               it->allowed_hosts, it->perms_mask);
    if (new_restricted_perms || hosts_expanded || it->needs_tier_review ||
        it->warning_sev >= CCORD_WARN_DANGER) {
      *out_needs_review = true;
    }
    return true;
  }
  return false;
}

bool ccord_catalog_query_overlay(const ccord_catalog *cat, const char *id,
                                 const char *installed_ver, uint64_t consented_perms,
                                 ccord_catalog_local_overlay *out)
{
  if (!cat || !id || !out) return false;
  memset(out, 0, sizeof *out);
  const installed_meta *m = cat_find_meta_const(cat, id);
  out->auto_update = m ? m->auto_update : true;
  if (m) out->tier = m->tier;

  const cat_item *it = cat_find_best_item(cat, id);
  if (!it) return m != nullptr;

  out->tier = it->tier;
  out->warning_sev = it->warning_sev;
  cat_copy_str(out->warning_title, sizeof out->warning_title, it->warning_title);
  cat_copy_str(out->warning_message, sizeof out->warning_message, it->warning_message);
  if (it->icon_url[0]) {
    (void)cat_resolve_cached_icon_path(cat, it->icon_url, out->icon_path, sizeof out->icon_path);
  }
  char consented_hosts[1024] = "";
  bool had_consented_hosts = false;
  if (cat->cbs.get_local_allowed_hosts) {
    (void)cat->cbs.get_local_allowed_hosts(cat->cbs.ud, id, consented_hosts,
                                           sizeof consented_hosts, &had_consented_hosts);
  }
  bool avail = false, review = false;
  if (cat_compute_update_info(it, installed_ver, consented_perms, consented_hosts, had_consented_hosts,
                              &avail, &review) && avail) {
    cat_copy_str(out->update_version, sizeof out->update_version, it->version);
    out->update_needs_review = review;
    out->update_perms_mask = it->perms_mask;
    cat_copy_str(out->update_permissions, sizeof out->update_permissions, it->permissions);
    cat_copy_str(out->update_allowed_hosts, sizeof out->update_allowed_hosts, it->allowed_hosts);
    cat_copy_str(out->update_changelog, sizeof out->update_changelog, it->changelog);
  }
  return true;
}

static void build_contrib_entry(const ccord_catalog *cat, const cat_item *it,
                                char local_ver_buf[33], char icon_path_buf[CCORD_PATH_MAX],
                                ccord_catalog_entry *out, bool *out_pending_upd)
{
  bool installed = false, bundled = false;
  uint64_t consented = 0;
  local_ver_buf[0] = '\0';
  icon_path_buf[0] = '\0';
  if (cat->cbs.get_local_plugin) {
    installed = cat->cbs.get_local_plugin(cat->cbs.ud, it->id, local_ver_buf, 33, &consented, &bundled);
  }
  const installed_meta *m = cat_find_meta_const(cat, it->id);
  bool avail = false, review = false;
  if (installed && !bundled) {
    char consented_hosts[1024] = "";
    bool had_consented_hosts = false;
    if (cat->cbs.get_local_allowed_hosts) {
      (void)cat->cbs.get_local_allowed_hosts(cat->cbs.ud, it->id, consented_hosts,
                                             sizeof consented_hosts, &had_consented_hosts);
    }
    const cat_item *upd_src = it->has_detail ? it : cat_find_update_item(cat, it->id);
    cat_compute_update_info(upd_src ? upd_src : it, local_ver_buf, consented,
                            consented_hosts, had_consented_hosts, &avail, &review);
  }
  if (out_pending_upd) *out_pending_upd = avail;
  if (it->icon_url[0]) {
    (void)cat_resolve_cached_icon_path(cat, it->icon_url, icon_path_buf, CCORD_PATH_MAX);
  }

  *out = (ccord_catalog_entry){
    .id = it->id,
    .name = it->name,
    .description = it->description,
    .license = it->license,
    .author = it->author,
    .repo = it->repo,
    .repo_url = it->repo_url,
    .version = it->version,
    .download_url = it->download_url,
    .sha256 = it->sha256,
    .permissions = it->permissions,
    .allowed_hosts = it->allowed_hosts,
    .changelog = it->changelog,
    .icon_url = it->icon_url,
    .icon_path = icon_path_buf,
    .perms_mask = it->perms_mask,
    .platforms = it->platforms ? it->platforms : CCORD_PLAT_ALL,
    .size_bytes = it->size_bytes,
    .downloads = it->downloads,
    .api_class = it->api_class,
    .tier = it->tier,
    .warning_sev = it->warning_sev,
    .warning_title = it->warning_title,
    .warning_message = it->warning_message,
    .has_detail = it->has_detail,
    .installed = installed,
    .bundled = bundled,
    .installing = cat_is_installing(cat, it->id),
    .update_available = avail,
    .update_needs_review = review,
    .auto_update = m ? m->auto_update : true,
    .installed_version = installed ? local_ver_buf : "",
  };
}

void cat_publish_catalog(ccord_catalog *cat)
{
  if (!cat || !cat->contrib) return;

  // 1. Publish installed updates list (independent of current Discover search page!)
  size_t pending = 0;
  ccord_catalog_entry *upd_entries = cat->nupdates ? ccord_calloc(cat->nupdates, sizeof *upd_entries) : nullptr;
  char (*upd_vers)[33] = cat->nupdates ? ccord_calloc(cat->nupdates, sizeof *upd_vers) : nullptr;
  char (*upd_icons)[CCORD_PATH_MAX] = cat->nupdates ? ccord_calloc(cat->nupdates, sizeof *upd_icons) : nullptr;
  size_t upd_out_n = 0;
  for (size_t i = 0; i < cat->nupdates && upd_entries && upd_vers && upd_icons; i++) {
    bool is_upd = false;
    build_contrib_entry(cat, &cat->updates[i], upd_vers[upd_out_n], upd_icons[upd_out_n], &upd_entries[upd_out_n], &is_upd);
    if (is_upd) {
      pending++;
      upd_out_n++;
    }
  }
  cat->status.pending_updates = pending;
  ccord_contrib_set_catalog_updates(cat->contrib, upd_entries, upd_out_n);
  ccord_free(upd_entries);
  ccord_free(upd_vers);
  ccord_free(upd_icons);

  // 2. Publish on-demand detail entry
  if (cat->has_detail && cat->detail) {
    char det_ver[33];
    char det_icon[CCORD_PATH_MAX];
    ccord_catalog_entry det_e = {0};
    build_contrib_entry(cat, cat->detail, det_ver, det_icon, &det_e, nullptr);
    ccord_contrib_set_catalog_detail(cat->contrib, &det_e);
  } else {
    ccord_contrib_set_catalog_detail(cat->contrib, nullptr);
  }

  // 3. Publish current Discover page + status + prefs
  ccord_catalog_entry *disc_entries = cat->ndiscover ? ccord_calloc(cat->ndiscover, sizeof *disc_entries) : nullptr;
  char (*disc_vers)[33] = cat->ndiscover ? ccord_calloc(cat->ndiscover, sizeof *disc_vers) : nullptr;
  char (*disc_icons)[CCORD_PATH_MAX] = cat->ndiscover ? ccord_calloc(cat->ndiscover, sizeof *disc_icons) : nullptr;
  for (size_t i = 0; i < cat->ndiscover && disc_entries && disc_vers && disc_icons; i++) {
    build_contrib_entry(cat, &cat->discover[i], disc_vers[i], disc_icons[i], &disc_entries[i], nullptr);
  }
  ccord_contrib_set_catalog(cat->contrib, disc_entries, (disc_entries && disc_vers && disc_icons) ? cat->ndiscover : 0,
                            &cat->status, &cat->prefs);
  ccord_free(disc_entries);
  ccord_free(disc_vers);
  ccord_free(disc_icons);

  if (cat->cbs.sync_installed_info) cat->cbs.sync_installed_info(cat->cbs.ud);
}

void cat_enforce_revocations(ccord_catalog *cat)
{
  if (!cat || cat->nrevoked == 0 || !cat->cbs.revoke_plugin) return;
  for (size_t i = 0; i < cat->nmeta; i++) {
    const installed_meta *m = &cat->meta[i];
    if (strlen(m->sha256) != 64) continue;
    for (size_t r = 0; r < cat->nrevoked; r++) {
      if (cat_hex_equal_ci(m->sha256, cat->revoked[r].sha256)) {
        char msg[256];
        snprintf(msg, sizeof msg, "Revoked by C-Cord moderators: %s",
                 cat->revoked[r].reason[0] ? cat->revoked[r].reason : "security policy");
        cat->cbs.revoke_plugin(cat->cbs.ud, m->id, msg);
        break;
      }
    }
  }
  for (size_t r = 0; r < cat->nrevoked; r++) {
    if (cat->revoked[r].sha256[0] == '\0' && cat->revoked[r].plugin_id[0] != '\0') {
      char msg[256];
      snprintf(msg, sizeof msg, "Revoked by C-Cord moderators: %s",
               cat->revoked[r].reason[0] ? cat->revoked[r].reason : "security policy");
      cat->cbs.revoke_plugin(cat->cbs.ud, cat->revoked[r].plugin_id, msg);
    }
  }
}

void ccord_catalog_enforce_revocations(ccord_catalog *cat)
{
  cat_enforce_revocations(cat);
}

bool ccord_catalog_is_hash_or_id_revoked(const ccord_catalog *cat, const char *id, const char *sha256_hex,
                                         char *reason_out, size_t reason_cap)
{
  if (!cat || cat->nrevoked == 0) return false;
  bool has_sha = sha256_hex && strlen(sha256_hex) == 64;
  bool has_id = id && id[0] != '\0';
  for (size_t r = 0; r < cat->nrevoked; r++) {
    bool hit = false;
    if (has_sha && strlen(cat->revoked[r].sha256) == 64 &&
        cat_hex_equal_ci(sha256_hex, cat->revoked[r].sha256)) {
      hit = true;
    } else if (has_id && cat->revoked[r].sha256[0] == '\0' && cat->revoked[r].plugin_id[0] != '\0' &&
               strcmp(cat->revoked[r].plugin_id, id) == 0) {
      hit = true;
    }
    if (hit) {
      if (reason_out && reason_cap) {
        snprintf(reason_out, reason_cap, "Revoked by C-Cord moderators: %s",
                 cat->revoked[r].reason[0] ? cat->revoked[r].reason : "security policy");
      }
      return true;
    }
  }
  return false;
}

bool ccord_catalog_is_revoked(const ccord_catalog *cat, const char *id, char *reason_out, size_t reason_cap)
{
  if (!cat || !id || !id[0] || cat->nrevoked == 0) return false;
  const installed_meta *m = cat_find_meta_const(cat, id);
  return ccord_catalog_is_hash_or_id_revoked(cat, id, m ? m->sha256 : nullptr, reason_out, reason_cap);
}

