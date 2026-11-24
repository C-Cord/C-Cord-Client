// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "plugins_internal.h"
#include "api_v3/api_v3_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/core/utf8.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !CCORD_OS_WINDOWS
#include <sys/stat.h>
#include <unistd.h>
#endif

#define PL_UPLOAD_JOB_TIMEOUT_MS 30000u
#define PL_UPLOAD_MAX_FILES 10u
#define PL_UPLOAD_MAX_REPLACEMENT_BYTES (100u * 1024u * 1024u)

typedef struct pl_upload_job {
  uint64_t job_id;
  char owner[65];
  ccord_id guild_id;
  ccord_id channel_id;
  uint64_t deadline_ms;
  size_t nfiles;
  ccord_upload_hook_file files[PL_UPLOAD_MAX_FILES];
  char tokens[PL_UPLOAD_MAX_FILES][48];
  char tmp_paths[PL_UPLOAD_MAX_FILES][CCORD_PATH_MAX];
} pl_upload_job;

// ---------------------------------------------------------------- MIME sniffing

static bool str_ends_ci(const char *s, const char *suf)
{
  size_t sl = strlen(s), fl = strlen(suf);
  if (fl > sl) return false;
  const char *p = s + (sl - fl);
  for (size_t i = 0; i < fl; i++) {
    if (tolower((unsigned char)p[i]) != tolower((unsigned char)suf[i])) return false;
  }
  return true;
}

