// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Settings > Profile: the own-profile editor. See settings_profile.h for the contract.
//
// Layout follows Discord's Profiles page: editable fields on the left, a live preview card on the right (above the
// fields when the window is narrow), the unsaved-changes bar docked at the bottom of the page. Everything is sized
// from the shared style tokens (ui_style.h). The controls the last draw produced are recorded in S.items; clicks are
// hit-tested against them, so what is drawn is what is clickable.
#include "settings_profile.h"

#include <stdio.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/net/profile_edit.h>
#include <ccord/session/session.h>
#include <ccord/store/store.h>
#include <ccord/ui/text_edit.h>

#include "ui_internal.h"
#include "ui_style.h"

#define COL(name) (ui->theme->c.name)

enum { F_NAME = 0, F_PRONOUNS, F_BIO, F_HEX, F_TEXT_COUNT };
enum { IMG_KEEP = 0, IMG_REMOVE, IMG_NEW };
enum { IMG_AVATAR = 0, IMG_BANNER };

#define PV_W 300.0f            // preview card width
#define PV_BANNER_H 96.0f
#define PV_AVATAR_D 80.0f
#define BIO_BOX_H 112.0f
#define BAR_H 56.0f
#define SAVED_MS 1800u         // the success state shows this long
#define WHEEL_STEP 108.0f
#define ACCENT_PRESETS 8

static const uint32_t k_presets[ACCENT_PRESETS] = {0x1ABC9C, 0x2ECC71, 0x3498DB, 0x9B59B6,
                                                   0xE91E63, 0xF1C40F, 0xE67E22, 0xE74C3C};

typedef struct snap {
  char name[CCORD_PROFILE_DISPLAY_NAME_MAX * 4u + 8u];
  char pronouns[CCORD_PROFILE_PRONOUNS_MAX * 4u + 1u];
  char bio[CCORD_PROFILE_BIO_MAX * 4u + 1u];
  char avatar_url[256];
  char banner_url[256];
  int32_t accent;   // -1 none
} snap;

typedef struct img_draft {
  int mode;
  uint8_t *data;
  size_t len;
  unsigned seq;
  char key[48];
  bool uploaded;
} img_draft;

static struct state {
  bool live;
  const ccord_ui *owner;
  ccord_text_edit ed[F_TEXT_COUNT];
  snap base;                         // the saved profile the drafts are compared with
  int32_t accent;                    // draft accent, -1 none
  bool hex_bad;
  img_draft img[2];
  bool pending;
  uint32_t sent;                     // fields of the request in flight
  uint64_t saved_until_ms;
  char err[CCORD_PROFILE_FIELD_COUNT][200];
  char general[200];
  float scroll, content_h, view_h;
  ui_profile_item items[UI_PROFILE_ITEMS_MAX];
  size_t nitems;
  int awaiting;                      // image slot waiting for a picked/dropped file, -1 none
  bool click_pending;                // a click on the About Me box waits for the next draw to place the caret
  float click_x, click_y;
  bool cb_done;                      // Discord's answer, handed from the session callback to the tick
  ccord_profile_edit_result cb_res;
  ccord_rect bar, view;
  ccord_rect err_rect[CCORD_PROFILE_FIELD_COUNT];   // last draw
  bool bar_visible;
  char pv_name[130], pv_pronouns[170], pv_bio[800];
  int pv_accent;
} S;

static uintptr_t s_gen = 1;          // bumped by every release: an answer for an older page state is dropped
static unsigned s_img_seq;

// ---------------------------------------------------------------- state

static const ccord_user *self_user(const ccord_ui *ui)
{
  if (!ui->d.store) return nullptr;
  return ccord_store_user(ui->d.store, ccord_store_self_id(ui->d.store));
}

static void copy_str(char *dst, size_t cap, const char *src)
{
  snprintf(dst, cap, "%s", src ? src : "");
}

static void read_snap(const ccord_ui *ui, snap *out)
{
  *out = (snap){.accent = -1};
  const ccord_user *u = self_user(ui);
  if (!u) return;
  const ccord_user_profile *prof = ui->d.store ? ccord_store_user_profile(ui->d.store, u->id) : nullptr;
  copy_str(out->name, sizeof out->name, u->display_name);
  copy_str(out->pronouns, sizeof out->pronouns, u->pronouns);
  copy_str(out->bio, sizeof out->bio, u->bio);
  copy_str(out->avatar_url, sizeof out->avatar_url, u->avatar_url);
  const char *burl = (u->banner_url && u->banner_url[0]) ? u->banner_url
                     : ((prof && prof->banner_url && prof->banner_url[0]) ? prof->banner_url : "");
  copy_str(out->banner_url, sizeof out->banner_url, burl);
  if (prof && prof->has_theme) {
    // Official Discord ignores accent_color and banner_color when a Nitro two-colour theme is active.
  } else if (prof) {
    if (u->has_accent_color || u->accent_color != 0) {
      out->accent = (int32_t)(u->accent_color & 0xffffffu);
    } else if (prof->has_banner_color) {
      out->accent = (int32_t)(prof->banner_color & 0xffffffu);
    } else if (prof->has_accent_color || prof->accent_color != 0) {
      out->accent = (int32_t)(prof->accent_color & 0xffffffu);
    }
  }
}

static bool snap_eq(const snap *a, const snap *b)
{
  return a->accent == b->accent && strcmp(a->name, b->name) == 0 && strcmp(a->pronouns, b->pronouns) == 0 &&
         strcmp(a->bio, b->bio) == 0 && strcmp(a->avatar_url, b->avatar_url) == 0 &&
         strcmp(a->banner_url, b->banner_url) == 0;
}

static void ed_set(ccord_text_edit *t, const char *s)
{
  ccord_text_edit_clear(t);
  ccord_error e;
  if (s && s[0]) (void)ccord_text_edit_insert(t, s, strlen(s), &e);
}

static void set_hex_text(void)
{
  char hex[16] = "";
  if (S.accent >= 0) snprintf(hex, sizeof hex, "%06X", (unsigned)S.accent);
  ed_set(&S.ed[F_HEX], hex);
  S.hex_bad = false;
}

static void load_field(int f)
{
  switch (f) {
    case F_NAME: ed_set(&S.ed[F_NAME], S.base.name); break;
    case F_PRONOUNS: ed_set(&S.ed[F_PRONOUNS], S.base.pronouns); break;
    case F_BIO: ed_set(&S.ed[F_BIO], S.base.bio); break;
    case F_HEX:
      S.accent = S.base.accent;
      set_hex_text();
      break;
    default: break;
  }
}

static void img_free(img_draft *d)
{
  ccord_free(d->data);
  *d = (img_draft){0};
}

static void clear_errors(void)
{
  memset(S.err, 0, sizeof S.err);
  S.general[0] = '\0';
}

static void load_all(void)
{
  for (int f = 0; f < F_TEXT_COUNT; f++) load_field(f);
  img_free(&S.img[IMG_AVATAR]);
  img_free(&S.img[IMG_BANNER]);
}

static uint32_t changed_mask(void)
{
  uint32_t m = 0;
  if (strcmp(ccord_text_edit_cstr(&S.ed[F_NAME]), S.base.name) != 0) m |= 1u << CCORD_PROFILE_FIELD_DISPLAY_NAME;
  if (strcmp(ccord_text_edit_cstr(&S.ed[F_PRONOUNS]), S.base.pronouns) != 0) m |= 1u << CCORD_PROFILE_FIELD_PRONOUNS;
  if (strcmp(ccord_text_edit_cstr(&S.ed[F_BIO]), S.base.bio) != 0) m |= 1u << CCORD_PROFILE_FIELD_BIO;
  if (S.accent != S.base.accent || S.hex_bad) m |= 1u << CCORD_PROFILE_FIELD_ACCENT;
  if (S.img[IMG_AVATAR].mode != IMG_KEEP) m |= 1u << CCORD_PROFILE_FIELD_AVATAR;
  if (S.img[IMG_BANNER].mode != IMG_KEEP) m |= 1u << CCORD_PROFILE_FIELD_BANNER;
  return m;
}

