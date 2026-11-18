// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "zip.h"

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void set_err(char *eb, size_t ec, const char *msg)
{
  if (eb && ec) snprintf(eb, ec, "%s", msg ? msg : "zip error");
}

static bool safe_rel_path(const char *name)
{
  if (!name || !name[0] || name[0] == '/' || strchr(name, '\\')) return false;
  const char *p = name;
  while (*p) {
    const char *sl = strchr(p, '/');
    size_t len = sl ? (size_t)(sl - p) : strlen(p);
    if ((len == 2 && p[0] == '.' && p[1] == '.') || (len == 1 && p[0] == '.')) return false;
    if (!sl) break;
    p = sl + 1;
  }
  return true;
}

static bool mkdir_p(const char *path)
{
  char tmp[PATH_MAX];
  size_t n = strlen(path);
  if (n == 0 || n >= sizeof tmp) return false;
  memcpy(tmp, path, n + 1);
  for (size_t i = 1; i < n; i++) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return false;
      tmp[i] = '/';
    }
  }
  return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static bool read_all(const char *path, uint8_t **out, size_t *out_len)
{
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
  long sz = ftell(f);
  if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
  uint8_t *buf = ccord_malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return false; }
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = 0;
  *out = buf;
  *out_len = rd;
  return true;
}

static bool inflate_raw(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen)
{
  z_stream zs = {0};
  zs.next_in = (Bytef *)(uintptr_t)src;
  zs.avail_in = (uInt)slen;
  zs.next_out = dst;
  zs.avail_out = (uInt)dlen;
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
  int rc = inflate(&zs, Z_FINISH);
  bool ok = rc == Z_STREAM_END && zs.total_out == dlen;
  inflateEnd(&zs);
  return ok;
}