const char *pl_sniff_upload_mime(const char *filename, const uint8_t *data, size_t len)
{
  if (data && len >= 8u && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
  if (data && len >= 3u && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return "image/jpeg";
  if (data && len >= 6u && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0)) return "image/gif";
  if (data && len >= 12u && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) return "image/webp";
  if (data && len >= 4u && memcmp(data, "%PDF", 4) == 0) return "application/pdf";
  if (data && len >= 4u && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) return "video/webm";
  if (data && len >= 8u && memcmp(data + 4, "ftyp", 4) == 0) return "video/mp4";
  if (filename && filename[0]) {
    if (str_ends_ci(filename, ".png")) return "image/png";
    if (str_ends_ci(filename, ".jpg") || str_ends_ci(filename, ".jpeg")) return "image/jpeg";
    if (str_ends_ci(filename, ".gif")) return "image/gif";
    if (str_ends_ci(filename, ".webp")) return "image/webp";
    if (str_ends_ci(filename, ".mp4") || str_ends_ci(filename, ".m4v")) return "video/mp4";
    if (str_ends_ci(filename, ".webm")) return "video/webm";
    if (str_ends_ci(filename, ".mov")) return "video/quicktime";
    if (str_ends_ci(filename, ".mp3")) return "audio/mpeg";
    if (str_ends_ci(filename, ".ogg") || str_ends_ci(filename, ".opus")) return "audio/ogg";
    if (str_ends_ci(filename, ".wav")) return "audio/wav";
    if (str_ends_ci(filename, ".txt") || str_ends_ci(filename, ".log")) return "text/plain";
    if (str_ends_ci(filename, ".json")) return "application/json";
    if (str_ends_ci(filename, ".pdf")) return "application/pdf";
  }
  return "application/octet-stream";
}

// ---------------------------------------------------------------- Pure-C lossless EXIF / XMP / IPTC metadata strippers

static bool strip_jpeg_metadata(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len)
{
  if (len < 4u || in[0] != 0xFF || in[1] != 0xD8) return false;
  uint8_t *buf = ccord_malloc(len);
  if (!buf) return false;
  buf[0] = 0xFF;
  buf[1] = 0xD8;
  size_t wp = 2u, rp = 2u;
  bool seen_sos_or_eoi = false;

  while (rp < len) {
    if (in[rp] != 0xFF) {
      ccord_free(buf);
      return false;
    }
    while (rp < len && in[rp] == 0xFF) rp++;
    if (rp >= len) break;
    uint8_t marker = in[rp++];
    if (marker == 0xD9) {   // EOI
      if (wp + 2u > len) { ccord_free(buf); return false; }
      buf[wp++] = 0xFF;
      buf[wp++] = 0xD9;
      seen_sos_or_eoi = true;
      break;
    }
    if (marker == 0xDA) {   // SOS: copy marker + remaining entropy scan + EOI verbatim
      size_t rem = len - rp;
      if (wp + 2u + rem > len) { ccord_free(buf); return false; }
      buf[wp++] = 0xFF;
      buf[wp++] = 0xDA;
      memcpy(buf + wp, in + rp, rem);
      wp += rem;
      seen_sos_or_eoi = true;
      break;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
      if (wp + 2u > len) { ccord_free(buf); return false; }
      buf[wp++] = 0xFF;
      buf[wp++] = marker;
      continue;
    }
    if (rp + 2u > len) { ccord_free(buf); return false; }
    size_t seg_len = ((size_t)in[rp] << 8) | (size_t)in[rp + 1u];
    if (seg_len < 2u || seg_len > len - rp) { ccord_free(buf); return false; }

    // Drop APP1 (EXIF/XMP), APP13 (IPTC/Photoshop), and COM (comment) segments.
    bool drop = (marker == 0xE1 || marker == 0xED || marker == 0xFE);
    if (!drop) {
      if (wp + 2u + seg_len > len) { ccord_free(buf); return false; }
      buf[wp++] = 0xFF;
      buf[wp++] = marker;
      memcpy(buf + wp, in + rp, seg_len);
      wp += seg_len;
    }
    rp += seg_len;
  }

  if (!seen_sos_or_eoi) {
    ccord_free(buf);
    return false;
  }
  *out = buf;
  *out_len = wp;
  return true;
}

static bool strip_png_metadata(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len)
{
  static const uint8_t k_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (len < 20u || memcmp(in, k_sig, 8) != 0) return false;
  uint8_t *buf = ccord_malloc(len);
  if (!buf) return false;
  memcpy(buf, in, 8u);
  size_t wp = 8u, rp = 8u;
  bool seen_iend = false;

  while (rp + 12u <= len) {
    uint32_t clen = ((uint32_t)in[rp] << 24) | ((uint32_t)in[rp + 1u] << 16) |
                    ((uint32_t)in[rp + 2u] << 8) | (uint32_t)in[rp + 3u];
    if ((size_t)clen > len - rp - 12u) {
      ccord_free(buf);
      return false;
    }
    const uint8_t *ct = in + rp + 4u;
    size_t chunk_total = 12u + (size_t)clen;
    bool drop = (memcmp(ct, "eXIf", 4) == 0 || memcmp(ct, "tEXt", 4) == 0 ||
                 memcmp(ct, "iTXt", 4) == 0 || memcmp(ct, "zTXt", 4) == 0 ||
                 memcmp(ct, "tIME", 4) == 0);
    if (!drop) {
      if (wp + chunk_total > len) { ccord_free(buf); return false; }
      memcpy(buf + wp, in + rp, chunk_total);
      wp += chunk_total;
    }
    rp += chunk_total;
    if (memcmp(ct, "IEND", 4) == 0) {
      seen_iend = true;
      break;
    }
  }

  if (!seen_iend) {
    ccord_free(buf);
    return false;
  }
  *out = buf;
  *out_len = wp;
  return true;
}

static bool strip_webp_metadata(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len)
{
  if (len < 20u || memcmp(in, "RIFF", 4) != 0 || memcmp(in + 8, "WEBP", 4) != 0) return false;
  uint8_t *buf = ccord_malloc(len);
  if (!buf) return false;
  memcpy(buf, in, 12u);
  size_t wp = 12u, rp = 12u;

  while (rp + 8u <= len) {
    const uint8_t *fcc = in + rp;
    uint32_t csz = (uint32_t)in[rp + 4u] | ((uint32_t)in[rp + 5u] << 8) |
                   ((uint32_t)in[rp + 6u] << 16) | ((uint32_t)in[rp + 7u] << 24);
    if ((size_t)csz > len - rp - 8u) {
      ccord_free(buf);
      return false;
    }
    size_t padded = (size_t)csz + (size_t)(csz & 1u);
    size_t take = (padded <= len - rp - 8u) ? padded : (size_t)csz;
    bool drop = (memcmp(fcc, "EXIF", 4) == 0 || memcmp(fcc, "XMP ", 4) == 0);
    if (!drop) {
      if (wp + 8u + take > len) { ccord_free(buf); return false; }
      memcpy(buf + wp, in + rp, 8u + take);
      if (memcmp(fcc, "VP8X", 4) == 0 && csz >= 10u) {
        // Clear VP8X EXIF (bit 3 = 0x08) and XMP (bit 2 = 0x04) feature flags.
        buf[wp + 8u] = (uint8_t)(buf[wp + 8u] & ~0x0Cu);
      }
      wp += 8u + take;
    }
    rp += 8u + take;
  }

  uint32_t riff_sz = (uint32_t)(wp - 8u);
  buf[4] = (uint8_t)(riff_sz & 0xFFu);
  buf[5] = (uint8_t)((riff_sz >> 8) & 0xFFu);
  buf[6] = (uint8_t)((riff_sz >> 16) & 0xFFu);
  buf[7] = (uint8_t)((riff_sz >> 24) & 0xFFu);
  *out = buf;
  *out_len = wp;
  return true;
}

bool pl_strip_image_metadata(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len)
{
  if (!in || in_len == 0u || !out || !out_len) return false;
  *out = nullptr;
  *out_len = 0u;
  if (in_len >= 4u && in[0] == 0xFF && in[1] == 0xD8) {
    return strip_jpeg_metadata(in, in_len, out, out_len);
  }
  if (in_len >= 20u && memcmp(in, "\x89PNG\r\n\x1a\n", 8) == 0) {
    return strip_png_metadata(in, in_len, out, out_len);
  }
  if (in_len >= 20u && memcmp(in, "RIFF", 4) == 0 && memcmp(in + 8, "WEBP", 4) == 0) {
    return strip_webp_metadata(in, in_len, out, out_len);
  }
  return false;
}

// ---------------------------------------------------------------- Filename & patch helpers

static bool sanitize_upload_filename(const char *in, char *out, size_t cap)
{
  if (!in || !in[0] || !out || cap < 2u) return false;
  size_t len = strlen(in);
  if (len >= cap || !ccord_utf8_valid_text(in, len)) return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)in[i];
    if (ch < 0x20u || ch == '/' || ch == '\\') return false;
  }
  if (strcmp(in, ".") == 0 || strcmp(in, "..") == 0) return false;
  memcpy(out, in, len + 1u);
  return true;
}

