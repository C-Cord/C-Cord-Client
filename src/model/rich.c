// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/utf8.h>
#include <ccord/model/rich.h>

// One allocation owns a block and everything it points at.
typedef struct rich_owner {
  ccord_arena arena;
  ccord_message_rich rich;
} rich_owner;

typedef struct cloner {
  ccord_arena *a;
  bool ok;
} cloner;

static const char *dup_str(cloner *c, const char *s)
{
  if (!s || s[0] == '\0') return "";
  char *d = ccord_arena_strndup(c->a, s, strlen(s));
  if (!d) {
    c->ok = false;
    return "";
  }
  return d;
}

static void *dup_array(cloner *c, const void *src, size_t n, size_t size)
{
  if (n == 0 || !src) return nullptr;
  void *d = ccord_arena_alloc(c->a, n * size, _Alignof(max_align_t));
  if (!d) {
    c->ok = false;
    return nullptr;
  }
  memcpy(d, src, n * size);
  return d;
}

static void dup_media(cloner *c, ccord_embed_media *m)
{
  m->url = dup_str(c, m->url);
  m->proxy_url = dup_str(c, m->proxy_url);
}

static ccord_attachment *dup_attachments(cloner *c, const ccord_attachment *src, size_t n)
{
  ccord_attachment *d = dup_array(c, src, n, sizeof *d);
  for (size_t i = 0; d && i < n; i++) {
    d[i].filename = dup_str(c, d[i].filename);
    d[i].description = dup_str(c, d[i].description);
    d[i].content_type = dup_str(c, d[i].content_type);
    d[i].url = dup_str(c, d[i].url);
    d[i].proxy_url = dup_str(c, d[i].proxy_url);
    d[i].waveform_b64 = dup_str(c, d[i].waveform_b64);
  }
  return d;
}

static ccord_embed *dup_embeds(cloner *c, const ccord_embed *src, size_t n)
{
  ccord_embed *d = dup_array(c, src, n, sizeof *d);
  for (size_t i = 0; d && i < n; i++) {
    ccord_embed *e = &d[i];
    e->title = dup_str(c, e->title);
    e->description = dup_str(c, e->description);
    e->url = dup_str(c, e->url);
    e->provider = dup_str(c, e->provider);
    e->author_name = dup_str(c, e->author_name);
    e->author_url = dup_str(c, e->author_url);
    e->author_icon = dup_str(c, e->author_icon);
    e->footer_text = dup_str(c, e->footer_text);
    e->footer_icon = dup_str(c, e->footer_icon);
    dup_media(c, &e->image);
    dup_media(c, &e->thumbnail);
    dup_media(c, &e->video);
    ccord_embed_field *f = dup_array(c, e->fields, e->field_count, sizeof *f);
    for (size_t k = 0; f && k < e->field_count; k++) {
      f[k].name = dup_str(c, f[k].name);
      f[k].value = dup_str(c, f[k].value);
    }
    e->fields = f;
    if (!f) e->field_count = 0;
  }
  return d;
}

static ccord_msg_sticker *dup_stickers(cloner *c, const ccord_msg_sticker *src, size_t n)
{
  ccord_msg_sticker *d = dup_array(c, src, n, sizeof *d);
  for (size_t i = 0; d && i < n; i++) d[i].name = dup_str(c, d[i].name);
  return d;
}

static void dup_media_ref(cloner *c, ccord_media_ref *m)
{
  m->url = dup_str(c, m->url);
  m->proxy_url = dup_str(c, m->proxy_url);
  m->content_type = dup_str(c, m->content_type);
  m->description = dup_str(c, m->description);
}

// A clone never follows a tree deeper than this (validate rejects anything past CCORD_RICH_MAX_V2_DEPTH); it only bounds
// the recursion for a producer that did not validate.
#define CLONE_MAX_DEPTH 8u

static ccord_component *dup_nodes(cloner *c, const ccord_component *src, size_t n, unsigned depth);

// Makes the strings and arrays of `x`, a node whose fields were already copied by value, its own.
static void dup_node_fields(cloner *c, ccord_component *x, unsigned depth)
{
  x->label = dup_str(c, x->label);
  x->custom_id = dup_str(c, x->custom_id);
  x->url = dup_str(c, x->url);
  x->placeholder = dup_str(c, x->placeholder);
  x->emoji = dup_str(c, x->emoji);
  x->content = dup_str(c, x->content);
  x->file_name = dup_str(c, x->file_name);
  dup_media_ref(c, &x->media);
  ccord_select_option *opts = dup_array(c, x->options, x->option_count, sizeof *opts);
  for (size_t j = 0; opts && j < x->option_count; j++) {
    opts[j].label = dup_str(c, opts[j].label);
    opts[j].value = dup_str(c, opts[j].value);
    opts[j].description = dup_str(c, opts[j].description);
    opts[j].emoji = dup_str(c, opts[j].emoji);
  }
  x->options = opts;
  if (!opts) x->option_count = 0;
  ccord_media_ref *gal = dup_array(c, x->gallery, x->gallery_count, sizeof *gal);
  for (size_t j = 0; gal && j < x->gallery_count; j++) dup_media_ref(c, &gal[j]);
  x->gallery = gal;
  if (!gal) x->gallery_count = 0;
  if (depth < CLONE_MAX_DEPTH) {
    x->children = dup_nodes(c, x->children, x->child_count, depth + 1u);
    x->accessory = dup_nodes(c, x->accessory, x->accessory ? 1u : 0u, depth + 1u);
  } else {
    x->children = nullptr;
    x->accessory = nullptr;
  }
  if (!x->children) x->child_count = 0;
}