// Follows the store: when the saved profile changed under a page with no edits, the drafts follow it.
static void sync_store(const ccord_ui *ui)
{
  snap cur;
  read_snap(ui, &cur);
  if (snap_eq(&cur, &S.base)) return;
  bool clean = changed_mask() == 0;
  S.base = cur;
  if (clean) load_all();
}

static void ensure_live(ccord_ui *ui)
{
  if (S.live && S.owner != ui) ui_settings_profile_release(nullptr);
  if (S.live) return;
  memset(&S, 0, sizeof S);
  S.live = true;
  S.owner = ui;
  S.awaiting = -1;
  S.accent = -1;
  S.pv_accent = -1;
  ccord_text_edit_init(&S.ed[F_NAME], CCORD_PROFILE_DISPLAY_NAME_MAX * 4u);
  ccord_text_edit_init(&S.ed[F_PRONOUNS], CCORD_PROFILE_PRONOUNS_MAX * 4u);
  ccord_text_edit_init(&S.ed[F_BIO], CCORD_PROFILE_BIO_MAX * 4u);
  ccord_text_edit_init(&S.ed[F_HEX], 16u);
  read_snap(ui, &S.base);
  load_all();
}

void ui_settings_profile_release(ccord_ui *ui)
{
  (void)ui;
  if (!S.live) return;
  for (int f = 0; f < F_TEXT_COUNT; f++) ccord_text_edit_free(&S.ed[f]);
  img_free(&S.img[IMG_AVATAR]);
  img_free(&S.img[IMG_BANNER]);
  memset(&S, 0, sizeof S);
  s_gen++;
}

bool ui_settings_profile_wants_text(const ccord_ui *ui)
{
  return ui->settings_open && ui->settings_page == UI_SETTINGS_PAGE_PROFILE &&
         ui->account_focus_field >= UI_PROFILE_FOCUS_BASE && ui->account_focus_field < UI_PROFILE_FOCUS_BASE + F_TEXT_COUNT;
}

void ui_settings_open_profile(ccord_ui *ui)
{
  ui_open_settings(ui, true);
  ui->settings_page = UI_SETTINGS_PAGE_PROFILE;
  ui->settings_category = 0;   // USER SETTINGS
  ui->account_focus_field = 0;
  ui->dirty = true;
}

// ---------------------------------------------------------------- text helpers

static size_t cp_count(const char *s, size_t len)
{
  size_t n = 0;
  for (size_t p = 0; p < len; p = ccord_utf8_next(s, len, p)) n++;
  return n;
}

static size_t field_max_cp(int f)
{
  switch (f) {
    case F_NAME: return CCORD_PROFILE_DISPLAY_NAME_MAX;
    case F_PRONOUNS: return CCORD_PROFILE_PRONOUNS_MAX;
    case F_BIO: return CCORD_PROFILE_BIO_MAX;
    default: return 7;   // "#RRGGBB"
  }
}

static bool parse_hex(const char *s, int32_t *out)
{
  if (*s == '#') s++;
  if (strlen(s) != 6) return false;
  uint32_t v = 0;
  for (int i = 0; i < 6; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else return false;
    v = v * 16u + d;
  }
  // if (v == 0) return false;   // 0 means "no accent" everywhere else; black is not a choosable accent
  *out = (int32_t)v;
  return true;
}

// Called after every edit of field `f`: forgets its error and refreshes what depends on the text.
static void edited(ccord_ui *ui, int f)
{
  static const int k_field[F_TEXT_COUNT] = {CCORD_PROFILE_FIELD_DISPLAY_NAME, CCORD_PROFILE_FIELD_PRONOUNS,
                                            CCORD_PROFILE_FIELD_BIO, CCORD_PROFILE_FIELD_ACCENT};
  S.err[k_field[f]][0] = '\0';
  if (f == F_HEX) {
    const char *t = ccord_text_edit_cstr(&S.ed[F_HEX]);
    int32_t v;
    if (t[0] == '\0') {
      S.accent = -1;
      S.hex_bad = false;
    } else if (parse_hex(t, &v)) {
      S.accent = v;
      S.hex_bad = false;
    } else {
      S.hex_bad = true;
    }
  }
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
  ui->dirty = true;
}

// Inserts at the caret, cut to the field's character limit (a selection being replaced frees room).
static void insert_capped(ccord_ui *ui, int f, const char *s, size_t len)
{
  ccord_text_edit *t = &S.ed[f];
  bool multi = f == F_BIO;
  char buf[1024];
  size_t o = 0;
  size_t sel = t->caret > t->anchor ? t->caret - t->anchor : t->anchor - t->caret;
  size_t lo = t->caret < t->anchor ? t->caret : t->anchor;
  size_t used = cp_count(t->data ? t->data : "", t->len) - cp_count(t->data ? t->data + lo : "", sel);
  size_t room = field_max_cp(f) > used ? field_max_cp(f) - used : 0;
  for (size_t p = 0; p < len && o < sizeof buf - 8u && room > 0;) {
    size_t n = ccord_utf8_next(s, len, p) - p;
    if (n == 1 && (s[p] == '\r' || (!multi && s[p] == '\n') || (f == F_HEX && !strchr("0123456789abcdefABCDEF#", s[p])))) {
      if (s[p] == '\n' || s[p] == '\r') {
        if (!multi && s[p] == '\n' && o > 0 && buf[o - 1] != ' ') {
          buf[o++] = ' ';
          room--;
        }
      }
      p += n;
      continue;
    }
    if (n == 1 && (unsigned char)s[p] < 0x20 && s[p] != '\n') {
      p += n;
      continue;
    }
    memcpy(buf + o, s + p, n);
    o += n;
    p += n;
    room--;
  }
  if (o == 0) return;
  ccord_error e;
  if (ccord_text_edit_insert(t, buf, o, &e) == CCORD_OK) edited(ui, f);
}

// ---------------------------------------------------------------- picking images

static const char *kind_noun(int slot)
{
  return slot == IMG_BANNER ? "banner" : "avatar";
}

static bool take_image(ccord_ui *ui, int slot, const char *name, const void *data, size_t size)
{
  (void)name;
  if (!data || size == 0) return false;
  if (size > CCORD_PROFILE_IMAGE_MAX_BYTES) {
    ui_notice(ui, "The %s must be 10 MB or smaller", kind_noun(slot));
    return true;
  }
  if (!ccord_profile_image_mime(data, size)) {
    ui_notice(ui, "Choose a PNG, JPEG, GIF or WebP image");
    return true;
  }
  uint8_t *copy = ccord_malloc(size);
  if (!copy) return true;
  memcpy(copy, data, size);
  img_draft *d = &S.img[slot];
  img_free(d);
  d->mode = IMG_NEW;
  d->data = copy;
  d->len = size;
  d->seq = ++s_img_seq;
  snprintf(d->key, sizeof d->key, "profile-draft:%s:%u", slot == IMG_BANNER ? "banner" : "avatar", d->seq);
  S.err[slot == IMG_BANNER ? CCORD_PROFILE_FIELD_BANNER : CCORD_PROFILE_FIELD_AVATAR][0] = '\0';
  S.awaiting = -1;
  ui->dirty = true;
  return true;
}

