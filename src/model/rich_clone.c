// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <assert.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/model/rich.h>

// Single exact-size allocation: ccord_message_rich sits at offset 0 and every owned array and non-empty string
// follows in the same block. Empty/null strings share the static "" literal and consume 0 bytes.
typedef struct rich_sizer {
  size_t total;
  bool ok;
} rich_sizer;

typedef struct rich_writer {
  unsigned char *buf;
  size_t off;
  size_t cap;
} rich_writer;

static void size_str(rich_sizer *sz, const char *s)
{
  if (!sz->ok || !s || s[0] == '\0') return;
  size_t len = strlen(s);
  size_t need = 0;
  size_t next = 0;
  if (ckd_add(&need, len, (size_t)1u) || ckd_add(&next, sz->total, need)) {
    sz->ok = false;
    return;
  }
  sz->total = next;
}

static const char *write_str(rich_writer *w, const char *s)
{
  if (!s || s[0] == '\0') return "";
  size_t len = strlen(s);
  size_t need = len + 1u;
  assert(need <= w->cap - w->off);
  if (need > w->cap - w->off) return "";
  char *d = (char *)(w->buf + w->off);
  memcpy(d, s, need);
  w->off += need;
  return d;
}

static bool size_array(rich_sizer *sz, const void *src, size_t n, size_t elem_size)
{
  if (!sz->ok || n == 0 || !src) return false;
  const size_t align = alignof(max_align_t);
  size_t bytes = 0;
  size_t padded = 0;
  if (ckd_mul(&bytes, n, elem_size) || ckd_add(&padded, sz->total, align - 1u)) {
    sz->ok = false;
    return false;
  }
  size_t aligned = padded & ~(align - 1u);
  size_t next = 0;
  if (ckd_add(&next, aligned, bytes)) {
    sz->ok = false;
    return false;
  }
  sz->total = next;
  return true;
}

static void *write_array(rich_writer *w, const void *src, size_t n, size_t elem_size)
{
  if (n == 0 || !src) return nullptr;
  const size_t align = alignof(max_align_t);
  size_t aligned = (w->off + (align - 1u)) & ~(align - 1u);
  size_t bytes = n * elem_size;
  assert(aligned <= w->cap && bytes <= w->cap - aligned);
  if (aligned > w->cap || bytes > w->cap - aligned) return nullptr;
  void *d = w->buf + aligned;
  memcpy(d, src, bytes);
  w->off = aligned + bytes;
  return d;
}

static void measure_media(rich_sizer *sz, const ccord_embed_media *m)
{
  size_str(sz, m->url);
  size_str(sz, m->proxy_url);
}

static void write_media(rich_writer *w, ccord_embed_media *m)
{
  m->url = write_str(w, m->url);
  m->proxy_url = write_str(w, m->proxy_url);
}

static void measure_attachments(rich_sizer *sz, const ccord_attachment *src, size_t n)
{
  if (!size_array(sz, src, n, sizeof *src)) return;
  for (size_t i = 0; i < n && sz->ok; i++) {
    size_str(sz, src[i].filename);
    size_str(sz, src[i].description);
    size_str(sz, src[i].content_type);
    size_str(sz, src[i].url);
    size_str(sz, src[i].proxy_url);
    size_str(sz, src[i].waveform_b64);
  }
}

static ccord_attachment *write_attachments(rich_writer *w, const ccord_attachment *src, size_t n)
{
  ccord_attachment *d = write_array(w, src, n, sizeof *d);
  for (size_t i = 0; d && i < n; i++) {
    d[i].filename = write_str(w, d[i].filename);
    d[i].description = write_str(w, d[i].description);
    d[i].content_type = write_str(w, d[i].content_type);
    d[i].url = write_str(w, d[i].url);
    d[i].proxy_url = write_str(w, d[i].proxy_url);
    d[i].waveform_b64 = write_str(w, d[i].waveform_b64);
  }
  return d;
}