// A deep copy of `n` nodes whose descendants sit at `depth` and below (a top-level node is depth 1).
static ccord_component *dup_nodes(cloner *c, const ccord_component *src, size_t n, unsigned depth)
{
  ccord_component *d = dup_array(c, src, n, sizeof *d);
  for (size_t i = 0; d && i < n; i++) dup_node_fields(c, &d[i], depth);
  return d;
}

static ccord_component_row *dup_components(cloner *c, const ccord_component_row *src, size_t n)
{
  ccord_component_row *rows = dup_array(c, src, n, sizeof *rows);
  for (size_t i = 0; rows && i < n; i++) {
    rows[i].components = dup_nodes(c, rows[i].components, rows[i].component_count, 1u);
    if (!rows[i].components) rows[i].component_count = 0;
  }
  return rows;
}

// Copies `src` into the cloner's arena. `out` is overwritten.
static void clone_into(cloner *c, const ccord_message_rich *src, ccord_message_rich *out)
{
  *out = *src;
  out->mention_users = dup_array(c, src->mention_users, src->mention_user_count, sizeof(ccord_id));
  if (!out->mention_users) out->mention_user_count = 0;
  out->mention_roles = dup_array(c, src->mention_roles, src->mention_role_count, sizeof(ccord_id));
  if (!out->mention_roles) out->mention_role_count = 0;
  out->attachments = dup_attachments(c, src->attachments, src->attachment_count);
  if (!out->attachments) out->attachment_count = 0;
  out->embeds = dup_embeds(c, src->embeds, src->embed_count);
  if (!out->embeds) out->embed_count = 0;
  out->stickers = dup_stickers(c, src->stickers, src->sticker_count);
  if (!out->stickers) out->sticker_count = 0;
  out->ref_preview.author_name = dup_str(c, src->ref_preview.author_name);
  out->ref_preview.content = dup_str(c, src->ref_preview.content);
  out->forward.content = dup_str(c, src->forward.content);
  out->forward.attachments = dup_attachments(c, src->forward.attachments, src->forward.attachment_count);
  if (!out->forward.attachments) out->forward.attachment_count = 0;
  out->forward.embeds = dup_embeds(c, src->forward.embeds, src->forward.embed_count);
  if (!out->forward.embeds) out->forward.embed_count = 0;
  out->forward.stickers = dup_stickers(c, src->forward.stickers, src->forward.sticker_count);
  if (!out->forward.stickers) out->forward.sticker_count = 0;
  out->poll.question = dup_str(c, src->poll.question);
  for (size_t i = 0; i < CCORD_RICH_MAX_POLL_ANSWERS; i++) {
    out->poll.answers[i].text = dup_str(c, src->poll.answers[i].text);
    out->poll.answers[i].emoji = dup_str(c, src->poll.answers[i].emoji);
  }
  out->thread.name = dup_str(c, src->thread.name);
  out->components = dup_components(c, src->components, src->component_row_count);
  if (!out->components) out->component_row_count = 0;
  out->v2_components = dup_nodes(c, src->v2_components, src->v2_component_count, 1u);
  if (!out->v2_components) out->v2_component_count = 0;
}

ccord_message_rich *ccord_message_rich_clone(const ccord_message_rich *src, ccord_error *err)
{
  ccord_error_clear(err);
  if (!src) return nullptr;
  rich_owner *o = ccord_calloc(1, sizeof *o);
  if (!o) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "rich: out of memory");
    return nullptr;
  }
  ccord_arena_init(&o->arena, 2048u);
  cloner c = {.a = &o->arena, .ok = true};
  clone_into(&c, src, &o->rich);
  if (!c.ok) {
    ccord_arena_free(&o->arena);
    ccord_free(o);
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "rich: out of memory");
    return nullptr;
  }
  return &o->rich;
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
  if (!r) return;
  rich_owner *o = (rich_owner *)(void *)((char *)r - offsetof(rich_owner, rich));
  ccord_arena_free(&o->arena);
  ccord_free(o);
}

// ---------------------------------------------------------------- validation

// A missing text is an empty one; clone turns it into "" so readers never meet null.
static bool text_ok(const char *s, size_t max)
{
  if (!s) return true;
  size_t n = strlen(s);
  return n <= max && ccord_utf8_valid_text(s, n);
}

static ccord_status vbad(ccord_error *err, const char *what)
{
  return ccord_error_set(err, CCORD_ERR_INVALID, "message: %s", what);
}

static ccord_status check_media(const ccord_embed_media *m, ccord_error *err)
{
  if (!text_ok(m->url, CCORD_RICH_URL_MAX_BYTES) || !text_ok(m->proxy_url, CCORD_RICH_URL_MAX_BYTES)) {
    return vbad(err, "embed media url");
  }
  return CCORD_OK;
}