bool ui_settings_profile_take_image(ccord_ui *ui, const char *filename, const void *data, size_t size)
{
  if (!ui || !S.live || S.awaiting < 0 || !ui->settings_open || ui->settings_page != UI_SETTINGS_PAGE_PROFILE) return false;
  return take_image(ui, S.awaiting, filename, data, size);
}

bool ui_settings_profile_set_image_file(ccord_ui *ui, bool banner, const char *path)
{
  if (!ui || !path || !path[0]) return false;
  ensure_live(ui);
  FILE *f = fopen(path, "rb");
  if (!f) {
    ui_notice(ui, "Cannot open file: %s", path);
    return false;
  }
  uint8_t *buf = nullptr;
  size_t got = 0;
  if (fseek(f, 0, SEEK_END) == 0) {
    long sz = ftell(f);
    if (sz > 0 && (unsigned long)sz <= CCORD_PROFILE_IMAGE_MAX_BYTES && fseek(f, 0, SEEK_SET) == 0) {
      buf = ccord_malloc((size_t)sz);
      if (buf) got = fread(buf, 1, (size_t)sz, f);
      if (got != (size_t)sz) got = 0;
    } else if (sz > 0) {
      fclose(f);
      ui_notice(ui, "The %s must be 10 MB or smaller", banner ? "banner" : "avatar");
      return false;
    }
  }
  fclose(f);
  bool ok = false;
  if (buf && got > 0 && ccord_profile_image_mime(buf, got)) {
    ok = take_image(ui, banner ? IMG_BANNER : IMG_AVATAR, path, buf, got);
  } else {
    ui_notice(ui, "Choose a PNG, JPEG, GIF or WebP image");
  }
  ccord_free(buf);
  return ok;
}

// Change clicked: ask the platform's file dialog (the one the composer's attach button uses). The chosen path comes
// back at once, or later as a file event that ccord_ui_attach_bytes routes to ui_settings_profile_take_image.
static void pick_image(ccord_ui *ui, int slot)
{
  S.awaiting = slot;
  if (!ui->d.host.open_file_dialog) {
    ui_notice(ui, "No file picker here: drop an image file onto the window");
    return;
  }
  char path[512] = "";
  if (ui->d.host.open_file_dialog(ui->d.host.ud, path, sizeof path) && path[0]) {
    (void)ui_settings_profile_set_image_file(ui, slot == IMG_BANNER, path);
  }
}

// ---------------------------------------------------------------- saving

static void on_done(void *ud, const ccord_profile_edit_result *res)
{
  if ((uintptr_t)ud != s_gen || !S.live) return;   // the page was closed since: nothing to show
  S.cb_res = *res;
  S.cb_done = true;
}

static void do_save(ccord_ui *ui)
{
  if (S.pending) return;
  clear_errors();
  ui->dirty = true;
  uint32_t m = changed_mask();
  if (!m) return;
  if (S.hex_bad) {
    copy_str(S.err[CCORD_PROFILE_FIELD_ACCENT], sizeof S.err[0], "Enter a 6-digit hex colour, e.g. 5865F2");
    return;
  }
  ccord_profile_edit e = CCORD_PROFILE_EDIT_INIT;
  if (m & (1u << CCORD_PROFILE_FIELD_DISPLAY_NAME)) {
    e.has_display_name = true;
    e.display_name = ccord_text_edit_cstr(&S.ed[F_NAME]);
  }
  if (m & (1u << CCORD_PROFILE_FIELD_PRONOUNS)) {
    e.has_pronouns = true;
    e.pronouns = ccord_text_edit_cstr(&S.ed[F_PRONOUNS]);
  }
  if (m & (1u << CCORD_PROFILE_FIELD_BIO)) {
    e.has_bio = true;
    e.bio = ccord_text_edit_cstr(&S.ed[F_BIO]);
  }
  if (m & (1u << CCORD_PROFILE_FIELD_ACCENT)) {
    e.has_accent = true;
    e.accent_color = S.accent;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_AVATAR)) {
    e.has_avatar = true;
    e.avatar = S.img[IMG_AVATAR].mode == IMG_NEW ? S.img[IMG_AVATAR].data : nullptr;
    e.avatar_len = S.img[IMG_AVATAR].mode == IMG_NEW ? S.img[IMG_AVATAR].len : 0;
  }
  if (m & (1u << CCORD_PROFILE_FIELD_BANNER)) {
    e.has_banner = true;
    e.banner = S.img[IMG_BANNER].mode == IMG_NEW ? S.img[IMG_BANNER].data : nullptr;
    e.banner_len = S.img[IMG_BANNER].mode == IMG_NEW ? S.img[IMG_BANNER].len : 0;
  }
  if (!ui->d.session) {
    copy_str(S.general, sizeof S.general, "Not connected to Discord");
    return;
  }
  ccord_error err;
  if (ccord_session_edit_profile(ui->d.session, &e, on_done, (void *)s_gen, &err) != CCORD_OK) {
    copy_str(S.general, sizeof S.general, err.message[0] ? err.message : "Couldn't save your profile");
    return;
  }
  S.pending = true;
  S.sent = m;
}

// Discord's answer: fields it applied are no longer "changed" (the store already shows them); a refusal puts its
// message under the field it names and leaves every draft in place.
static void apply_result(ccord_ui *ui)
{
  const ccord_profile_edit_result *res = &S.cb_res;
  S.cb_done = false;
  S.pending = false;
  sync_store(ui);   // the store was updated before the callback ran
  uint32_t ap = res->applied.fields;
  if (ap & (1u << CCORD_PROFILE_FIELD_AVATAR)) img_free(&S.img[IMG_AVATAR]);
  if (ap & (1u << CCORD_PROFILE_FIELD_BANNER)) img_free(&S.img[IMG_BANNER]);
  if (ap & (1u << CCORD_PROFILE_FIELD_DISPLAY_NAME)) load_field(F_NAME);
  if (ap & (1u << CCORD_PROFILE_FIELD_PRONOUNS)) load_field(F_PRONOUNS);
  if (ap & (1u << CCORD_PROFILE_FIELD_BIO)) load_field(F_BIO);
  if (ap & (1u << CCORD_PROFILE_FIELD_ACCENT)) load_field(F_HEX);
  clear_errors();
  if (res->status == CCORD_OK) {
    S.saved_until_ms = ui->now_ms + SAVED_MS;
    ui->dirty = true;
    return;
  }
  bool named = false;
  for (size_t i = 0; i < res->field_error_count; i++) {
    const ccord_profile_field_error *fe = &res->field_errors[i];
    if (fe->field < CCORD_PROFILE_FIELD_COUNT) {
      copy_str(S.err[fe->field], sizeof S.err[0], fe->message[0] ? fe->message : "Discord refused this change");
      named = true;
    } else if (!S.general[0]) {
      copy_str(S.general, sizeof S.general, fe->message);
    }
  }
  if (res->password_required || res->mfa_required) {
    copy_str(S.general, sizeof S.general, "Discord wants to verify your account: make this change on discord.com");
  } else if (res->captcha_required) {
    copy_str(S.general, sizeof S.general, "Discord asked for a captcha: make this change on discord.com");
  } else if (res->rate_limited) {
    snprintf(S.general, sizeof S.general, "Slow down: try again in %d s", (int)(res->retry_after_s + 0.5));
  } else if (!named && !S.general[0]) {
    copy_str(S.general, sizeof S.general, res->message[0] ? res->message : "Couldn't save your profile");
  }
  ui->dirty = true;
}