bool ccord_zip_extract(const char *zip_path, const char *dest_dir, char *err_buf, size_t err_cap)
{
  if (!zip_path || !dest_dir) { set_err(err_buf, err_cap, "invalid arguments"); return false; }
  uint8_t *data = nullptr;
  size_t len = 0;
  if (!read_all(zip_path, &data, &len) || len < 22) {
    ccord_free(data);
    set_err(err_buf, err_cap, "cannot read package file");
    return false;
  }
  if (len > 64u * 1024u * 1024u) {
    ccord_free(data);
    set_err(err_buf, err_cap, "archive exceeds 64 MiB limit");
    return false;
  }

  // Locate EOCD
  size_t eocd = SIZE_MAX;
  size_t min_off = len > 65557 ? len - 65557 : 0;
  for (size_t i = len - 22;; i--) {
    if (rd32(data + i) == 0x06054b50u) { eocd = i; break; }
    if (i == min_off) break;
  }
  if (eocd == SIZE_MAX) {
    ccord_free(data);
    set_err(err_buf, err_cap, "not a valid zip/cce archive");
    return false;
  }

  uint16_t nentries = rd16(data + eocd + 10);
  if (nentries > 1024) {
    ccord_free(data);
    set_err(err_buf, err_cap, "archive exceeds 1024 entries limit");
    return false;
  }
  uint32_t cd_size = rd32(data + eocd + 12);
  uint32_t cd_off = rd32(data + eocd + 16);
  if ((uint64_t)cd_off + cd_size > eocd) {
    ccord_free(data);
    set_err(err_buf, err_cap, "corrupt zip central directory");
    return false;
  }

  // Detect if all entries share a single top-level directory containing plugin.json
  char prefix[256] = "";
  bool has_root_manifest = false;
  bool same_prefix = true;
  size_t pos = cd_off;
  uint64_t cumulative_usize = 0;
  for (uint16_t i = 0; i < nentries; i++) {
    if (pos + 46 > cd_off + cd_size || rd32(data + pos) != 0x02014b50u) {
      ccord_free(data);
      set_err(err_buf, err_cap, "corrupt central directory header");
      return false;
    }
    uint32_t usize = rd32(data + pos + 24);
    if (usize > 32u * 1024u * 1024u) {
      ccord_free(data);
      set_err(err_buf, err_cap, "entry exceeds 32 MiB uncompressed limit");
      return false;
    }
    cumulative_usize += usize;
    if (cumulative_usize > 128u * 1024u * 1024u) {
      ccord_free(data);
      set_err(err_buf, err_cap, "archive exceeds 128 MiB cumulative uncompressed limit");
      return false;
    }
    uint32_t ext_attr = rd32(data + pos + 38);
    if (((ext_attr >> 16) & 0170000u) == 0120000u) {
      ccord_free(data);
      set_err(err_buf, err_cap, "symlink external attributes rejected");
      return false;
    }
    uint16_t nlen = rd16(data + pos + 28);
    uint16_t elen = rd16(data + pos + 30);
    uint16_t clen = rd16(data + pos + 32);
    if (pos + 46 + nlen + elen + clen > cd_off + cd_size) {
      ccord_free(data);
      set_err(err_buf, err_cap, "truncated central directory entry");
      return false;
    }
    char name[PATH_MAX];
    size_t copy = nlen < sizeof name - 1 ? nlen : sizeof name - 1;
    memcpy(name, data + pos + 46, copy);
    name[copy] = '\0';
    if (strcmp(name, "plugin.json") == 0) has_root_manifest = true;
    const char *sl = strchr(name, '/');
    if (!sl) {
      same_prefix = false;
    } else {
      size_t plen = (size_t)(sl - name) + 1;
      if (plen >= sizeof prefix) same_prefix = false;
      else if (prefix[0] == '\0') { memcpy(prefix, name, plen); prefix[plen] = '\0'; }
      else if (strncmp(prefix, name, plen) != 0 || prefix[plen] != '\0') same_prefix = false;
    }
    pos += (size_t)46 + nlen + elen + clen;
  }
  size_t strip_len = (!has_root_manifest && same_prefix) ? strlen(prefix) : 0;

  if (!mkdir_p(dest_dir)) {
    ccord_free(data);
    set_err(err_buf, err_cap, "cannot create destination directory");
    return false;
  }

  pos = cd_off;
  for (uint16_t i = 0; i < nentries; i++) {
    uint16_t method = rd16(data + pos + 10);
    uint32_t crc_exp = rd32(data + pos + 16);
    uint32_t csize = rd32(data + pos + 20);
    uint32_t usize = rd32(data + pos + 24);
    uint16_t nlen = rd16(data + pos + 28);
    uint16_t elen = rd16(data + pos + 30);
    uint16_t clen = rd16(data + pos + 32);
    uint32_t lhoff = rd32(data + pos + 42);
    char name[PATH_MAX];
    size_t copy = nlen < sizeof name - 1 ? nlen : sizeof name - 1;
    memcpy(name, data + pos + 46, copy);
    name[copy] = '\0';
    pos += (size_t)46 + nlen + elen + clen;

    const char *rel = name + strip_len;
    if (!rel[0]) continue;
    if (!safe_rel_path(rel)) {
      ccord_free(data);
      set_err(err_buf, err_cap, "unsafe path in archive");
      return false;
    }

    char out_path[PATH_MAX];
    if (snprintf(out_path, sizeof out_path, "%s/%s", dest_dir, rel) >= (int)sizeof out_path) {
      ccord_free(data);
      set_err(err_buf, err_cap, "path too long");
      return false;
    }

    if (rel[strlen(rel) - 1] == '/') {
      if (!mkdir_p(out_path)) { ccord_free(data); set_err(err_buf, err_cap, "mkdir failed"); return false; }
      continue;
    }

    if ((uint64_t)lhoff + 30 > len || rd32(data + lhoff) != 0x04034b50u) {
      ccord_free(data);
      set_err(err_buf, err_cap, "corrupt local file header");
      return false;
    }
    uint16_t lh_nlen = rd16(data + lhoff + 26);
    uint16_t lh_elen = rd16(data + lhoff + 28);
    uint64_t doff = (uint64_t)lhoff + 30 + lh_nlen + lh_elen;
    if (doff + csize > len) {
      ccord_free(data);
      set_err(err_buf, err_cap, "truncated file payload");
      return false;
    }

    // Ensure parent dir exists
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", out_path);
    char *last = strrchr(parent, '/');
    if (last) { *last = '\0'; (void)mkdir_p(parent); }

    uint8_t *out_buf = ccord_malloc(usize ? usize : 1);
    if (!out_buf) { ccord_free(data); set_err(err_buf, err_cap, "out of memory"); return false; }
    bool ok = false;
    if (method == 0) {
      ok = csize == usize;
      if (ok && usize) memcpy(out_buf, data + doff, usize);
    } else if (method == 8) {
      ok = inflate_raw(data + doff, csize, out_buf, usize);
    }
    if (!ok || crc32(0, out_buf, (uInt)usize) != crc_exp) {
      ccord_free(out_buf);
      ccord_free(data);
      set_err(err_buf, err_cap, "decompression or CRC error");
      return false;
    }
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    FILE *wf = fd >= 0 ? fdopen(fd, "wb") : nullptr;
    if (!wf || (usize && fwrite(out_buf, 1, usize, wf) != usize)) {
      if (wf) fclose(wf);
      else if (fd >= 0) close(fd);
      ccord_free(out_buf);
      ccord_free(data);
      set_err(err_buf, err_cap, "failed writing extracted file");
      return false;
    }
    fclose(wf);
    ccord_free(out_buf);
  }

  ccord_free(data);
  return true;
}