static ccord_status check_attachments(const ccord_attachment *a, size_t n, ccord_error *err)
{
  if (n > CCORD_RICH_MAX_ATTACHMENTS) return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many attachments");
  if (n > 0 && !a) return vbad(err, "null attachment array");
  for (size_t i = 0; i < n; i++) {
    if (!text_ok(a[i].filename, CCORD_RICH_SHORT_MAX_BYTES) || !text_ok(a[i].description, CCORD_RICH_TEXT_MAX_BYTES) ||
        !text_ok(a[i].content_type, CCORD_RICH_SHORT_MAX_BYTES) || !text_ok(a[i].url, CCORD_RICH_URL_MAX_BYTES) ||
        !text_ok(a[i].proxy_url, CCORD_RICH_URL_MAX_BYTES) ||
        !text_ok(a[i].waveform_b64, CCORD_RICH_WAVEFORM_MAX_BYTES)) {
      return vbad(err, "attachment text");
    }
  }
  return CCORD_OK;
}

static ccord_status check_embeds(const ccord_embed *e, size_t n, ccord_error *err)
{
  if (n > CCORD_RICH_MAX_EMBEDS) return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many embeds");
  if (n > 0 && !e) return vbad(err, "null embed array");
  for (size_t i = 0; i < n; i++) {
    const ccord_embed *x = &e[i];
    if (x->kind > CCORD_EMBED_OTHER) return vbad(err, "embed kind");
    if (x->color > 0xFFFFFFu) return vbad(err, "embed colour");
    if (x->timestamp_ms < 0) return vbad(err, "embed timestamp");
    if (!text_ok(x->title, CCORD_RICH_SHORT_MAX_BYTES * 2u) || !text_ok(x->description, CCORD_RICH_TEXT_MAX_BYTES) ||
        !text_ok(x->url, CCORD_RICH_URL_MAX_BYTES) || !text_ok(x->provider, CCORD_RICH_SHORT_MAX_BYTES) ||
        !text_ok(x->author_name, CCORD_RICH_SHORT_MAX_BYTES) || !text_ok(x->author_url, CCORD_RICH_URL_MAX_BYTES) ||
        !text_ok(x->author_icon, CCORD_RICH_URL_MAX_BYTES) || !text_ok(x->footer_text, CCORD_RICH_TEXT_MAX_BYTES / 2u) ||
        !text_ok(x->footer_icon, CCORD_RICH_URL_MAX_BYTES)) {
      return vbad(err, "embed text");
    }
    ccord_status st = check_media(&x->image, err);
    if (st == CCORD_OK) st = check_media(&x->thumbnail, err);
    if (st == CCORD_OK) st = check_media(&x->video, err);
    if (st != CCORD_OK) return st;
    if (x->field_count > CCORD_RICH_MAX_EMBED_FIELDS) return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many embed fields");
    if (x->field_count > 0 && !x->fields) return vbad(err, "null embed field array");
    for (size_t k = 0; k < x->field_count; k++) {
      if (!text_ok(x->fields[k].name, CCORD_RICH_SHORT_MAX_BYTES) ||
          !text_ok(x->fields[k].value, CCORD_RICH_FIELD_VALUE_MAX_BYTES)) {
        return vbad(err, "embed field text");
      }
    }
  }
  return CCORD_OK;
}

static ccord_status check_stickers(const ccord_msg_sticker *s, size_t n, ccord_error *err)
{
  if (n > CCORD_RICH_MAX_STICKERS) return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many stickers");
  if (n > 0 && !s) return vbad(err, "null sticker array");
  for (size_t i = 0; i < n; i++) {
    if (s[i].format > 4u) return vbad(err, "sticker format");
    if (!text_ok(s[i].name, CCORD_RICH_SHORT_MAX_BYTES)) return vbad(err, "sticker name");
  }
  return CCORD_OK;
}

static bool kind_is_select(ccord_component_kind k)
{
  return k == CCORD_COMP_STRING_SELECT || k == CCORD_COMP_USER_SELECT || k == CCORD_COMP_ROLE_SELECT ||
         k == CCORD_COMP_MENTIONABLE_SELECT || k == CCORD_COMP_CHANNEL_SELECT;
}

// A button or select menu: the part of a component v1 and v2 share.
static ccord_status check_interactive(const ccord_component *c, ccord_error *err)
{
  if (c->min_values > CCORD_RICH_MAX_SELECT_OPTIONS || c->max_values > CCORD_RICH_MAX_SELECT_OPTIONS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "message: component select bounds");
  }
  if (!text_ok(c->label, CCORD_RICH_SHORT_MAX_BYTES) || !text_ok(c->custom_id, CCORD_RICH_CUSTOM_ID_MAX_BYTES) ||
      !text_ok(c->url, CCORD_RICH_URL_MAX_BYTES) || !text_ok(c->placeholder, CCORD_RICH_SHORT_MAX_BYTES) ||
      !text_ok(c->emoji, CCORD_EMOJI_MAX_BYTES)) {
    return vbad(err, "component text");
  }
  if (c->kind == CCORD_COMP_BUTTON) {
    if (c->style < CCORD_BTN_PRIMARY || c->style > CCORD_BTN_PREMIUM) return vbad(err, "button style");
    if (c->style == CCORD_BTN_LINK && (!c->url || c->url[0] == '\0')) return vbad(err, "link button url");
    if (c->style != CCORD_BTN_LINK && c->style != CCORD_BTN_PREMIUM && (!c->custom_id || c->custom_id[0] == '\0')) {
      return vbad(err, "button custom_id");
    }
  } else {
    if (!c->custom_id || c->custom_id[0] == '\0') return vbad(err, "select custom_id");
    uint8_t eff_max = c->max_values > 0 ? c->max_values : 1u;
    if (c->min_values > eff_max) return vbad(err, "select min/max values");
  }
  if (c->option_count > CCORD_RICH_MAX_SELECT_OPTIONS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many select options");
  }
  if (c->option_count > 0 && !c->options) return vbad(err, "null select option array");
  for (size_t j = 0; j < c->option_count; j++) {
    const ccord_select_option *o = &c->options[j];
    if (!o->value || o->value[0] == '\0') return vbad(err, "select option value");
    if (!text_ok(o->label, CCORD_RICH_SHORT_MAX_BYTES) || !text_ok(o->value, CCORD_RICH_SHORT_MAX_BYTES) ||
        !text_ok(o->description, CCORD_RICH_SHORT_MAX_BYTES) || !text_ok(o->emoji, CCORD_EMOJI_MAX_BYTES)) {
      return vbad(err, "select option text");
    }
  }
  return CCORD_OK;
}