bool ui_settings_profile_tick(ccord_ui *ui)
{
  if (!S.live) return false;
  if (!ui->settings_open) {
    ui_settings_profile_release(ui);
    return false;
  }
  if (ui->settings_page != UI_SETTINGS_PAGE_PROFILE) {
    S.awaiting = -1;
    if (ui->account_focus_field >= UI_PROFILE_FOCUS_BASE && ui->account_focus_field < UI_PROFILE_FOCUS_BASE + F_TEXT_COUNT) {
      ui->account_focus_field = 0;
    }
    return false;
  }
  if (S.cb_done) apply_result(ui);
  if (S.saved_until_ms && ui->now_ms >= S.saved_until_ms) {
    S.saved_until_ms = 0;
    ui->dirty = true;
  }
  return S.pending || S.saved_until_ms != 0;
}

// ---------------------------------------------------------------- input

static void focus_field(ccord_ui *ui, int f)
{
  ui->account_focus_field = f < 0 ? 0 : UI_PROFILE_FOCUS_BASE + f;
  if (f >= 0) {
    ccord_text_edit_move(&S.ed[f], 2, false);
    ui->caret_on = true;
    ui->caret_epoch_ms = ui->now_ms;
  }
  ui->dirty = true;
}

bool ui_settings_profile_click(ccord_ui *ui, ccord_rect content, ccord_rect panel, float x, float y)
{
  (void)content;
  ensure_live(ui);
  if (!ui_in_rect(x, y, panel)) return false;
  bool in_bar = S.bar_visible && ui_in_rect(x, y, S.bar);
  bool in_view = ui_in_rect(x, y, S.view) && !in_bar;
  for (size_t i = S.nitems; i-- > 0;) {
    const ui_profile_item *it = &S.items[i];
    bool bar_item = it->kind == UI_PROF_SAVE || it->kind == UI_PROF_RESET;
    if (!ui_in_rect(x, y, it->rect) || !(bar_item ? in_bar : in_view)) continue;
    switch (it->kind) {
      case UI_PROF_NAME:
      case UI_PROF_PRONOUNS:
      case UI_PROF_BIO:
      case UI_PROF_ACCENT_HEX:
        focus_field(ui, (int)it->kind);
        if (it->kind == UI_PROF_BIO) {
          S.click_pending = true;
          S.click_x = x;
          S.click_y = y;
        }
        return true;
      case UI_PROF_AVATAR_CHANGE: focus_field(ui, -1); pick_image(ui, IMG_AVATAR); return true;
      case UI_PROF_BANNER_CHANGE: focus_field(ui, -1); pick_image(ui, IMG_BANNER); return true;
      case UI_PROF_AVATAR_REMOVE:
      case UI_PROF_BANNER_REMOVE: {
        int slot = it->kind == UI_PROF_BANNER_REMOVE ? IMG_BANNER : IMG_AVATAR;
        const char *saved = slot == IMG_BANNER ? S.base.banner_url : S.base.avatar_url;
        img_free(&S.img[slot]);
        S.img[slot].mode = saved[0] ? IMG_REMOVE : IMG_KEEP;
        S.err[slot == IMG_BANNER ? CCORD_PROFILE_FIELD_BANNER : CCORD_PROFILE_FIELD_AVATAR][0] = '\0';
        focus_field(ui, -1);
        return true;
      }
      case UI_PROF_ACCENT_SWATCH:
        if (it->arg >= 0 && it->arg < ACCENT_PRESETS) S.accent = (int32_t)k_presets[it->arg];
        set_hex_text();
        S.err[CCORD_PROFILE_FIELD_ACCENT][0] = '\0';
        focus_field(ui, -1);
        return true;
      case UI_PROF_ACCENT_DEFAULT:
        S.accent = -1;
        set_hex_text();
        S.err[CCORD_PROFILE_FIELD_ACCENT][0] = '\0';
        focus_field(ui, -1);
        return true;
      case UI_PROF_SAVE:
        focus_field(ui, -1);
        do_save(ui);
        return true;
      case UI_PROF_RESET:
        if (!S.pending) {
          load_all();
          clear_errors();
        }
        focus_field(ui, -1);
        return true;
      case UI_PROF_WEB:
        (void)ui_open_url(ui, UI_PROFILE_WEB_URL, "Discord");
        ui->dirty = true;
        return true;
    }
  }
  if (in_view || in_bar) focus_field(ui, -1);   // a click on empty page space drops the keyboard focus
  return true;
}

static int focused_field(const ccord_ui *ui)
{
  int f = ui->account_focus_field - UI_PROFILE_FOCUS_BASE;
  return f >= 0 && f < F_TEXT_COUNT ? f : -1;
}

bool ui_settings_profile_key(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!S.live || ui->settings_page != UI_SETTINGS_PAGE_PROFILE) return false;
  int f = focused_field(ui);
  if (f < 0) return false;
  ccord_text_edit *t = &S.ed[f];
  if (ev->type == CCORD_IN_TEXT) {
    if (ev->text[0]) insert_capped(ui, f, ev->text, strlen(ev->text));
    return true;
  }
  if (ev->type != CCORD_IN_KEY_DOWN) return true;
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0, shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(t);
      else ccord_text_edit_backspace(t);
      edited(ui, f);
      return true;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(t);
      else ccord_text_edit_delete(t);
      edited(ui, f);
      return true;
    case CCORD_KEY_LEFT: ccord_text_edit_move(t, ctrl ? -3 : -1, shift); break;
    case CCORD_KEY_RIGHT: ccord_text_edit_move(t, ctrl ? 3 : 1, shift); break;
    case CCORD_KEY_HOME: ccord_text_edit_move(t, ctrl ? -2 : -4, shift); break;
    case CCORD_KEY_END: ccord_text_edit_move(t, ctrl ? 2 : 4, shift); break;
    case CCORD_KEY_RETURN:
      if (f == F_BIO) insert_capped(ui, f, "\n", 1);
      else focus_field(ui, f == F_HEX ? -1 : f + 1);
      return true;
    case CCORD_KEY_TAB: focus_field(ui, shift ? (f + F_TEXT_COUNT - 1) % F_TEXT_COUNT : (f + 1) % F_TEXT_COUNT); return true;
    case CCORD_KEY_A:
      if (ctrl) ccord_text_edit_select_all(t);
      break;
    case CCORD_KEY_Z:
      if (ctrl) {
        if (shift) (void)ccord_text_edit_redo(t);
        else (void)ccord_text_edit_undo(t);
        edited(ui, f);
      }
      break;
    case CCORD_KEY_Y:
      if (ctrl) {
        (void)ccord_text_edit_redo(t);
        edited(ui, f);
      }
      break;
    case CCORD_KEY_C:
    case CCORD_KEY_X:
      if (ctrl && ui->d.host.clipboard_set) {
        char *sel = ccord_text_edit_copy_selection(t);
        if (sel) {
          ui->d.host.clipboard_set(ui->d.host.ud, sel);
          ccord_free(sel);
          if (ev->key == CCORD_KEY_X) {
            ccord_text_edit_backspace(t);
            edited(ui, f);
          }
        }
      }
      break;
    case CCORD_KEY_V:
      if (ctrl && ui->d.host.clipboard_get) {
        char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
        if (clip) {
          insert_capped(ui, f, clip, strlen(clip));
          ccord_free(clip);
        }
      }
      break;
    default: break;
  }
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
  ui->dirty = true;
  return true;
}