static void replace_ext_for_format(char *filename, size_t cap, const char *fmt)
{
  const char *ext = (strcmp(fmt, "webp") == 0) ? ".webp"
                  : (strcmp(fmt, "png") == 0)  ? ".png"
                  : ".jpg";
  char *dot = strrchr(filename, '.');
  size_t base_len = dot ? (size_t)(dot - filename) : strlen(filename);
  size_t ext_len = strlen(ext);
  if (base_len + ext_len >= cap) base_len = cap - ext_len - 1u;
  memcpy(filename + base_len, ext, ext_len + 1u);
}

static const ccord_json *lookup_file_patch(const ccord_json *files_j, size_t idx_zero_based)
{
  if (!files_j) return nullptr;
  ccord_json_type t = ccord_json_type_of(files_j);
  if (t == CCORD_JSON_ARRAY) {
    return ccord_json_array_at(files_j, idx_zero_based);
  }
  if (t == CCORD_JSON_OBJECT) {
    char key[16];
    snprintf(key, sizeof key, "%zu", idx_zero_based + 1u);
    return ccord_json_get(files_j, key);
  }
  return nullptr;
}

static void apply_file_patch(ccord_plugins *m, plugin *p, ccord_upload_hook_file *f, const ccord_json *pf,
                             bool allow_replacement, bool *out_want_async)
{
  if (!f || !pf || ccord_json_type_of(pf) != CCORD_JSON_OBJECT) return;
  if (ccord_json_bool(ccord_json_get(pf, "drop"), false)) {
    f->dropped = true;
    return;
  }
  bool renamed = false;
  size_t fl = 0;
  const char *new_name = pl_str_param(pf, "filename", &fl);
  if (new_name && fl > 0u) {
    char clean[128];
    if (sanitize_upload_filename(new_name, clean, sizeof clean)) {
      snprintf(f->filename, sizeof f->filename, "%s", clean);
      renamed = true;
    }
  }
  const ccord_json *spj = ccord_json_get(pf, "spoiler");
  if (spj && ccord_json_type_of(spj) == CCORD_JSON_BOOL) {
    f->spoiler = ccord_json_bool(spj, f->spoiler);
  }

  if (allow_replacement) {
    size_t rl = 0;
    const char *rpath = pl_str_param(pf, "replacement_path", &rl);
    if (rpath && rl > 0u) {
      pl_path_scope scope = PL_PATH_HOST_FS;
      char resolved[CCORD_PATH_MAX];
      const char *ecode = nullptr;
      char ebuf[160] = "";
      if (pl_resolve_resource_path(m, p, rpath, false, &scope, resolved, sizeof resolved, &ecode, ebuf, sizeof ebuf)) {
        uint8_t *raw = nullptr;
        size_t rlen = 0u;
        if (pl_read_resolved_file(resolved, PL_UPLOAD_MAX_REPLACEMENT_BYTES, &raw, &rlen, &ecode, ebuf, sizeof ebuf)) {
          ccord_free(f->data);
          f->data = raw;
          f->size = rlen;
        }
      }
    }
  }

  if (ccord_json_bool(ccord_json_get(pf, "strip_metadata"), false) && f->data && f->size > 0u) {
    uint8_t *stripped = nullptr;
    size_t slen = 0u;
    if (pl_strip_image_metadata(f->data, f->size, &stripped, &slen) && stripped && slen > 0u) {
      ccord_free(f->data);
      f->data = stripped;
      f->size = slen;
    }
  }

  const ccord_json *rej = ccord_json_get(pf, "reencode");
  if (rej && ccord_json_type_of(rej) == CCORD_JSON_OBJECT && m->plat_ops.reencode_image && f->data && f->size > 0u) {
    size_t fmtl = 0;
    const char *fmt = pl_str_param(rej, "format", &fmtl);
    if (fmt && (strcmp(fmt, "webp") == 0 || strcmp(fmt, "jpeg") == 0 || strcmp(fmt, "jpg") == 0 || strcmp(fmt, "png") == 0)) {
      int64_t qual = 85, max_dim = 0;
      (void)ccord_json_int64(ccord_json_get(rej, "quality"), &qual);
      (void)ccord_json_int64(ccord_json_get(rej, "max_dimension"), &max_dim);
      if (qual < 1) qual = 1;
      if (qual > 100) qual = 100;
      uint8_t *enc = nullptr;
      size_t elen = 0u;
      char ebuf[128] = "";
      if (m->plat_ops.reencode_image(m->plat_ops.ud, f->data, f->size, fmt, (int)qual, (int)max_dim,
                                     &enc, &elen, ebuf, sizeof ebuf) && enc && elen > 0u) {
        ccord_free(f->data);
        f->data = enc;
        f->size = elen;
        if (!renamed) replace_ext_for_format(f->filename, sizeof f->filename, fmt);
      }
    }
  }

  if (out_want_async && ccord_json_bool(ccord_json_get(pf, "async_transform"), false)) {
    f->async_transform = true;
    *out_want_async = true;
  }
}