static ccord_status check_components(const ccord_component_row *rows, size_t n, ccord_error *err)
{
  if (n > CCORD_RICH_MAX_COMPONENT_ROWS) return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many component rows");
  if (n > 0 && !rows) return vbad(err, "null component row array");
  for (size_t i = 0; i < n; i++) {
    const ccord_component_row *row = &rows[i];
    if (row->component_count > CCORD_RICH_MAX_ROW_COMPONENTS) {
      return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many row components");
    }
    if (row->component_count > 0 && !row->components) return vbad(err, "null component array");
    for (size_t k = 0; k < row->component_count; k++) {
      const ccord_component *c = &row->components[k];
      if (c->kind < CCORD_COMP_BUTTON || c->kind > CCORD_COMP_CHANNEL_SELECT || c->kind == CCORD_COMP_TEXT_INPUT) {
        return vbad(err, "component kind");
      }
      ccord_status st = check_interactive(c, err);
      if (st != CCORD_OK) return st;
    }
  }
  return CCORD_OK;
}

// Where a v2 node sits, which decides the kinds it may be.
typedef enum v2_parent { V2_TOP, V2_CONTAINER, V2_SECTION_TEXT, V2_SECTION_ACCESSORY, V2_ROW } v2_parent;

static bool v2_kind_allowed(v2_parent where, ccord_component_kind k)
{
  switch (where) {
    case V2_TOP:
    case V2_CONTAINER:
      return k == CCORD_COMP_ACTION_ROW || k == CCORD_COMP_SECTION || k == CCORD_COMP_TEXT_DISPLAY ||
             k == CCORD_COMP_MEDIA_GALLERY || k == CCORD_COMP_FILE || k == CCORD_COMP_SEPARATOR ||
             (where == V2_TOP && k == CCORD_COMP_CONTAINER);
    case V2_SECTION_TEXT: return k == CCORD_COMP_TEXT_DISPLAY;
    case V2_SECTION_ACCESSORY: return k == CCORD_COMP_THUMBNAIL || k == CCORD_COMP_BUTTON;
    case V2_ROW: return k == CCORD_COMP_BUTTON || kind_is_select(k);
  }
  return false;
}

static ccord_status check_media_ref(const ccord_media_ref *m, bool need_url, ccord_error *err)
{
  if (!text_ok(m->url, CCORD_RICH_URL_MAX_BYTES) || !text_ok(m->proxy_url, CCORD_RICH_URL_MAX_BYTES) ||
      !text_ok(m->content_type, CCORD_RICH_SHORT_MAX_BYTES) || !text_ok(m->description, CCORD_RICH_TEXT_MAX_BYTES)) {
    return vbad(err, "component media text");
  }
  if (need_url && (!m->url || m->url[0] == '\0') && (!m->proxy_url || m->proxy_url[0] == '\0')) {
    return vbad(err, "component media url");
  }
  return CCORD_OK;
}

// The size budget of a v2 tree: at most CCORD_RICH_MAX_V2_NODES nodes in all and CCORD_RICH_MAX_V2_DEPTH levels. It runs
// before the field checks, so the verdict on an oversized tree is the same whatever the nodes are, and it bounds the walk
// of the checks after it. `*count` is the running node total of the whole message.
static ccord_status check_v2_budget(const ccord_component *c, unsigned depth, size_t *count, ccord_error *err)
{
  if (depth > CCORD_RICH_MAX_V2_DEPTH) return ccord_error_set(err, CCORD_ERR_RANGE, "message: components nested too deep");
  if (++*count > CCORD_RICH_MAX_V2_NODES) return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many components");
  for (size_t i = 0; c->children && i < c->child_count; i++) {
    ccord_status st = check_v2_budget(&c->children[i], depth + 1u, count, err);
    if (st != CCORD_OK) return st;
  }
  return c->accessory ? check_v2_budget(c->accessory, depth + 1u, count, err) : CCORD_OK;
}

static ccord_status check_v2_children(const ccord_component *kids, size_t n, v2_parent where, ccord_error *err);