static void measure_embeds(rich_sizer *sz, const ccord_embed *src, size_t n)
{
  if (!size_array(sz, src, n, sizeof *src)) return;
  for (size_t i = 0; i < n && sz->ok; i++) {
    const ccord_embed *e = &src[i];
    size_str(sz, e->title);
    size_str(sz, e->description);
    size_str(sz, e->url);
    size_str(sz, e->provider);
    size_str(sz, e->author_name);
    size_str(sz, e->author_url);
    size_str(sz, e->author_icon);
    size_str(sz, e->footer_text);
    size_str(sz, e->footer_icon);
    measure_media(sz, &e->image);
    measure_media(sz, &e->thumbnail);
    measure_media(sz, &e->video);
    if (size_array(sz, e->fields, e->field_count, sizeof *e->fields)) {
      for (size_t k = 0; k < e->field_count && sz->ok; k++) {
        size_str(sz, e->fields[k].name);
        size_str(sz, e->fields[k].value);
      }
    }
  }
}

static ccord_embed *write_embeds(rich_writer *w, const ccord_embed *src, size_t n)
{
  ccord_embed *d = write_array(w, src, n, sizeof *d);
  for (size_t i = 0; d && i < n; i++) {
    ccord_embed *e = &d[i];
    e->title = write_str(w, e->title);
    e->description = write_str(w, e->description);
    e->url = write_str(w, e->url);
    e->provider = write_str(w, e->provider);
    e->author_name = write_str(w, e->author_name);
    e->author_url = write_str(w, e->author_url);
    e->author_icon = write_str(w, e->author_icon);
    e->footer_text = write_str(w, e->footer_text);
    e->footer_icon = write_str(w, e->footer_icon);
    write_media(w, &e->image);
    write_media(w, &e->thumbnail);
    write_media(w, &e->video);
    ccord_embed_field *f = write_array(w, e->fields, e->field_count, sizeof *f);
    for (size_t k = 0; f && k < e->field_count; k++) {
      f[k].name = write_str(w, f[k].name);
      f[k].value = write_str(w, f[k].value);
    }
    e->fields = f;
    if (!f) e->field_count = 0;
  }
  return d;
}

static void measure_stickers(rich_sizer *sz, const ccord_msg_sticker *src, size_t n)
{
  if (!size_array(sz, src, n, sizeof *src)) return;
  for (size_t i = 0; i < n && sz->ok; i++) size_str(sz, src[i].name);
}

static ccord_msg_sticker *write_stickers(rich_writer *w, const ccord_msg_sticker *src, size_t n)
{
  ccord_msg_sticker *d = write_array(w, src, n, sizeof *d);
  for (size_t i = 0; d && i < n; i++) d[i].name = write_str(w, d[i].name);
  return d;
}

static void measure_media_ref(rich_sizer *sz, const ccord_media_ref *m)
{
  size_str(sz, m->url);
  size_str(sz, m->proxy_url);
  size_str(sz, m->content_type);
  size_str(sz, m->description);
}

static void write_media_ref(rich_writer *w, ccord_media_ref *m)
{
  m->url = write_str(w, m->url);
  m->proxy_url = write_str(w, m->proxy_url);
  m->content_type = write_str(w, m->content_type);
  m->description = write_str(w, m->description);
}

// A clone never follows a tree deeper than this (validate rejects anything past CCORD_RICH_MAX_V2_DEPTH); it only bounds
// the recursion for a producer that did not validate.
#define CLONE_MAX_DEPTH 8u

static void measure_nodes(rich_sizer *sz, const ccord_component *src, size_t n, unsigned depth);