bool ui_settings_profile_wheel(ccord_ui *ui, float wheel)
{
  if (!S.live) return false;
  float max = S.content_h - S.view_h;
  if (max <= 0.0f) return false;
  float old = S.scroll;
  S.scroll -= wheel * WHEEL_STEP;
  if (S.scroll < 0.0f) S.scroll = 0.0f;
  if (S.scroll > max) S.scroll = max;
  ui->dirty = true;
  return S.scroll != old;
}

// ---------------------------------------------------------------- drawing

typedef struct dctx {
  ccord_ui *ui;
  ccord_renderer *r;
  uis_tokens tk;
  float x, w;      // the field column
  float top;       // screen y of scroll-space 0
  float y;         // scroll-space cursor
} dctx;

static void add_item(ui_profile_kind kind, int arg, ccord_rect rc, const char *label)
{
  if (S.nitems >= UI_PROFILE_ITEMS_MAX) return;
  ui_profile_item *it = &S.items[S.nitems++];
  it->kind = kind;
  it->arg = arg;
  it->rect = rc;
  copy_str(it->label, sizeof it->label, label);
}

static bool hovered(const ccord_ui *ui, ccord_rect rc, bool in_bar)
{
  if (!ui_in_rect(ui->mx, ui->my, rc)) return false;
  if (in_bar) return true;
  return ui_in_rect(ui->mx, ui->my, S.view) && !(S.bar_visible && ui_in_rect(ui->mx, ui->my, S.bar));
}


static ccord_color rgb(uint32_t v)
{
  return (ccord_color){(uint8_t)((v >> 16) & 0xffu), (uint8_t)((v >> 8) & 0xffu), (uint8_t)(v & 0xffu), 255};
}

static void section_label(dctx *c, const char *label, const char *right, ccord_color right_col)
{
  ccord_ui *ui = c->ui;
  ccord_text_style ls = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_overline, COL(text_muted));
  ccord_text_draw(c->r, label, strlen(label), c->x, c->top + c->y, &ls, 0.0f);
  if (right) {
    ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_overline, right_col);
    ccord_vec2 m = ccord_text_measure(c->r, right, strlen(right), &rs, 0.0f);
    ccord_text_draw(c->r, right, strlen(right), c->x + c->w - m.x, c->top + c->y, &rs, 0.0f);
  }
  c->y += ui_line_h(ui, ls.size) + UIS_SP_2;
}

// The error Discord (or validation) gave for `field`, under the control; nothing when there is none.
static void field_error(dctx *c, int field)
{
  if (!S.err[field][0]) return;
  ccord_text_style es = ui_style(c->ui, CCORD_FONT_REGULAR, c->tk.fs_caption, c->ui->theme->c.danger);
  float h = ccord_text_measure(c->r, S.err[field], strlen(S.err[field]), &es, c->w).y;
  ccord_text_draw(c->r, S.err[field], strlen(S.err[field]), c->x, c->top + c->y + UIS_SP_1, &es, c->w);
  S.err_rect[field] = (ccord_rect){c->x, c->top + c->y + UIS_SP_1, c->w, h};
  c->y += h + UIS_SP_1;
}

static void caption(dctx *c, const char *text)
{
  ccord_text_style cs = ui_style(c->ui, CCORD_FONT_REGULAR, c->tk.fs_caption, c->ui->theme->c.text_muted);
  float h = ccord_text_measure(c->r, text, strlen(text), &cs, c->w).y;
  ccord_text_draw(c->r, text, strlen(text), c->x, c->top + c->y + UIS_SP_1, &cs, c->w);
  c->y += h + UIS_SP_1;
}

static void text_input(dctx *c, int f, ui_profile_kind kind, const char *placeholder, float w, float pad_l)
{
  ccord_ui *ui = c->ui;
  ccord_rect box = {c->x, c->top + c->y, w, UIS_H_INPUT};
  uis_state st = {.hover = hovered(ui, box, false), .focused = focused_field(ui) == f};
  (void)uis_input_padded(ui, c->r, box, pad_l, UIS_SP_3, &S.ed[f], placeholder, st, false, "pfld", (uint64_t)f);
  add_item(kind, f, box, placeholder);
  c->y += UIS_H_INPUT;
}

// The multi-line About Me box: wrapped text, caret and selection, scrolls to keep the caret in view.
static void bio_box(dctx *c)
{
  ccord_ui *ui = c->ui;
  ccord_renderer *r = c->r;
  ccord_text_edit *ed = &S.ed[F_BIO];
  ccord_rect box = {c->x, c->top + c->y, c->w, BIO_BOX_H};
  bool focused = focused_field(ui) == F_BIO;
  float ring = ui_fade(ui, "pfld", F_BIO, focused);
  if (ring > 0.004f) {
    ccord_draw_rrect(r, (ccord_rect){box.x - 1.0f, box.y - 1.0f, box.w + 2.0f, box.h + 2.0f}, c->tk.r_control + 1.0f,
                     ui_alpha(COL(brand), ring));
  }
  uis_plate(ui, r, box, c->tk.r_control, COL(bg_input));
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, c->tk.fs_body, COL(text_normal));
  float lh = ui_line_h(ui, ts.size), inner_w = box.w - 2.0f * UIS_SP_3;
  const char *t = ed->data ? ed->data : "";
  ccord_rect cr = ccord_text_caret_rect(r, t, ed->len, &ts, inner_w, ed->caret);
  float view_h = box.h - 2.0f * UIS_SP_2, shift = 0.0f;
  if (focused && cr.y + lh > view_h) shift = cr.y + lh - view_h;
  float tx = box.x + UIS_SP_3, ty = box.y + UIS_SP_2 - shift;
  if (S.click_pending && ui_in_rect(S.click_x, S.click_y, box)) {
    size_t pos = ccord_text_hit_test(r, t, ed->len, &ts, inner_w, S.click_x - tx, S.click_y - ty);
    ccord_text_edit_set_caret(ed, pos, false);
    ui->caret_on = true;
    ui->caret_epoch_ms = ui->now_ms;
    S.click_pending = false;
    cr = ccord_text_caret_rect(r, t, ed->len, &ts, inner_w, ed->caret);
  }
  ccord_push_clip(r, (ccord_rect){box.x, box.y + 1.0f, box.w, box.h - 2.0f});
  if (ed->len == 0) {
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, c->tk.fs_body, COL(text_placeholder));
    ccord_text_draw(r, "Tell people about yourself", 26, tx, ty, &ps, inner_w);
  } else {
    if (focused && ed->anchor != ed->caret) {
      size_t lo = ed->anchor < ed->caret ? ed->anchor : ed->caret, hi = ed->anchor < ed->caret ? ed->caret : ed->anchor;
      ccord_rect a = ccord_text_caret_rect(r, t, ed->len, &ts, inner_w, lo);
      ccord_rect b = ccord_text_caret_rect(r, t, ed->len, &ts, inner_w, hi);
      if (b.y - a.y < 1.0f) {
        ccord_draw_rect(r, (ccord_rect){tx + a.x, ty + a.y, b.x - a.x, lh}, COL(selection));
      } else {
        ccord_draw_rect(r, (ccord_rect){tx + a.x, ty + a.y, inner_w - a.x, lh}, COL(selection));
        for (float yy = a.y + lh; yy < b.y - 1.0f; yy += lh) ccord_draw_rect(r, (ccord_rect){tx, ty + yy, inner_w, lh}, COL(selection));
        ccord_draw_rect(r, (ccord_rect){tx, ty + b.y, b.x, lh}, COL(selection));
      }
    }
    ccord_text_draw(r, t, ed->len, tx, ty, &ts, inner_w);
  }
  if (focused && ui->caret_on) ccord_draw_rect(r, (ccord_rect){tx + cr.x, ty + cr.y, 1.5f, lh}, COL(caret));
  ccord_pop_clip(r);
  if (focused) ui->text_area = box;
  add_item(UI_PROF_BIO, F_BIO, box, "About Me");
  c->y += BIO_BOX_H;
}