// One node of a v2 tree and everything under it (the budget has been checked).
static ccord_status check_v2_node(const ccord_component *c, v2_parent where, ccord_error *err)
{
  if (!v2_kind_allowed(where, c->kind)) return vbad(err, "component kind");
  ccord_status st = CCORD_OK;
  switch (c->kind) {
    case CCORD_COMP_BUTTON:
    case CCORD_COMP_STRING_SELECT:
    case CCORD_COMP_USER_SELECT:
    case CCORD_COMP_ROLE_SELECT:
    case CCORD_COMP_MENTIONABLE_SELECT:
    case CCORD_COMP_CHANNEL_SELECT:
      return check_interactive(c, err);
    case CCORD_COMP_ACTION_ROW:
      if (c->child_count > CCORD_RICH_MAX_ROW_COMPONENTS) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many row components");
      }
      return check_v2_children(c->children, c->child_count, V2_ROW, err);
    case CCORD_COMP_SECTION:
      if (c->child_count == 0 || c->child_count > CCORD_RICH_MAX_V2_SECTION_TEXTS) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "message: section text count");
      }
      if (!c->accessory) return vbad(err, "section accessory");
      st = check_v2_children(c->children, c->child_count, V2_SECTION_TEXT, err);
      return st != CCORD_OK ? st : check_v2_node(c->accessory, V2_SECTION_ACCESSORY, err);
    case CCORD_COMP_TEXT_DISPLAY:
      return text_ok(c->content, CCORD_RICH_TEXT_MAX_BYTES) ? CCORD_OK : vbad(err, "text display content");
    case CCORD_COMP_THUMBNAIL:
      return check_media_ref(&c->media, true, err);
    case CCORD_COMP_MEDIA_GALLERY:
      if (c->gallery_count == 0 || c->gallery_count > CCORD_RICH_MAX_V2_GALLERY_ITEMS) {
        return ccord_error_set(err, CCORD_ERR_RANGE, "message: gallery item count");
      }
      if (!c->gallery) return vbad(err, "null gallery array");
      for (size_t i = 0; i < c->gallery_count && st == CCORD_OK; i++) st = check_media_ref(&c->gallery[i], true, err);
      return st;
    case CCORD_COMP_FILE:
      if (!text_ok(c->file_name, CCORD_RICH_SHORT_MAX_BYTES)) return vbad(err, "file name");
      return check_media_ref(&c->media, true, err);
    case CCORD_COMP_SEPARATOR:
      return c->spacing <= 2u ? CCORD_OK : vbad(err, "separator spacing");
    case CCORD_COMP_CONTAINER:
      if (c->accent_color > 0xFFFFFFu) return vbad(err, "container colour");
      return check_v2_children(c->children, c->child_count, V2_CONTAINER, err);
    default: return vbad(err, "component kind");
  }
}

static ccord_status check_v2_children(const ccord_component *kids, size_t n, v2_parent where, ccord_error *err)
{
  if (n > 0 && !kids) return vbad(err, "null component array");
  for (size_t i = 0; i < n; i++) {
    ccord_status st = check_v2_node(&kids[i], where, err);
    if (st != CCORD_OK) return st;
  }
  return CCORD_OK;
}

static ccord_status check_v2_components(const ccord_component *nodes, size_t n, ccord_error *err)
{
  if (n > CCORD_RICH_MAX_V2_NODES) return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many components");
  if (n > 0 && !nodes) return vbad(err, "null component array");
  size_t count = 0;
  for (size_t i = 0; i < n; i++) {
    ccord_status st = check_v2_budget(&nodes[i], 1u, &count, err);
    if (st != CCORD_OK) return st;
  }
  return check_v2_children(nodes, n, V2_TOP, err);
}

ccord_status ccord_message_rich_validate(const ccord_message_rich *r, ccord_error *err)
{
  ccord_error_clear(err);
  if (!r) return CCORD_OK;
  if (r->mention_user_count > CCORD_RICH_MAX_MENTIONS || r->mention_role_count > CCORD_RICH_MAX_MENTIONS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many mentions");
  }
  if ((r->mention_user_count > 0 && !r->mention_users) || (r->mention_role_count > 0 && !r->mention_roles)) {
    return vbad(err, "null mention array");
  }
  ccord_status st = check_attachments(r->attachments, r->attachment_count, err);
  if (st == CCORD_OK) st = check_embeds(r->embeds, r->embed_count, err);
  if (st == CCORD_OK) st = check_stickers(r->stickers, r->sticker_count, err);
  if (st != CCORD_OK) return st;
  if (r->reference.kind > CCORD_REF_FORWARD) return vbad(err, "reference kind");
  if (!text_ok(r->ref_preview.author_name, CCORD_RICH_SHORT_MAX_BYTES) ||
      !text_ok(r->ref_preview.content, CCORD_RICH_SNIPPET_MAX_BYTES * 2u)) {
    return vbad(err, "reply preview text");
  }
  const ccord_forward *f = &r->forward;
  if (!text_ok(f->content, CCORD_RICH_TEXT_MAX_BYTES) || f->timestamp_ms < 0) return vbad(err, "forward");
  st = check_attachments(f->attachments, f->attachment_count, err);
  if (st == CCORD_OK) st = check_embeds(f->embeds, f->embed_count, err);
  if (st == CCORD_OK) st = check_stickers(f->stickers, f->sticker_count, err);
  if (st != CCORD_OK) return st;
  const ccord_poll *p = &r->poll;
  if (p->answer_count > CCORD_RICH_MAX_POLL_ANSWERS) return ccord_error_set(err, CCORD_ERR_RANGE, "message: too many poll answers");
  if (p->expiry_ms < 0) return vbad(err, "poll expiry");
  if (!text_ok(p->question, CCORD_RICH_SHORT_MAX_BYTES * 2u)) return vbad(err, "poll question");
  for (size_t i = 0; i < p->answer_count; i++) {
    if (!text_ok(p->answers[i].text, CCORD_RICH_SHORT_MAX_BYTES) || !text_ok(p->answers[i].emoji, CCORD_EMOJI_MAX_BYTES)) {
      return vbad(err, "poll answer text");
    }
  }
  if (!text_ok(r->thread.name, CCORD_RICH_SHORT_MAX_BYTES)) return vbad(err, "thread name");
  st = check_components(r->components, r->component_row_count, err);
  if (st != CCORD_OK) return st;
  return check_v2_components(r->v2_components, r->v2_component_count, err);
}