// ---------------------------------------------------------------- Stage 2 async temp-file lifecycle

static void upload_job_cleanup_files(pl_upload_job *job, bool free_buffers)
{
  if (!job) return;
  for (size_t i = 0; i < job->nfiles && i < PL_UPLOAD_MAX_FILES; i++) {
    if (job->tmp_paths[i][0]) {
      (void)remove(job->tmp_paths[i]);
      job->tmp_paths[i][0] = '\0';
    }
    if (free_buffers) {
      ccord_free(job->files[i].data);
      job->files[i].data = nullptr;
      job->files[i].size = 0u;
    }
  }
}

static void upload_job_remove_at(ccord_plugins *m, size_t idx, bool free_buffers)
{
  if (!m || idx >= m->nupload_jobs) return;
  pl_upload_job *job = m->upload_jobs[idx];
  upload_job_cleanup_files(job, free_buffers);
  ccord_free(job);
  if (idx + 1u < m->nupload_jobs) {
    memmove(&m->upload_jobs[idx], &m->upload_jobs[idx + 1u], (m->nupload_jobs - idx - 1u) * sizeof m->upload_jobs[0]);
  }
  m->nupload_jobs--;
}

static bool stage_upload_tmp_dir(ccord_plugins *m, char *out_dir, size_t cap)
{
  if (m->has_data_dir && m->data_dir && m->data_dir[0]) {
    int n = snprintf(out_dir, cap, "%s/plugin_upload_tmp", m->data_dir);
    if (n <= 0 || (size_t)n >= cap) return false;
  } else {
    int n = snprintf(out_dir, cap, "/tmp/ccord_upload_tmp_%llu", (unsigned long long)ccord_os_pid());
    if (n <= 0 || (size_t)n >= cap) return false;
  }
  ccord_error err = {0};
  return ccord_mkdir_p(out_dir, 0700, &err) == CCORD_OK;
}