static bool ensure_img(ccord_ui *ui, ccord_renderer *r, img_draft *d)
{
  if (d->mode != IMG_NEW || !d->data) return false;
  if (d->uploaded && ccord_render_has_texture(r, d->key, nullptr, nullptr)) return true;
  ccord_error e;
  int w, h;
  if (ccord_render_upload_image(r, d->key, d->data, d->len, &w, &h, &e) == CCORD_OK) {
    d->uploaded = true;
    return true;
  }
  ui_notice(ui, "Couldn't read that image");
  img_free(d);
  return false;
}

static void image_row(dctx *c, int slot, const char *heading, const char *change, int field)
{
  ccord_ui *ui = c->ui;
  section_label(c, heading, nullptr, COL(text_muted));
  float bh = uis_button_height(UIS_BTN_S_MD);
  float cw = uis_label_width(ui, c->r, change);
  ccord_rect crc = {c->x, c->top + c->y, cw, bh};
  uis_state cst = {.hover = hovered(ui, crc, false), .selected = S.awaiting == slot};
  uis_button(ui, c->r, crc, change, UIS_BTN_PRIMARY, UIS_BTN_S_MD, cst, "pbtn", (uint64_t)slot * 2u);
  add_item(slot == IMG_BANNER ? UI_PROF_BANNER_CHANGE : UI_PROF_AVATAR_CHANGE, 0, crc, change);
  const char *saved = slot == IMG_BANNER ? S.base.banner_url : S.base.avatar_url;
  img_draft *d = &S.img[slot];
  if (d->mode == IMG_NEW || (d->mode == IMG_KEEP && saved[0])) {
    const char *rm = slot == IMG_BANNER ? "Remove Banner" : "Remove Avatar";
    float rw = uis_label_width(ui, c->r, rm);
    ccord_rect rrc = {crc.x + cw + UIS_SP_2, crc.y, rw, bh};
    uis_state rst = {.hover = hovered(ui, rrc, false)};
    uis_button(ui, c->r, rrc, rm, UIS_BTN_SECONDARY, UIS_BTN_S_MD, rst, "pbtn", (uint64_t)slot * 2u + 1u);
    add_item(slot == IMG_BANNER ? UI_PROF_BANNER_REMOVE : UI_PROF_AVATAR_REMOVE, 0, rrc, rm);
  }
  c->y += bh;
  if (d->mode == IMG_NEW) caption(c, "New image chosen. Save Changes to apply it.");
  else if (d->mode == IMG_REMOVE) caption(c, "It will be removed when you save.");
  else if (S.awaiting == slot) caption(c, "Choose an image file, or drop one onto the window.");
  field_error(c, field);
}

static void accent_rows(dctx *c)
{
  ccord_ui *ui = c->ui;
  section_label(c, "ACCENT COLOUR", nullptr, COL(text_muted));
  for (int i = 0; i < ACCENT_PRESETS; i++) {
    ccord_rect rc = {c->x + (float)i * (UIS_SWATCH_D + UIS_SP_2), c->top + c->y, UIS_SWATCH_D, UIS_SWATCH_D};
    uis_state st = {.hover = hovered(ui, rc, false), .selected = S.accent == (int32_t)k_presets[i]};
    uis_swatch(ui, c->r, rc, rgb(k_presets[i]), st, "pswatch", (uint64_t)i);
    add_item(UI_PROF_ACCENT_SWATCH, i, rc, "Accent colour");
  }
  c->y += UIS_SWATCH_D + UIS_SP_2;
  ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, c->tk.fs_body, COL(text_normal));
  float hash_w = ccord_text_measure(c->r, "#", 1, &ts, 0.0f).x;
  float row_y = c->top + c->y;
  text_input(c, F_HEX, UI_PROF_ACCENT_HEX, "5865F2", 120.0f, UIS_SP_3 + hash_w + UIS_SP_1);
  ccord_text_draw(c->r, "#", 1, c->x + UIS_SP_3, row_y + (UIS_H_INPUT - ui_line_h(ui, ts.size)) * 0.5f, &ts, 0.0f);
  float bh = uis_button_height(UIS_BTN_S_MD), dw = uis_label_width(ui, c->r, "Default");
  ccord_rect drc = {c->x + 120.0f + UIS_SP_2, row_y + (UIS_H_INPUT - bh) * 0.5f, dw, bh};
  uis_state dst = {.hover = hovered(ui, drc, false)};
  uis_button(ui, c->r, drc, "Default", UIS_BTN_SECONDARY, UIS_BTN_S_MD, dst, "pbtn", 4u);
  add_item(UI_PROF_ACCENT_DEFAULT, 0, drc, "Default");
  field_error(c, CCORD_PROFILE_FIELD_ACCENT);
}

// The live preview card: shows the drafts exactly as the profile would look after Save.
static float preview_height(dctx *c, const char *bio, float pw)
{
  ccord_text_style bs = ui_style(c->ui, CCORD_FONT_REGULAR, c->tk.fs_control, c->ui->theme->c.text_muted);
  float lh = ui_line_h(c->ui, bs.size);
  float bio_h = lh;
  if (bio[0]) {
    bio_h = ccord_text_measure(c->r, bio, strlen(bio), &bs, pw - 2.0f * UIS_SP_4).y;
    if (bio_h > 8.0f * lh) bio_h = 8.0f * lh;
  }
  return PV_BANNER_H + PV_AVATAR_D * 0.5f + UIS_SP_2 + ui_line_h(c->ui, c->tk.fs_title) + ui_line_h(c->ui, c->tk.fs_control) +
         UIS_SP_3 + UIS_SP_3 + ui_line_h(c->ui, c->tk.fs_overline) + UIS_SP_1 + bio_h + UIS_SP_4;
}