static void measure_node_fields(rich_sizer *sz, const ccord_component *x, unsigned depth)
{
  size_str(sz, x->label);
  size_str(sz, x->custom_id);
  size_str(sz, x->url);
  size_str(sz, x->placeholder);
  size_str(sz, x->emoji);
  size_str(sz, x->content);
  size_str(sz, x->file_name);
  measure_media_ref(sz, &x->media);
  if (size_array(sz, x->options, x->option_count, sizeof *x->options)) {
    for (size_t j = 0; j < x->option_count && sz->ok; j++) {
      size_str(sz, x->options[j].label);
      size_str(sz, x->options[j].value);
      size_str(sz, x->options[j].description);
      size_str(sz, x->options[j].emoji);
    }
  }
  if (size_array(sz, x->gallery, x->gallery_count, sizeof *x->gallery)) {
    for (size_t j = 0; j < x->gallery_count && sz->ok; j++) measure_media_ref(sz, &x->gallery[j]);
  }
  if (depth < CLONE_MAX_DEPTH) {
    measure_nodes(sz, x->children, x->child_count, depth + 1u);
    measure_nodes(sz, x->accessory, x->accessory ? 1u : 0u, depth + 1u);
  }
}

static void measure_nodes(rich_sizer *sz, const ccord_component *src, size_t n, unsigned depth)
{
  if (!size_array(sz, src, n, sizeof *src)) return;
  for (size_t i = 0; i < n && sz->ok; i++) measure_node_fields(sz, &src[i], depth);
}

static ccord_component *write_nodes(rich_writer *w, const ccord_component *src, size_t n, unsigned depth);

// Makes the strings and arrays of `x`, a node whose fields were already copied by value, its own.
static void write_node_fields(rich_writer *w, ccord_component *x, unsigned depth)
{
  x->label = write_str(w, x->label);
  x->custom_id = write_str(w, x->custom_id);
  x->url = write_str(w, x->url);
  x->placeholder = write_str(w, x->placeholder);
  x->emoji = write_str(w, x->emoji);
  x->content = write_str(w, x->content);
  x->file_name = write_str(w, x->file_name);
  write_media_ref(w, &x->media);
  ccord_select_option *opts = write_array(w, x->options, x->option_count, sizeof *opts);
  for (size_t j = 0; opts && j < x->option_count; j++) {
    opts[j].label = write_str(w, opts[j].label);
    opts[j].value = write_str(w, opts[j].value);
    opts[j].description = write_str(w, opts[j].description);
    opts[j].emoji = write_str(w, opts[j].emoji);
  }
  x->options = opts;
  if (!opts) x->option_count = 0;
  ccord_media_ref *gal = write_array(w, x->gallery, x->gallery_count, sizeof *gal);
  for (size_t j = 0; gal && j < x->gallery_count; j++) write_media_ref(w, &gal[j]);
  x->gallery = gal;
  if (!gal) x->gallery_count = 0;
  if (depth < CLONE_MAX_DEPTH) {
    x->children = write_nodes(w, x->children, x->child_count, depth + 1u);
    x->accessory = write_nodes(w, x->accessory, x->accessory ? 1u : 0u, depth + 1u);
  } else {
    x->children = nullptr;
    x->accessory = nullptr;
  }
  if (!x->children) x->child_count = 0;
}

// A deep copy of `n` nodes whose descendants sit at `depth` and below (a top-level node is depth 1).
static ccord_component *write_nodes(rich_writer *w, const ccord_component *src, size_t n, unsigned depth)
{
  ccord_component *d = write_array(w, src, n, sizeof *d);
  for (size_t i = 0; d && i < n; i++) write_node_fields(w, &d[i], depth);
  return d;
}

static void measure_components(rich_sizer *sz, const ccord_component_row *src, size_t n)
{
  if (!size_array(sz, src, n, sizeof *src)) return;
  for (size_t i = 0; i < n && sz->ok; i++) {
    measure_nodes(sz, src[i].components, src[i].component_count, 1u);
  }
}

static ccord_component_row *write_components(rich_writer *w, const ccord_component_row *src, size_t n)
{
  ccord_component_row *rows = write_array(w, src, n, sizeof *rows);
  for (size_t i = 0; rows && i < n; i++) {
    rows[i].components = write_nodes(w, rows[i].components, rows[i].component_count, 1u);
    if (!rows[i].components) rows[i].component_count = 0;
  }
  return rows;
}