static bool stage_async_upload_job(ccord_plugins *m, plugin *p, ccord_id guild_id, ccord_id channel_id,
                                   const char *content, ccord_upload_hook_file *files, size_t count,
                                   uint64_t *out_job_id)
{
  char tmp_dir[CCORD_PATH_MAX];
  if (!stage_upload_tmp_dir(m, tmp_dir, sizeof tmp_dir)) return false;

  pl_upload_job *job = ccord_calloc(1, sizeof *job);
  if (!job) return false;
  job->job_id = ++m->next_upload_job_id;
  snprintf(job->owner, sizeof job->owner, "%s", p->m.id);
  job->guild_id = guild_id;
  job->channel_id = channel_id;
  uint64_t now = m->now_ms ? m->now_ms : ccord_clock_ms();
  job->deadline_ms = now + PL_UPLOAD_JOB_TIMEOUT_MS;
  job->nfiles = count < PL_UPLOAD_MAX_FILES ? count : PL_UPLOAD_MAX_FILES;

  for (size_t i = 0; i < job->nfiles; i++) {
    job->files[i] = files[i];
    job->files[i].data = nullptr;
    if (files[i].data && files[i].size > 0u) {
      job->files[i].data = ccord_malloc(files[i].size);
      if (!job->files[i].data) {
        upload_job_cleanup_files(job, true);
        ccord_free(job);
        return false;
      }
      memcpy(job->files[i].data, files[i].data, files[i].size);
    }
    if (!files[i].dropped && files[i].async_transform && files[i].data && files[i].size > 0u) {
      uint64_t mix = (uint64_t)ccord_clock_ms() ^ ((uint64_t)ccord_os_pid() << 20) ^ (job->job_id * 0x9E3779B97F4A7C15ull) ^ (i + 1u);
      snprintf(job->tokens[i], sizeof job->tokens[i], "%016llx%08llx",
               (unsigned long long)mix, (unsigned long long)(job->job_id * 31u + i));
      int n = snprintf(job->tmp_paths[i], sizeof job->tmp_paths[i], "%s/%s.bin", tmp_dir, job->tokens[i]);
      if (n <= 0 || (size_t)n >= sizeof job->tmp_paths[i]) {
        upload_job_cleanup_files(job, true);
        ccord_free(job);
        return false;
      }
      FILE *fp = fopen(job->tmp_paths[i], "wb");
      if (!fp) {
        upload_job_cleanup_files(job, true);
        ccord_free(job);
        return false;
      }
      size_t wr = fwrite(files[i].data, 1, files[i].size, fp);
      fclose(fp);
#if !CCORD_OS_WINDOWS
      (void)chmod(job->tmp_paths[i], 0600);
#endif
      if (wr != files[i].size) {
        upload_job_cleanup_files(job, true);
        ccord_free(job);
        return false;
      }
    }
  }

  pl_upload_job **grown = ccord_realloc_array(m->upload_jobs, m->nupload_jobs + 1u, sizeof *grown);
  if (!grown) {
    upload_job_cleanup_files(job, true);
    ccord_free(job);
    return false;
  }
  m->upload_jobs = grown;
  m->upload_jobs[m->nupload_jobs++] = job;

  ccord_buf b; ccord_buf_init(&b, 0);
  ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "job_id"); ccord_jw_int(&w, (int64_t)job->job_id);
  ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, guild_id);
  ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, channel_id);
  ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, content ? content : "");
  ccord_jw_key(&w, "files");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < job->nfiles; i++) {
    if (job->files[i].dropped) continue;
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "index"); ccord_jw_int(&w, (int64_t)(i + 1u));
    ccord_jw_key(&w, "filename"); ccord_jw_cstr(&w, job->files[i].filename);
    ccord_jw_key(&w, "size"); ccord_jw_int(&w, (int64_t)job->files[i].size);
    ccord_jw_key(&w, "mime"); ccord_jw_cstr(&w, pl_sniff_upload_mime(job->files[i].filename, job->files[i].data, job->files[i].size));
    ccord_jw_key(&w, "spoiler"); ccord_jw_bool(&w, job->files[i].spoiler);
    if (job->tokens[i][0]) {
      char hbuf[80];
      snprintf(hbuf, sizeof hbuf, "upload_tmp://%s", job->tokens[i]);
      ccord_jw_key(&w, "handle"); ccord_jw_cstr(&w, hbuf);
    }
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "hook.upload_transform", ccord_buf_cstr(&b));
  ccord_buf_free(&b);

  *out_job_id = job->job_id;
  return true;
}