typedef struct pack_entry {
  char *rel;
  uint32_t crc, csize, usize, offset;
  uint16_t method;
} pack_entry;

typedef struct pack_ctx {
  ccord_buf out;
  pack_entry *entries;
  size_t nentries, capentries;
} pack_ctx;

static bool deflate_raw(const uint8_t *src, size_t slen, uint8_t **dst, size_t *dlen)
{
  z_stream zs = {0};
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
  size_t bound = deflateBound(&zs, (uLong)slen);
  uint8_t *buf = ccord_malloc(bound ? bound : 1);
  if (!buf) { deflateEnd(&zs); return false; }
  zs.next_in = (Bytef *)(uintptr_t)src;
  zs.avail_in = (uInt)slen;
  zs.next_out = buf;
  zs.avail_out = (uInt)bound;
  int rc = deflate(&zs, Z_FINISH);
  *dlen = zs.total_out;
  deflateEnd(&zs);
  if (rc != Z_STREAM_END) { ccord_free(buf); return false; }
  *dst = buf;
  return true;
}

static bool pack_walk(pack_ctx *ctx, const char *base_dir, const char *rel_prefix)
{
  char dir_path[PATH_MAX];
  if (rel_prefix[0]) snprintf(dir_path, sizeof dir_path, "%s/%s", base_dir, rel_prefix);
  else snprintf(dir_path, sizeof dir_path, "%s", base_dir);
  DIR *d = opendir(dir_path);
  if (!d) return false;
  struct dirent *de;
  while ((de = readdir(d)) != nullptr) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    char rel[PATH_MAX], full[PATH_MAX];
    if (rel_prefix[0]) snprintf(rel, sizeof rel, "%s/%s", rel_prefix, de->d_name);
    else snprintf(rel, sizeof rel, "%s", de->d_name);
    if (snprintf(full, sizeof full, "%s/%s", base_dir, rel) >= (int)sizeof full) continue;
    struct stat st;
    if (stat(full, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (!pack_walk(ctx, base_dir, rel)) { closedir(d); return false; }
      continue;
    }
    if (!S_ISREG(st.st_mode)) continue;
    uint8_t *raw = nullptr;
    size_t rlen = 0;
    if (!read_all(full, &raw, &rlen)) { closedir(d); return false; }
    uint32_t crc = (uint32_t)crc32(0, raw, (uInt)rlen);
    uint8_t *comp = nullptr;
    size_t clen = 0;
    uint16_t method = 8;
    if (!deflate_raw(raw, rlen, &comp, &clen) || clen >= rlen) {
      ccord_free(comp);
      comp = raw;
      clen = rlen;
      method = 0;
    } else {
      ccord_free(raw);
    }

    size_t nlen = strlen(rel);
    uint8_t lh[30] = {0};
    wr32(lh, 0x04034b50u);
    wr16(lh + 4, 20);
    wr16(lh + 8, method);
    wr32(lh + 14, crc);
    wr32(lh + 18, (uint32_t)clen);
    wr32(lh + 22, (uint32_t)rlen);
    wr16(lh + 26, (uint16_t)nlen);

    uint32_t off = (uint32_t)ctx->out.len;
    if (ccord_buf_append(&ctx->out, lh, 30) != CCORD_OK ||
        ccord_buf_append(&ctx->out, rel, nlen) != CCORD_OK ||
        ccord_buf_append(&ctx->out, comp, clen) != CCORD_OK) {
      ccord_free(comp);
      closedir(d);
      return false;
    }
    ccord_free(comp);

    if (ctx->nentries == ctx->capentries) {
      size_t ncap = ctx->capentries ? ctx->capentries * 2 : 8;
      pack_entry *ne = ccord_realloc_array(ctx->entries, ncap, sizeof *ne);
      if (!ne) { closedir(d); return false; }
      ctx->entries = ne;
      ctx->capentries = ncap;
    }
    ctx->entries[ctx->nentries++] = (pack_entry){
      .rel = ccord_strdup(rel), .crc = crc, .csize = (uint32_t)clen,
      .usize = (uint32_t)rlen, .offset = off, .method = method,
    };
  }
  closedir(d);
  return true;
}