static void measure_rich(rich_sizer *sz, const ccord_message_rich *src)
{
  (void)size_array(sz, src->mention_users, src->mention_user_count, sizeof(ccord_id));
  (void)size_array(sz, src->mention_roles, src->mention_role_count, sizeof(ccord_id));
  measure_attachments(sz, src->attachments, src->attachment_count);
  measure_embeds(sz, src->embeds, src->embed_count);
  measure_stickers(sz, src->stickers, src->sticker_count);
  size_str(sz, src->ref_preview.author_name);
  size_str(sz, src->ref_preview.content);
  size_str(sz, src->forward.content);
  measure_attachments(sz, src->forward.attachments, src->forward.attachment_count);
  measure_embeds(sz, src->forward.embeds, src->forward.embed_count);
  measure_stickers(sz, src->forward.stickers, src->forward.sticker_count);
  if (src->poll.present) {
    size_str(sz, src->poll.question);
    size_t n_ans = src->poll.answer_count < CCORD_RICH_MAX_POLL_ANSWERS
                     ? src->poll.answer_count
                     : CCORD_RICH_MAX_POLL_ANSWERS;
    if (size_array(sz, src->poll.answers, n_ans, sizeof(ccord_poll_answer))) {
      for (size_t i = 0; i < n_ans && sz->ok; i++) {
        size_str(sz, src->poll.answers[i].text);
        size_str(sz, src->poll.answers[i].emoji);
      }
    }
  }
  size_str(sz, src->thread.name);
  measure_components(sz, src->components, src->component_row_count);
  measure_nodes(sz, src->v2_components, src->v2_component_count, 1u);
}

// Copies `src` into the writer's pre-sized block. `out` is overwritten.
static void write_rich(rich_writer *w, const ccord_message_rich *src, ccord_message_rich *out)
{
  *out = *src;
  out->mention_users = write_array(w, src->mention_users, src->mention_user_count, sizeof(ccord_id));
  if (!out->mention_users) out->mention_user_count = 0;
  out->mention_roles = write_array(w, src->mention_roles, src->mention_role_count, sizeof(ccord_id));
  if (!out->mention_roles) out->mention_role_count = 0;
  out->attachments = write_attachments(w, src->attachments, src->attachment_count);
  if (!out->attachments) out->attachment_count = 0;
  out->embeds = write_embeds(w, src->embeds, src->embed_count);
  if (!out->embeds) out->embed_count = 0;
  out->stickers = write_stickers(w, src->stickers, src->sticker_count);
  if (!out->stickers) out->sticker_count = 0;
  out->ref_preview.author_name = write_str(w, src->ref_preview.author_name);
  out->ref_preview.content = write_str(w, src->ref_preview.content);
  out->forward.content = write_str(w, src->forward.content);
  out->forward.attachments = write_attachments(w, src->forward.attachments, src->forward.attachment_count);
  if (!out->forward.attachments) out->forward.attachment_count = 0;
  out->forward.embeds = write_embeds(w, src->forward.embeds, src->forward.embed_count);
  if (!out->forward.embeds) out->forward.embed_count = 0;
  out->forward.stickers = write_stickers(w, src->forward.stickers, src->forward.sticker_count);
  if (!out->forward.stickers) out->forward.sticker_count = 0;
  if (src->poll.present) {
    out->poll.question = write_str(w, src->poll.question);
    size_t n_ans = src->poll.answer_count < CCORD_RICH_MAX_POLL_ANSWERS
                     ? src->poll.answer_count
                     : CCORD_RICH_MAX_POLL_ANSWERS;
    ccord_poll_answer *pa = write_array(w, src->poll.answers, n_ans, sizeof *pa);
    out->poll.answers = pa;
    out->poll.answer_count = pa ? n_ans : 0u;
    for (size_t i = 0; pa && i < n_ans; i++) {
      pa[i].text = write_str(w, src->poll.answers[i].text);
      pa[i].emoji = write_str(w, src->poll.answers[i].emoji);
    }
  } else {
    out->poll.question = "";
    out->poll.answers = nullptr;
    out->poll.answer_count = 0;
  }
  out->thread.name = write_str(w, src->thread.name);
  out->components = write_components(w, src->components, src->component_row_count);
  if (!out->components) out->component_row_count = 0;
  out->v2_components = write_nodes(w, src->v2_components, src->v2_component_count, 1u);
  if (!out->v2_components) out->v2_component_count = 0;
}