// ---------------------------------------------------------------- helpers

uint16_t ccord_message_type(const ccord_message *m)
{
  return (m && m->rich) ? m->rich->type : 0u;
}

uint32_t ccord_message_flags(const ccord_message *m)
{
  return (m && m->rich) ? m->rich->flags : 0u;
}

bool ccord_message_is_ephemeral(const ccord_message *m)
{
  return (ccord_message_flags(m) & CCORD_MSGFLAG_EPHEMERAL) != 0u;
}

static bool slash_ident_ok(const char *s)
{
  if (!s || s[0] == '\0') return false;
  size_t n = strlen(s);
  if (n > CCORD_SLASH_NAME_MAX_BYTES || !ccord_utf8_valid_text(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') return false;
  }
  return true;
}

ccord_status ccord_slash_command_validate(const ccord_slash_command *cmd, ccord_error *err)
{
  ccord_error_clear(err);
  if (!cmd) return vbad(err, "null slash command");
  if (cmd->id == CCORD_ID_NONE || cmd->application_id == CCORD_ID_NONE) {
    return vbad(err, "slash command id");
  }
  if (cmd->command_type > 3) return vbad(err, "slash command type");
  if (cmd->command_type >= 2) {
    // USER / MESSAGE context-menu commands: names may contain spaces, and they carry no options.
    if (cmd->name[0] == '\0' || !text_ok(cmd->name, CCORD_SLASH_NAME_MAX_BYTES)) {
      return vbad(err, "slash command name");
    }
    if (cmd->option_count > 0) return vbad(err, "slash context command options");
  } else if (!slash_ident_ok(cmd->name)) {
    return vbad(err, "slash command name");
  }
  if (!text_ok(cmd->description, CCORD_SLASH_DESC_MAX_BYTES)) return vbad(err, "slash command description");
  if (cmd->option_count > CCORD_SLASH_MAX_OPTIONS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "slash: too many options");
  }
  bool root_has_sub = false, root_has_leaf = false;
  for (size_t i = 0; i < cmd->option_count; i++) {
    const ccord_slash_option *o = &cmd->options[i];
    if (o->kind < CCORD_SLASH_OPT_SUB_COMMAND || o->kind > CCORD_SLASH_OPT_NUMBER) {
      return vbad(err, "slash option kind");
    }
    if (!slash_ident_ok(o->name)) return vbad(err, "slash option name");
    if (!text_ok(o->description, CCORD_SLASH_DESC_MAX_BYTES)) return vbad(err, "slash option description");
    if (o->autocomplete && o->kind != CCORD_SLASH_OPT_STRING &&
        o->kind != CCORD_SLASH_OPT_INTEGER && o->kind != CCORD_SLASH_OPT_NUMBER) {
      return vbad(err, "slash option autocomplete type");
    }
    bool is_sub = o->kind == CCORD_SLASH_OPT_SUB_COMMAND || o->kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP;
    if (o->parent_plus_one == 0) {
      if (is_sub) root_has_sub = true; else root_has_leaf = true;
      continue;
    }
    // k = child of options[k-1]: the parent must exist, come first, and be a group or subcommand.
    if (o->parent_plus_one > cmd->option_count || o->parent_plus_one > i) {
      return vbad(err, "slash option parent");
    }
    const ccord_slash_option *parent = &cmd->options[o->parent_plus_one - 1u];
    if (parent->kind == CCORD_SLASH_OPT_SUB_COMMAND_GROUP) {
      if (o->kind != CCORD_SLASH_OPT_SUB_COMMAND) return vbad(err, "slash option group child");
    } else if (parent->kind == CCORD_SLASH_OPT_SUB_COMMAND) {
      if (is_sub) return vbad(err, "slash option subcommand child");
    } else {
      return vbad(err, "slash option parent kind");
    }
  }
  // Discord: a command's root options are either all subcommands/groups or all leaf options, never mixed.
  if (root_has_sub && root_has_leaf) return vbad(err, "slash option mix");
  return CCORD_OK;
}

static bool has_id(const ccord_id *ids, size_t n, ccord_id id)
{
  for (size_t i = 0; ids && i < n; i++) {
    if (ids[i] == id) return true;
  }
  return false;
}

bool ccord_rich_mentions_user(const ccord_message_rich *r, ccord_id user_id)
{
  return r && user_id != CCORD_ID_NONE && has_id(r->mention_users, r->mention_user_count, user_id);
}