bool ccord_zip_pack_dir(const char *src_dir, const char *zip_path, char *err_buf, size_t err_cap)
{
  if (!src_dir || !zip_path) { set_err(err_buf, err_cap, "invalid arguments"); return false; }
  pack_ctx ctx = {0};
  ccord_buf_init(&ctx.out, 0);
  if (!pack_walk(&ctx, src_dir, "")) {
    for (size_t i = 0; i < ctx.nentries; i++) ccord_free(ctx.entries[i].rel);
    ccord_free(ctx.entries);
    ccord_buf_free(&ctx.out);
    set_err(err_buf, err_cap, "failed reading plugin directory");
    return false;
  }

  uint32_t cd_off = (uint32_t)ctx.out.len;
  for (size_t i = 0; i < ctx.nentries; i++) {
    pack_entry *e = &ctx.entries[i];
    size_t nlen = strlen(e->rel);
    uint8_t cd[46] = {0};
    wr32(cd, 0x02014b50u);
    wr16(cd + 4, 20);
    wr16(cd + 6, 20);
    wr16(cd + 10, e->method);
    wr32(cd + 16, e->crc);
    wr32(cd + 20, e->csize);
    wr32(cd + 24, e->usize);
    wr16(cd + 28, (uint16_t)nlen);
    wr32(cd + 42, e->offset);
    (void)ccord_buf_append(&ctx.out, cd, 46);
    (void)ccord_buf_append(&ctx.out, e->rel, nlen);
  }
  uint32_t cd_size = (uint32_t)(ctx.out.len - cd_off);
  uint8_t eocd[22] = {0};
  wr32(eocd, 0x06054b50u);
  wr16(eocd + 8, (uint16_t)ctx.nentries);
  wr16(eocd + 10, (uint16_t)ctx.nentries);
  wr32(eocd + 12, cd_size);
  wr32(eocd + 16, cd_off);
  (void)ccord_buf_append(&ctx.out, eocd, 22);

  FILE *wf = fopen(zip_path, "wb");
  bool ok = wf && fwrite(ctx.out.data, 1, ctx.out.len, wf) == ctx.out.len;
  if (wf) fclose(wf);
  for (size_t i = 0; i < ctx.nentries; i++) ccord_free(ctx.entries[i].rel);
  ccord_free(ctx.entries);
  ccord_buf_free(&ctx.out);
  if (!ok) { set_err(err_buf, err_cap, "failed writing archive"); return false; }
  return true;
}