static void draw_preview(dctx *c, float px, float py, float pw)
{
  ccord_ui *ui = c->ui;
  ccord_renderer *r = c->r;
  const uis_tokens *tk = &c->tk;
  const ccord_user *u = self_user(ui);
  ccord_user tmp = {0};
  if (u) tmp = *u;
  const char *username = tmp.handle ? tmp.handle : "";
  const char *typed = ccord_text_edit_cstr(&S.ed[F_NAME]);
  const char *shown = typed[0] ? typed : (username[0] ? username : "Your name");
  const char *pron = ccord_text_edit_cstr(&S.ed[F_PRONOUNS]);
  const char *bio = ccord_text_edit_cstr(&S.ed[F_BIO]);
  copy_str(S.pv_name, sizeof S.pv_name, shown);
  copy_str(S.pv_pronouns, sizeof S.pv_pronouns, pron);
  copy_str(S.pv_bio, sizeof S.pv_bio, bio);
  S.pv_accent = S.accent;
  tmp.name = shown;
  tmp.display_name = shown;
  if (S.img[IMG_AVATAR].mode == IMG_REMOVE) tmp.avatar_url = "";
  if (S.img[IMG_BANNER].mode == IMG_REMOVE) tmp.banner_url = "";
  const ccord_user_profile *prof = (ui->d.store && u) ? ccord_store_user_profile(ui->d.store, u->id) : nullptr;
  float ph = preview_height(c, bio, pw);
  ccord_rect card = {px, py, pw, ph};
  uis_card(ui, r, card);
  ccord_push_clip(r, card);
  ccord_rect banner = {px, py, pw, PV_BANNER_H};
  ui_profile_look look = {0};
  bool use_theme = S.accent < 0 && prof && prof->has_theme;
  if (use_theme) {
    ccord_user lview = tmp;
    if (S.img[IMG_BANNER].mode == IMG_KEEP && (!lview.banner_url || !lview.banner_url[0]) && prof->banner_url &&
        prof->banner_url[0]) {
      lview.banner_url = prof->banner_url;
    }
    ui_profile_resolve(prof, CCORD_ID_NONE, &lview, &look);
    ui_profile_draw_theme_bg(ui, r, &look, card, PV_BANNER_H, tk->r_surface);
    if (ensure_img(ui, r, &S.img[IMG_BANNER])) {
      const float border = 4.0f;
      ccord_rect inner = {banner.x + border, banner.y + border, banner.w - border * 2.0f, banner.h - border};
      float ir = tk->r_surface > border ? tk->r_surface - border : 0.0f;
      ccord_push_clip(r, inner);
      ccord_draw_texture_cover(r, S.img[IMG_BANNER].key, (ccord_rect){inner.x, inner.y, inner.w, inner.h + ir * 2.0f}, ir,
                               255, false);
      ccord_pop_clip(r);
    } else if (S.img[IMG_BANNER].mode == IMG_KEEP) {
      ui_profile_draw_banner(ui, r, &look, &lview, banner, tk->r_surface);
    }
  } else {
    ccord_color bcol = S.accent >= 0 ? rgb((uint32_t)S.accent) : ui_mix(COL(bg_tertiary), COL(bg_secondary), 0.35f);
    ccord_push_clip(r, banner);   // rounded at the top only: the bar runs past the banner's bottom edge and is clipped
    ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, banner.w, banner.h + tk->r_surface * 2.0f}, tk->r_surface, bcol);
    char burl[256];
    if (ensure_img(ui, r, &S.img[IMG_BANNER])) {
      ccord_draw_texture_cover(r, S.img[IMG_BANNER].key, banner, 0.0f, 255, false);
    } else if (S.img[IMG_BANNER].mode == IMG_KEEP && ui_user_banner_url(&tmp, burl, sizeof burl) &&
               ui_ensure_media(ui, r, burl)) {
      int btw = 0, bth = 0;
      if (ccord_render_has_texture(r, burl, &btw, &bth) && btw > 2 && bth > 2) {
        ccord_draw_texture_cover(r, burl, banner, 0.0f, 255, false);
      }
    }
    ccord_pop_clip(r);
  }
  ui_profile_text ptx = ui_profile_text_colors(ui, &look);
  ccord_color av_ring = use_theme ? ui_profile_card_bg(ui, &look, COL(bg_secondary)) : COL(bg_secondary);
  float acx = px + UIS_SP_4 + PV_AVATAR_D * 0.5f, acy = py + PV_BANNER_H;
  ccord_draw_circle(r, acx, acy, PV_AVATAR_D * 0.5f + UIS_SP_1, av_ring);
  if (ensure_img(ui, r, &S.img[IMG_AVATAR])) {
    ccord_draw_texture_rounded(r, S.img[IMG_AVATAR].key, (ccord_rect){acx - PV_AVATAR_D * 0.5f, acy - PV_AVATAR_D * 0.5f, PV_AVATAR_D, PV_AVATAR_D},
                               PV_AVATAR_D * 0.5f, 255);
  } else {
    ui_draw_avatar(ui, r, &tmp, acx, acy, PV_AVATAR_D, (ccord_color){0}, false);
  }
  if (use_theme) ui_profile_draw_deco(ui, r, &look, acx, acy, PV_AVATAR_D);
  float ty = py + PV_BANNER_H + PV_AVATAR_D * 0.5f + UIS_SP_2, tw = pw - 2.0f * UIS_SP_4, tx = px + UIS_SP_4;
  ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, tk->fs_title, ptx.header);
  ccord_text_draw_ellipsis(r, shown, strlen(shown), tx, ty, tw, &ns);
  ty += ui_line_h(ui, ns.size);
  char sub[256];
  snprintf(sub, sizeof sub, "%s%s%s", username, username[0] && pron[0] ? " \xC2\xB7 " : "", pron);
  ccord_text_style us = ui_style(ui, CCORD_FONT_REGULAR, tk->fs_control, ptx.secondary);
  ccord_text_draw_ellipsis(r, sub, strlen(sub), tx, ty, tw, &us);
  ty += ui_line_h(ui, us.size) + UIS_SP_3;
  uis_divider(ui, r, tx, ty, tw);
  ty += UIS_SP_3;
  ccord_text_style hs = ui_style(ui, CCORD_FONT_BOLD, tk->fs_overline, ptx.muted);
  ccord_text_draw(r, "ABOUT ME", 8, tx, ty, &hs, 0.0f);
  ty += ui_line_h(ui, hs.size) + UIS_SP_1;
  ccord_color bio_col = use_theme ? (bio[0] ? ptx.header : ptx.muted) : (bio[0] ? COL(text_normal) : COL(text_muted));
  ccord_text_style bs = ui_style(ui, CCORD_FONT_REGULAR, tk->fs_control, bio_col);
  float bh = 8.0f * ui_line_h(ui, bs.size);
  ccord_push_clip(r, (ccord_rect){tx, ty, tw, bh});
  const char *bt = bio[0] ? bio : "Nothing here yet.";
  ccord_text_draw(r, bt, strlen(bt), tx, ty, &bs, tw);
  ccord_pop_clip(r);
  ccord_pop_clip(r);
}

static void draw_bar(dctx *c, uint32_t changed)
{
  ccord_ui *ui = c->ui;
  ccord_renderer *r = c->r;
  ccord_rect bar = S.bar;
  uis_frame(ui, r, bar, c->tk.r_surface, COL(bg_floating), UIS_ELEV_POPOVER);
  float bh = uis_button_height(UIS_BTN_S_MD), by = bar.y + (bar.h - bh) * 0.5f;
  float right = bar.x + bar.w - UIS_SP_4;
  if (changed) {
    float sw = uis_label_width(ui, r, "Save Changes");
    ccord_rect src = {right - sw, by, sw, bh};
    uis_state sst = {.hover = hovered(ui, src, true), .disabled = S.pending};
    uis_button(ui, r, src, S.pending ? "Saving..." : "Save Changes", UIS_BTN_SUCCESS, UIS_BTN_S_MD, sst, "pbar", 1u);
    float rw = uis_label_width(ui, r, "Reset");
    ccord_rect rrc = {src.x - UIS_SP_2 - rw, by, rw, bh};
    uis_state rst = {.hover = hovered(ui, rrc, true), .disabled = S.pending};
    uis_button(ui, r, rrc, "Reset", UIS_BTN_LINK, UIS_BTN_S_MD, rst, "pbar", 0u);
    add_item(UI_PROF_RESET, 0, rrc, "Reset");
    add_item(UI_PROF_SAVE, 0, src, "Save Changes");
    right = rrc.x - UIS_SP_3;
  }
  const char *msg = "You have unsaved changes";
  ccord_color mc = COL(text_header);
  float tx = bar.x + UIS_SP_4;
  if (S.pending) {
    msg = "Saving your profile...";
  } else if (S.general[0]) {
    msg = S.general;
    mc = COL(danger);
  } else if (!changed) {
    msg = "Changes saved";
    float cy = bar.y + bar.h * 0.5f;
    ccord_draw_circle(r, tx + UIS_SP_3, cy, UIS_SP_3, COL(status_online));
    ccord_color wc = COL(text_on_brand);
    ccord_draw_line(r, (ccord_vec2){tx + UIS_SP_3 - 5.0f, cy}, (ccord_vec2){tx + UIS_SP_3 - 1.5f, cy + 3.5f}, 2.0f, wc);
    ccord_draw_line(r, (ccord_vec2){tx + UIS_SP_3 - 1.5f, cy + 3.5f}, (ccord_vec2){tx + UIS_SP_3 + 5.0f, cy - 3.5f}, 2.0f, wc);
    tx += UIS_SP_6 + UIS_SP_2;
  }
  ccord_text_style ms = ui_style(ui, CCORD_FONT_BOLD, c->tk.fs_control, mc);
  float max_w = right - tx;
  if (max_w > 0.0f) ccord_text_draw_ellipsis(r, msg, strlen(msg), tx, bar.y + (bar.h - ui_line_h(ui, ms.size)) * 0.5f, max_w, &ms);
}