bool ccord_rich_mentions_role(const ccord_message_rich *r, ccord_id role_id)
{
  return r && role_id != CCORD_ID_NONE && has_id(r->mention_roles, r->mention_role_count, role_id);
}

bool ccord_message_mentions_user(const ccord_message *m, ccord_id user_id)
{
  if (!m || user_id == CCORD_ID_NONE) return false;
  if (m->rich) return ccord_rich_mentions_user(m->rich, user_id);
  if (!m->content || m->content[0] == '\0') return false;
  char idbuf[21];
  ccord_id_format(user_id, idbuf);
  size_t n = strlen(idbuf);
  char plain[24];   // "<@" + 20 digits + ">"
  char nick[25];    // "<@!" + 20 digits + ">"
  memcpy(plain, "<@", 2);
  memcpy(plain + 2, idbuf, n);
  plain[n + 2] = '>';
  plain[n + 3] = '\0';
  memcpy(nick, "<@!", 3);
  memcpy(nick + 3, idbuf, n);
  nick[n + 3] = '>';
  nick[n + 4] = '\0';
  return strstr(m->content, plain) != nullptr || strstr(m->content, nick) != nullptr;
}

static bool ext_is(const char *name, const char *const *exts)
{
  if (!name) return false;
  const char *dot = strrchr(name, '.');
  if (!dot || dot[1] == '\0') return false;
  for (size_t i = 0; exts[i]; i++) {
    const char *a = dot + 1, *b = exts[i];
    while (*a && *b && ((*a | 0x20) == *b)) {
      a++;
      b++;
    }
    if (*a == '\0' && *b == '\0') return true;
  }
  return false;
}

static bool type_is(const char *mime, const char *prefix)
{
  return mime && strncmp(mime, prefix, strlen(prefix)) == 0;
}

bool ccord_attachment_is_image(const ccord_attachment *a)
{
  static const char *const exts[] = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "avif", nullptr};
  if (!a) return false;
  if (a->content_type && a->content_type[0] != '\0') return type_is(a->content_type, "image/");
  return ext_is(a->filename, exts);
}

bool ccord_attachment_is_video(const ccord_attachment *a)
{
  static const char *const exts[] = {"mp4", "webm", "mov", "mkv", "m4v", nullptr};
  if (!a) return false;
  if (a->content_type && a->content_type[0] != '\0') return type_is(a->content_type, "video/");
  return ext_is(a->filename, exts);
}

bool ccord_attachment_is_audio(const ccord_attachment *a)
{
  static const char *const exts[] = {"mp3", "ogg", "oga", "wav", "flac", "m4a", "opus", nullptr};
  if (!a) return false;
  if (a->content_type && a->content_type[0] != '\0') return type_is(a->content_type, "audio/");
  return ext_is(a->filename, exts);
}

// ---------------------------------------------------------------- components v2 helpers

// Walks stop here; a validated tree never gets this deep.
#define WALK_MAX_DEPTH 8u

static const ccord_component *find_in_nodes(const ccord_component *nodes, size_t n, const char *custom_id, unsigned depth)
{
  for (size_t i = 0; nodes && i < n; i++) {
    const ccord_component *c = &nodes[i];
    if (c->custom_id && strcmp(c->custom_id, custom_id) == 0) return c;
    if (depth >= WALK_MAX_DEPTH) continue;
    const ccord_component *hit = find_in_nodes(c->children, c->child_count, custom_id, depth + 1u);
    if (!hit && c->accessory) hit = find_in_nodes(c->accessory, 1u, custom_id, depth + 1u);
    if (hit) return hit;
  }
  return nullptr;
}

const ccord_component *ccord_rich_find_component(const ccord_message_rich *rich, const char *custom_id)
{
  if (!rich || !custom_id || custom_id[0] == '\0') return nullptr;
  const ccord_component *hit = find_in_nodes(rich->v2_components, rich->v2_component_count, custom_id, 1u);
  for (size_t i = 0; !hit && rich->components && i < rich->component_row_count; i++) {
    hit = find_in_nodes(rich->components[i].components, rich->components[i].component_count, custom_id, 1u);
  }
  return hit;
}

const char *ccord_rich_media_url(const ccord_message_rich *rich, const ccord_media_ref *ref)
{
  if (!ref) return "";
  const char *url = ref->url ? ref->url : "";
  const char *proxy = ref->proxy_url ? ref->proxy_url : "";
  static const char scheme[] = "attachment://";
  if (strncmp(url, scheme, sizeof scheme - 1u) != 0) return url[0] != '\0' ? url : proxy;
  const char *name = url + sizeof scheme - 1u;
  for (size_t i = 0; rich && rich->attachments && i < rich->attachment_count; i++) {
    const ccord_attachment *a = &rich->attachments[i];
    if (!a->filename || strcmp(a->filename, name) != 0) continue;
    if (a->url && a->url[0] != '\0') return a->url;
    if (a->proxy_url && a->proxy_url[0] != '\0') return a->proxy_url;
    break;
  }
  return proxy;
}