bool pl_upload_tmp_resolve(const ccord_plugins *m, const plugin *p, const char *token, char *out_abs, size_t cap)
{
  if (!m || !p || !token || !token[0] || !out_abs || cap == 0u) return false;
  out_abs[0] = '\0';
  size_t tl = strlen(token);
  if (tl == 0u || tl >= 48u) return false;
  for (size_t i = 0; i < tl; i++) {
    unsigned char ch = (unsigned char)token[i];
    if (!isalnum(ch)) return false;
  }
  for (size_t j = 0; j < m->nupload_jobs; j++) {
    const pl_upload_job *job = m->upload_jobs[j];
    if (!job || strcmp(job->owner, p->m.id) != 0) continue;
    for (size_t i = 0; i < job->nfiles && i < PL_UPLOAD_MAX_FILES; i++) {
      if (job->tokens[i][0] && strcmp(job->tokens[i], token) == 0 && job->tmp_paths[i][0]) {
        if (strlen(job->tmp_paths[i]) >= cap) return false;
        snprintf(out_abs, cap, "%s", job->tmp_paths[i]);
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------- Public plugin manager entry points

void plugins_ext_on_before_upload(void *ud, ccord_id guild_id, ccord_id channel_id, const char *content,
                                  ccord_upload_hook_file *files, size_t count, ccord_upload_hook_result *out)
{
  ccord_plugins *m = ud;
  if (out) *out = (ccord_upload_hook_result){0};
  if (!m || !files || count == 0u || !out) return;
  if (count > PL_UPLOAD_MAX_FILES) count = PL_UPLOAD_MAX_FILES;

  plugin *async_plugin = nullptr;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    if (!p->used || p->state != CCORD_PLUGIN_READY || !(p->hook_mask & HOOK_BEFORE_UPLOAD)) continue;

    ccord_buf b; ccord_buf_init(&b, 0);
    ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "hook"); ccord_jw_cstr(&w, "before_upload");
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, guild_id);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, channel_id);
    ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, content ? content : "");
    ccord_jw_key(&w, "files");
    ccord_jw_begin_array(&w);
    for (size_t fi = 0; fi < count; fi++) {
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "index"); ccord_jw_int(&w, (int64_t)(fi + 1u));
      ccord_jw_key(&w, "filename"); ccord_jw_cstr(&w, files[fi].filename);
      ccord_jw_key(&w, "size"); ccord_jw_int(&w, (int64_t)files[fi].size);
      ccord_jw_key(&w, "mime"); ccord_jw_cstr(&w, pl_sniff_upload_mime(files[fi].filename, files[fi].data, files[fi].size));
      ccord_jw_key(&w, "spoiler"); ccord_jw_bool(&w, files[fi].spoiler);
      ccord_jw_key(&w, "dropped"); ccord_jw_bool(&w, files[fi].dropped);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
    ccord_jw_end_object(&w);
    ccord_jw_end_object(&w);

    char *res = nullptr;
    char eb[128] = "";
    bool ok = ccord_jw_ok(&w) && ext_sync_host_call(m, p, "hook.call", ccord_buf_cstr(&b), 150u, &res, eb, sizeof eb);
    ccord_buf_free(&b);
    if (!ok || !res) {
      ccord_free(res);
      continue;
    }

    ccord_arena ar; ccord_arena_init(&ar, 0);
    ccord_error pe = {0};
    const ccord_json *rj = ccord_json_parse(&ar, res, strlen(res), &k_ext_limits, &pe);
    if (rj && ccord_json_type_of(rj) == CCORD_JSON_OBJECT) {
      size_t nl = 0;
      const char *nt = pl_str_param(rj, "notice", &nl);
      if (nt && nl > 0u) pl_set_text(out->notice, sizeof out->notice, nt);
      if (ccord_json_bool(ccord_json_get(rj, "cancel"), false)) {
        out->cancelled = true;
        ccord_arena_free(&ar);
        ccord_free(res);
        return;
      }
      const ccord_json *fj = ccord_json_get(rj, "files");
      if (fj) {
        bool want_async = false;
        for (size_t fi = 0; fi < count; fi++) {
          if (files[fi].dropped) continue;
          const ccord_json *pf = lookup_file_patch(fj, fi);
          if (pf) apply_file_patch(m, p, &files[fi], pf, false, &want_async);
        }
        if (want_async && !async_plugin) async_plugin = p;
      }
    }
    ccord_arena_free(&ar);
    ccord_free(res);
  }

  if (async_plugin) {
    uint64_t jid = 0u;
    if (stage_async_upload_job(m, async_plugin, guild_id, channel_id, content, files, count, &jid)) {
      out->async_pending = true;
      out->async_job_id = jid;
    }
  }
}