ccord_message_rich *ccord_message_rich_clone(const ccord_message_rich *src, ccord_error *err)
{
  ccord_error_clear(err);
  if (!src) return nullptr;
  rich_sizer sz = {.total = sizeof(ccord_message_rich), .ok = true};
  measure_rich(&sz, src);
  if (!sz.ok) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "rich: out of memory");
    return nullptr;
  }
  void *raw = ccord_malloc(sz.total);
  if (!raw) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "rich: out of memory");
    return nullptr;
  }
  ccord_message_rich *out = raw;
  rich_writer w = {.buf = raw, .off = sizeof(ccord_message_rich), .cap = sz.total};
  write_rich(&w, src, out);
  assert(w.off == w.cap);
  return out;
}

ccord_message_rich *ccord_message_rich_merge(const ccord_message_rich *old, const ccord_message_rich *upd, ccord_error *err)
{
  ccord_error_clear(err);
  if (!old && !upd) return nullptr;
  if (!old || !upd) {
    ccord_message_rich *only = ccord_message_rich_clone(old ? old : upd, err);
    if (only) only->text_absent = false;
    return only;
  }
  ccord_message_rich view = *old;
  uint32_t p = upd->present;
  if (p & CCORD_RICH_HAS_TYPE) {
    view.type = upd->type;
  }
  if (p & CCORD_RICH_HAS_FLAGS) {
    view.flags = upd->flags;
  } else if ((p & CCORD_RICH_HAS_TYPE) != 0 && upd->flags != 0) {
    view.flags = upd->flags;
  }
  if (p & CCORD_RICH_HAS_MENTIONS) {
    view.mention_everyone = upd->mention_everyone;
    view.mention_users = upd->mention_users;
    view.mention_user_count = upd->mention_user_count;
    view.mention_roles = upd->mention_roles;
    view.mention_role_count = upd->mention_role_count;
  }
  if (p & CCORD_RICH_HAS_ATTACHMENTS) {
    view.attachments = upd->attachments;
    view.attachment_count = upd->attachment_count;
  }
  if (p & CCORD_RICH_HAS_EMBEDS) {
    view.embeds = upd->embeds;
    view.embed_count = upd->embed_count;
  }
  if (p & CCORD_RICH_HAS_STICKERS) {
    view.stickers = upd->stickers;
    view.sticker_count = upd->sticker_count;
  }
  if (p & CCORD_RICH_HAS_REFERENCE) {
    view.reference = upd->reference;
    view.ref_preview = upd->ref_preview;
  }
  if (p & CCORD_RICH_HAS_FORWARD) view.forward = upd->forward;
  if (p & CCORD_RICH_HAS_POLL) view.poll = upd->poll;
  if (p & CCORD_RICH_HAS_THREAD) view.thread = upd->thread;
  if (p & CCORD_RICH_HAS_COMPONENTS) {
    if (upd->application_id != CCORD_ID_NONE) view.application_id = upd->application_id;
    view.components = upd->components;
    view.component_row_count = upd->component_row_count;
    view.v2_components = upd->v2_components;
    view.v2_component_count = upd->v2_component_count;
  }
  view.present = old->present | upd->present;
  view.text_absent = false;
  ccord_message_rich *merged = ccord_message_rich_clone(&view, err);
  return merged;
}

void ccord_message_rich_free(ccord_message_rich *r)
{
  ccord_free(r);
}