// The first node of `kind` in a v2 tree (depth first, a section's accessory after its texts), or nullptr.
static const ccord_component *find_kind(const ccord_component *nodes, size_t n, ccord_component_kind kind, unsigned depth)
{
  for (size_t i = 0; nodes && i < n; i++) {
    const ccord_component *c = &nodes[i];
    if (c->kind == kind && (kind != CCORD_COMP_TEXT_DISPLAY || (c->content && c->content[0] != '\0'))) return c;
    if (depth >= WALK_MAX_DEPTH) continue;
    const ccord_component *hit = find_kind(c->children, c->child_count, kind, depth + 1u);
    if (!hit && c->accessory) hit = find_kind(c->accessory, 1u, kind, depth + 1u);
    if (hit) return hit;
  }
  return nullptr;
}

// ---------------------------------------------------------------- previews and system messages

bool ccord_message_type_is_system(uint16_t type)
{
  switch (type) {
    case 0:    // default
    case 19:   // reply
    case 20:   // chat input command
    case 21:   // thread starter message (shows the message the thread began from)
    case 23:   // context menu command
      return false;
    default: return true;
  }
}

const char *ccord_message_type_phrase(uint16_t type)
{
  switch (type) {
    case 1: return "added someone to the group.";
    case 2: return "removed someone from the group.";
    case 3: return "started a call.";
    case 4: return "changed the channel name:";
    case 5: return "changed the group icon.";
    case 6: return "pinned a message to this channel.";
    case 7: return "joined the server.";
    case 8: return "boosted the server!";
    case 9: return "boosted the server, which reached Level 1!";
    case 10: return "boosted the server, which reached Level 2!";
    case 11: return "boosted the server, which reached Level 3!";
    case 12: return "added a channel to follow.";
    case 18: return "started a thread:";
    case 22: return "Wondering who to invite? Share the server link!";
    default: return nullptr;
  }
}

// Appends whole characters of valid UTF-8 (never half of one) while they fit; newlines and tabs become spaces.
static void put(char *out, size_t cap, size_t *n, const char *s, size_t len)
{
  size_t i = 0;
  while (i < len) {
    unsigned char c = (unsigned char)s[i];
    size_t seq = c < 0x80u ? 1u : (c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC0u ? 2u : 1u)));
    if (seq > len - i || *n + seq + 1u > cap) break;
    if (seq == 1u) {
      out[(*n)++] = (c == '\n' || c == '\t') ? ' ' : (char)c;
    } else {
      memcpy(out + *n, s + i, seq);
      *n += seq;
    }
    i += seq;
  }
}

size_t ccord_message_preview(const ccord_message *m, char *out, size_t cap)
{
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!m) return 0;
  size_t n = 0;
  const char *content = m->content ? m->content : "";
  const ccord_message_rich *r = m->rich;
  char tmp[CCORD_RICH_SHORT_MAX_BYTES + 32u];
  const char *label = nullptr;
  if (content[0] != '\0') {
    put(out, cap, &n, content, strlen(content));
  } else if (!r) {
    label = "";
  } else if (r->v2_component_count > 0) {
    // A v2 message has no text of its own: say what its first piece of text, file or media is.
    const ccord_component *t = find_kind(r->v2_components, r->v2_component_count, CCORD_COMP_TEXT_DISPLAY, 1u);
    const ccord_component *f = find_kind(r->v2_components, r->v2_component_count, CCORD_COMP_FILE, 1u);
    if (t) {
      label = t->content;
    } else if (f) {
      snprintf(tmp, sizeof tmp, "Attachment: %s", f->file_name && f->file_name[0] ? f->file_name : "file");
      label = tmp;
    } else if (find_kind(r->v2_components, r->v2_component_count, CCORD_COMP_MEDIA_GALLERY, 1u) ||
               find_kind(r->v2_components, r->v2_component_count, CCORD_COMP_THUMBNAIL, 1u)) {
      label = "Media";
    } else {
      label = "Interactive message";
    }
  } else if (r->poll.present) {
    snprintf(tmp, sizeof tmp, "Poll: %s", r->poll.question ? r->poll.question : "");
    label = tmp;
  } else if (r->forward.present) {
    label = "Forwarded message";
  } else if (r->attachment_count > 0) {
    const ccord_attachment *a = &r->attachments[0];
    if (ccord_attachment_is_image(a)) label = "Image";
    else if (ccord_attachment_is_video(a)) label = "Video";
    else if (ccord_attachment_is_audio(a)) label = (r->flags & CCORD_MSGFLAG_VOICE_MESSAGE) ? "Voice message" : "Audio";
    else {
      snprintf(tmp, sizeof tmp, "Attachment: %s", a->filename && a->filename[0] ? a->filename : "file");
      label = tmp;
    }
  } else if (r->sticker_count > 0) {
    snprintf(tmp, sizeof tmp, "Sticker: %s", r->stickers[0].name ? r->stickers[0].name : "");
    label = tmp;
  } else if (r->embed_count > 0) {
    const ccord_embed *e = &r->embeds[0];
    if (e->title && e->title[0] != '\0') label = e->title;
    else if (e->description && e->description[0] != '\0') label = e->description;
    else if (e->url && e->url[0] != '\0') label = e->url;
    else label = "Embed";
  } else if (r->component_row_count > 0) {
    label = "Interactive message";
  } else if (ccord_message_type_is_system(r->type)) {
    label = "System message";
  } else {
    label = "";
  }
  if (label) put(out, cap, &n, label, strlen(label));
  out[n] = '\0';
  return n;
}