bool pl_handle_upload_transform_complete(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  if (!pl_has_perm(p, CCORD_PERM_MESSAGES_HOOK)) {
    pl_fail_plugin(m, p, "undeclared restricted API called: messages.hook");
    return true;
  }
  int64_t jid_i = 0;
  (void)ccord_json_int64(ccord_json_get(params, "job_id"), &jid_i);
  if (jid_i <= 0) {
    pl_reply_err(m, p, id, "invalid_argument", "hooks.complete_upload_transform: job_id required");
    return true;
  }
  uint64_t job_id = (uint64_t)jid_i;
  size_t found = SIZE_MAX;
  for (size_t i = 0; i < m->nupload_jobs; i++) {
    if (m->upload_jobs[i]->job_id == job_id && strcmp(m->upload_jobs[i]->owner, p->m.id) == 0) {
      found = i;
      break;
    }
  }
  if (found == SIZE_MAX) {
    pl_reply_err(m, p, id, "not_found", "hooks.complete_upload_transform: unknown or expired job_id");
    return true;
  }

  pl_upload_job *job = m->upload_jobs[found];
  const ccord_json *patch = ccord_json_get(params, "patch");
  bool cancelled = false;
  char notice[160] = "";
  if (patch && ccord_json_type_of(patch) == CCORD_JSON_OBJECT) {
    size_t nl = 0;
    const char *nt = pl_str_param(patch, "notice", &nl);
    if (nt && nl > 0u) pl_set_text(notice, sizeof notice, nt);
    cancelled = ccord_json_bool(ccord_json_get(patch, "cancel"), false);
    if (!cancelled) {
      const ccord_json *fj = ccord_json_get(patch, "files");
      if (fj) {
        for (size_t fi = 0; fi < job->nfiles; fi++) {
          if (job->files[fi].dropped) continue;
          const ccord_json *pf = lookup_file_patch(fj, fi);
          if (pf) apply_file_patch(m, p, &job->files[fi], pf, true, nullptr);
        }
      }
    }
  }

  // Detach job before notifying UI so re-entrant calls cannot observe stale handles.
  ccord_upload_hook_file out_files[PL_UPLOAD_MAX_FILES];
  size_t nfiles = job->nfiles;
  memcpy(out_files, job->files, nfiles * sizeof out_files[0]);
  upload_job_remove_at(m, found, false);

  pl_reply_ok(m, p, id, nullptr);
  ccord_contrib_ui_upload_transform_complete(m->contrib, job_id, cancelled, notice, out_files, nfiles);
  for (size_t i = 0; i < nfiles; i++) ccord_free(out_files[i].data);
  return true;
}