void ui_settings_profile_draw(ccord_ui *ui, ccord_renderer *r, ccord_rect content, ccord_rect panel)
{
  const ccord_user *u_self = self_user(ui);
  if (ui->d.session && u_self && u_self->id != CCORD_ID_NONE) {
    ccord_error perr;
    (void)ccord_session_request_profile_ex(ui->d.session, u_self->id, CCORD_ID_NONE, ui->now_ms, false, &perr);
  }
  ensure_live(ui);
  sync_store(ui);
  uis_tokens tk = uis_tokens_of(ui);
  S.nitems = 0;
  memset(S.err_rect, 0, sizeof S.err_rect);
  uint32_t changed = changed_mask();
  float top = content.y + 52.0f;
  S.bar_visible = changed != 0 || S.pending || S.saved_until_ms != 0 || S.general[0] != '\0';
  S.bar = (ccord_rect){content.x, panel.y + panel.h - UIS_SP_4 - BAR_H, content.w, BAR_H};
  S.view = (ccord_rect){content.x - UIS_SP_2, top, content.w + 2.0f * UIS_SP_2, panel.y + panel.h - top};
  S.view_h = S.view.h;
  float max_scroll = S.content_h - S.view_h;
  if (max_scroll < 0.0f) max_scroll = 0.0f;
  if (S.scroll > max_scroll) S.scroll = max_scroll;
  bool two_col = content.w >= 700.0f;
  float fw = content.w;
  if (two_col) fw = content.w - PV_W - UIS_SP_8;
  if (fw > 480.0f) fw = 480.0f;
  dctx c = {.ui = ui, .r = r, .tk = tk, .x = content.x, .w = fw, .top = top - S.scroll, .y = 0.0f};

  ccord_push_clip(r, S.view);
  if (two_col) {
    draw_preview(&c, content.x + fw + UIS_SP_8, top, PV_W);   // fixed: the preview stays in view while the fields scroll
  } else {
    float pw = content.w < PV_W ? content.w : PV_W;
    draw_preview(&c, content.x, c.top + c.y, pw);
    c.y += preview_height(&c, ccord_text_edit_cstr(&S.ed[F_BIO]), pw) + UIS_SP_6;
  }

  section_label(&c, "DISPLAY NAME", nullptr, COL(text_muted));
  text_input(&c, F_NAME, UI_PROF_NAME, "Display name", fw, UIS_SP_3);
  field_error(&c, CCORD_PROFILE_FIELD_DISPLAY_NAME);
  c.y += UIS_SP_5;

  section_label(&c, "PRONOUNS", nullptr, COL(text_muted));
  text_input(&c, F_PRONOUNS, UI_PROF_PRONOUNS, "Add your pronouns", fw, UIS_SP_3);
  field_error(&c, CCORD_PROFILE_FIELD_PRONOUNS);
  c.y += UIS_SP_5;

  char left[32];
  long remaining = (long)CCORD_PROFILE_BIO_MAX - (long)cp_count(ccord_text_edit_cstr(&S.ed[F_BIO]), S.ed[F_BIO].len);
  snprintf(left, sizeof left, "%ld", remaining);
  section_label(&c, "ABOUT ME", left, remaining <= 10 ? COL(danger) : COL(text_muted));
  bio_box(&c);
  field_error(&c, CCORD_PROFILE_FIELD_BIO);
  c.y += UIS_SP_5;

  image_row(&c, IMG_AVATAR, "AVATAR", "Change Avatar", CCORD_PROFILE_FIELD_AVATAR);
  c.y += UIS_SP_5;
  image_row(&c, IMG_BANNER, "BANNER", "Change Banner", CCORD_PROFILE_FIELD_BANNER);
  c.y += UIS_SP_5;
  accent_rows(&c);
  c.y += UIS_SP_6;

  // Credentials never change here: one sentence and a link out.
  uis_divider(ui, r, c.x, c.top + c.y, c.w);
  c.y += UIS_SP_5;
  section_label(&c, "PASSWORD AND AUTHENTICATION", nullptr, COL(text_muted));
  const char *sentence = "Password, email, username and two-factor authentication changes are made on discord.com.";
  ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, COL(text_normal));
  ccord_text_draw(r, sentence, strlen(sentence), c.x, c.top + c.y, &ps, c.w);
  c.y += ccord_text_measure(r, sentence, strlen(sentence), &ps, c.w).y + UIS_SP_3;
  float bh = uis_button_height(UIS_BTN_S_MD), ww = uis_label_width(ui, r, "Open discord.com");
  ccord_rect wrc = {c.x, c.top + c.y, ww, bh};
  uis_state wst = {.hover = hovered(ui, wrc, false)};
  uis_button(ui, r, wrc, "Open discord.com", UIS_BTN_SECONDARY, UIS_BTN_S_MD, wst, "pbtn", 5u);
  add_item(UI_PROF_WEB, 0, wrc, "Open discord.com");
  c.y += bh;
  S.content_h = 52.0f + c.y + UIS_SP_6 + BAR_H + 2.0f * UIS_SP_4;
  ccord_pop_clip(r);

  if (S.bar_visible) draw_bar(&c, changed);
}

// ---------------------------------------------------------------- test seams

size_t ui_settings_profile_items(const ccord_ui *ui, const ui_profile_item **out)
{
  (void)ui;
  if (out) *out = S.items;
  return S.live ? S.nitems : 0;
}

void ui_settings_profile_debug(const ccord_ui *ui, ui_profile_debug *out)
{
  (void)ui;
  *out = (ui_profile_debug){0};
  out->preview_accent = -1;
  if (!S.live) return;
  out->live = true;
  out->changed = changed_mask();
  out->pending = S.pending;
  out->saved = S.saved_until_ms != 0;
  out->bar_visible = S.bar_visible;
  for (int i = 0; i < 6; i++) {
    out->has_error[i] = S.err[i][0] != '\0';
    copy_str(out->error[i], sizeof out->error[i], S.err[i]);
  }
  copy_str(out->general, sizeof out->general, S.general);
  for (int i = 0; i < 6; i++) out->error_rect[i] = S.err_rect[i];
  copy_str(out->preview_name, sizeof out->preview_name, S.pv_name);
  copy_str(out->preview_pronouns, sizeof out->preview_pronouns, S.pv_pronouns);
  copy_str(out->preview_bio, sizeof out->preview_bio, S.pv_bio);
  out->preview_accent = S.pv_accent;
  out->avatar_mode = S.img[IMG_AVATAR].mode;
  out->banner_mode = S.img[IMG_BANNER].mode;
  out->bar = S.bar;
  out->view = S.view;
  out->scroll = S.scroll;
  out->max_scroll = S.content_h > S.view_h ? S.content_h - S.view_h : 0.0f;
  for (int f = 0; f < F_TEXT_COUNT; f++) out->text[f] = ccord_text_edit_cstr(&S.ed[f]);
}