void plugins_ext_on_cancel_upload_transform(void *ud, uint64_t job_id)
{
  ccord_plugins *m = ud;
  if (!m || job_id == 0u) return;
  for (size_t i = 0; i < m->nupload_jobs; i++) {
    if (m->upload_jobs[i]->job_id == job_id) {
      upload_job_remove_at(m, i, true);
      return;
    }
  }
}

void pl_upload_jobs_pump(ccord_plugins *m, uint64_t now_ms)
{
  if (!m || m->nupload_jobs == 0u) return;
  for (size_t i = m->nupload_jobs; i-- > 0;) {
    pl_upload_job *job = m->upload_jobs[i];
    if (now_ms < job->deadline_ms) continue;
    uint64_t jid = job->job_id;
    upload_job_remove_at(m, i, true);
    ccord_contrib_ui_upload_transform_complete(m->contrib, jid, true, "Attachment transform timed out", nullptr, 0);
  }
}

void pl_upload_jobs_cancel_owner(ccord_plugins *m, const char *owner)
{
  if (!m || !owner || !owner[0]) return;
  for (size_t i = m->nupload_jobs; i-- > 0;) {
    pl_upload_job *job = m->upload_jobs[i];
    if (strcmp(job->owner, owner) != 0) continue;
    uint64_t jid = job->job_id;
    upload_job_remove_at(m, i, true);
    ccord_contrib_ui_upload_transform_complete(m->contrib, jid, true, "Plugin unloaded during attachment processing", nullptr, 0);
  }
}

void pl_upload_jobs_destroy(ccord_plugins *m)
{
  if (!m) return;
  while (m->nupload_jobs > 0u) {
    upload_job_remove_at(m, m->nupload_jobs - 1u, true);
  }
  ccord_free(m->upload_jobs);
  m->upload_jobs = nullptr;
}
