// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"
#include <ccord/core/alloc.h>

#if CCORD_WEB_CAPTCHA
#include <ccord/web/web_view.h>
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

#if CCORD_WEB_CAPTCHA

#define CAPTCHA_HEADER_H         140.0f
#define CAPTCHA_HEADER_COMPACT_H  86.0f
#define CAPTCHA_FOOTER_H          64.0f
#define CAPTCHA_FOOTER_MOBILE_H   70.0f
#define CAPTCHA_WELL_COMPACT_H   132.0f
#define CAPTCHA_WELL_URL_H       520.0f
#define CAPTCHA_QUEUE_MAX          8u

#if CCORD_LIVE_ACCOUNT
typedef struct app_captcha_queued {
  uint64_t challenge_id;
  ccord_captcha_challenge challenge;
  char route_path[128];
  struct app_captcha_queued *next;
} app_captcha_queued;
#endif

static bool captcha_is_narrow(const app *a)
{
  float vw = (a && a->viewport_w > 100.0f) ? a->viewport_w : 960.0f;
  return vw < 520.0f;
}

static bool captcha_is_touch(const app *a)
{
#if CCORD_OS_ANDROID
  (void)a;
  return true;
#else
  return (a && a->opt && a->opt->mobile) ||
         (a && a->platform && ccord_platform_has_touch(a->platform));
#endif
}

static float captcha_pad_x(const app *a)
{
  return captcha_is_narrow(a) ? 10.0f : 24.0f;
}

static float captcha_footer_h(const app *a)
{
  return (captcha_is_narrow(a) || captcha_is_touch(a)) ? CAPTCHA_FOOTER_MOBILE_H : CAPTCHA_FOOTER_H;
}

static bool captcha_use_compact_header(const app *a)
{
  return a && a->captcha_challenge_open && !a->captcha_is_url_mode;
}

static float captcha_header_h(const app *a)
{
  return captcha_use_compact_header(a) ? CAPTCHA_HEADER_COMPACT_H : CAPTCHA_HEADER_H;
}

static ccord_rect captcha_safe_bounds(const app *a)
{
  float vw = (a && a->viewport_w > 100.0f) ? a->viewport_w : 960.0f;
  float vh = (a && a->viewport_h > 100.0f) ? a->viewport_h : 720.0f;
  float sl = a ? fmaxf(0.0f, a->last_safe.left) : 0.0f;
  float sr = a ? fmaxf(0.0f, a->last_safe.right) : 0.0f;
  float st = a ? fmaxf(0.0f, a->last_safe.top) : 0.0f;
  float sb = a ? fmaxf(fmaxf(0.0f, a->last_safe.bottom), fmaxf(0.0f, a->last_kb_inset)) : 0.0f;
  float sw = fmaxf(280.0f, vw - sl - sr);
  float sh = fmaxf(260.0f, vh - st - sb);
  return (ccord_rect){sl, st, sw, sh};
}

static bool captcha_theme_is_dark(const app *a)
{
  if (!a || !a->theme) return true;
  ccord_color bg = a->theme->c.bg_secondary;
  int lum = ((int)bg.r * 299 + (int)bg.g * 587 + (int)bg.b * 114) / 1000;
  return lum < 128;
}

static ccord_color c_alpha(ccord_color c, uint8_t alpha)
{
  c.a = alpha;
  return c;
}

static ccord_color c_mix(ccord_color a, ccord_color b, float t)
{
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  return (ccord_color){
    (uint8_t)((float)a.r + ((float)b.r - (float)a.r) * t + 0.5f),
    (uint8_t)((float)a.g + ((float)b.g - (float)a.g) * t + 0.5f),
    (uint8_t)((float)a.b + ((float)b.b - (float)a.b) * t + 0.5f),
    (uint8_t)((float)a.a + ((float)b.a - (float)a.a) * t + 0.5f),
  };
}

static float captcha_max_well_h(const app *a)
{
  ccord_rect sb = captcha_safe_bounds(a);
  float margin_y = captcha_is_narrow(a) ? 16.0f : 24.0f;
  return fmaxf(120.0f, sb.h - margin_y - captcha_header_h(a) - captcha_footer_h(a));
}

static ccord_rect captcha_modal_card_rect(const app *a)
{
  ccord_rect sb = captcha_safe_bounds(a);
  bool narrow = captcha_is_narrow(a);
  float margin_x = narrow ? 12.0f : 24.0f;
  float margin_y = narrow ? 16.0f : 24.0f;
  float want_w = (a && (a->captcha_challenge_open || a->captcha_is_url_mode)) ? 516.0f : 460.0f;
  float mw = fminf(want_w, sb.w - margin_x);
  if (mw < 280.0f) mw = sb.w > 0.0f ? sb.w : 460.0f;
  float well_h = (a && a->captcha_well_h_cur > 10.0f) ? a->captcha_well_h_cur : CAPTCHA_WELL_COMPACT_H;
  float max_h = fmaxf(260.0f, sb.h - margin_y);
  float mh = fminf(captcha_header_h(a) + well_h + captcha_footer_h(a), max_h);
  return (ccord_rect){floorf(sb.x + (sb.w - mw) * 0.5f), floorf(sb.y + (sb.h - mh) * 0.5f), mw, mh};
}

static ccord_rect captcha_close_btn_rect(const app *a, ccord_rect card)
{
  float sz = (captcha_is_narrow(a) || captcha_is_touch(a)) ? 34.0f : 28.0f;
  float inset = (captcha_is_narrow(a) || captcha_is_touch(a)) ? 8.0f : 10.0f;
  return (ccord_rect){card.x + card.w - sz - inset, card.y + inset, sz, sz};
}

static ccord_rect captcha_well_rect(const app *a, ccord_rect card)
{
  float hh = captcha_header_h(a);
  float px = captcha_pad_x(a);
  float h = fmaxf(88.0f, card.h - hh - captcha_footer_h(a));
  return (ccord_rect){card.x + px, card.y + hh, card.w - 2.0f * px, h};
}

static ccord_rect captcha_web_rect(const app *a, ccord_rect card)
{
  ccord_rect well = captcha_well_rect(a, card);
  return (ccord_rect){well.x + 2.0f, well.y + 2.0f, fmaxf(40.0f, well.w - 4.0f), fmaxf(40.0f, well.h - 4.0f)};
}

static ccord_rect captcha_reload_btn_rect(const app *a, ccord_rect card)
{
  float px = captcha_pad_x(a);
  float bh = (captcha_is_narrow(a) || captcha_is_touch(a)) ? 40.0f : 34.0f;
  float by = card.y + card.h - floorf((captcha_footer_h(a) + bh) * 0.5f);
  float bw = captcha_is_narrow(a) ? 96.0f : 108.0f;
  return (ccord_rect){card.x + px, by, bw, bh};
}

static ccord_rect captcha_cancel_btn_rect(const app *a, ccord_rect card)
{
  float px = captcha_pad_x(a);
  ccord_rect rbtn = captcha_reload_btn_rect(a, card);
  float x = rbtn.x + rbtn.w + 10.0f;
  float w = fmaxf(100.0f, (card.x + card.w - px) - x);
  return (ccord_rect){x, rbtn.y, w, rbtn.h};
}

static bool pt_in_rect(float x, float y, ccord_rect r)
{
  return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

static void on_captcha_web_msg(void *ud, const char *kind, const char *payload)
{
  app *a = ud;
  if (!a || !kind) return;
  if (strcmp(kind, "solved") == 0 && payload && payload[0]) {
    snprintf(a->captcha_solved_token, sizeof a->captcha_solved_token, "%s", payload);
    a->captcha_has_solution = true;
    a->captcha_is_error = false;
    a->captcha_solved_banner = true;
    a->captcha_widget_ready = true;
    snprintf(a->captcha_modal_status, sizeof a->captcha_modal_status, "%s",
             a->captcha_test_mode ? "Verified! Test token received."
                                  : (a->captcha_rest_id != 0 ? "Verified! Completing action..."
                                                             : "Verified! Signing in to Discord..."));
    CCORD_LOG_I("captcha: solved token received (%zu bytes)", strlen(payload));
  } else if (strcmp(kind, "ready") == 0) {
    a->captcha_is_error = false;
    snprintf(a->captcha_modal_status, sizeof a->captcha_modal_status,
             "Complete the verification below to continue.");
  } else if (strcmp(kind, "expired") == 0) {
    a->captcha_is_error = true;
    a->captcha_solved_banner = false;
    a->captcha_widget_ready = true;
    snprintf(a->captcha_modal_status, sizeof a->captcha_modal_status,
             "Verification expired — click Reload or try again.");
  } else if (strcmp(kind, "error") == 0) {
    a->captcha_is_error = true;
    a->captcha_solved_banner = false;
    a->captcha_widget_ready = true;
    snprintf(a->captcha_modal_status, sizeof a->captcha_modal_status, "Verification error: %s",
             (payload && payload[0]) ? payload : "unknown");
    CCORD_LOG_W("captcha: widget error: %s", (payload && payload[0]) ? payload : "unknown");
  }
}

static void captcha_reset_view_state(app *a, bool is_url_mode)
{
  uint64_t now = app_now_ms(a);
  a->captcha_is_url_mode = is_url_mode;
  a->captcha_has_solution = false;
  a->captcha_is_error = false;
  a->captcha_solved_banner = false;
  a->captcha_widget_ready = is_url_mode;
  a->captcha_challenge_open = false;
  a->captcha_open_ms = now;
  a->captcha_last_anim_ms = now;
  a->captcha_pressed_btn = 0;
  a->captcha_touch_btn_active = false;
  a->captcha_touch_btn_finger = 0;
  a->captcha_mx = -1.0f;
  a->captcha_my = -1.0f;
  float init_h = is_url_mode ? fminf(CAPTCHA_WELL_URL_H, captcha_max_well_h(a)) : CAPTCHA_WELL_COMPACT_H;
  a->captcha_well_h_cur = init_h;
  a->captcha_well_h_target = init_h;
  a->captcha_solved_token[0] = '\0';
}

static void captcha_destroy_active(app *a)
{
  if (!a) return;
  if (a->captcha_web) {
    ccord_web_view_destroy(a->captcha_web);
    a->captcha_web = nullptr;
  }
  a->captcha_rest_id = 0;
  a->captcha_test_mode = false;
  a->captcha_has_solution = false;
  a->captcha_is_error = false;
  a->captcha_solved_banner = false;
  a->captcha_widget_ready = false;
  a->captcha_challenge_open = false;
  a->captcha_is_url_mode = false;
  a->captcha_pressed_btn = 0;
  a->captcha_touch_btn_active = false;
  a->captcha_touch_btn_finger = 0;
  ccord_secure_wipe(a->captcha_solved_token, sizeof a->captcha_solved_token);
  ccord_secure_wipe(a->captcha_rqdata, sizeof a->captcha_rqdata);
  ccord_secure_wipe(a->captcha_rest_rqtoken, sizeof a->captcha_rest_rqtoken);
  ccord_secure_wipe(a->captcha_rest_session_id, sizeof a->captcha_rest_session_id);
  a->captcha_rest_route[0] = '\0';
  a->captcha_service[0] = '\0';
  a->captcha_sitekey[0] = '\0';
  a->captcha_host[0] = '\0';
  a->captcha_url[0] = '\0';
  a->captcha_modal_status[0] = '\0';
}

#if CCORD_LIVE_ACCOUNT
static void captcha_clear_queue(app *a, bool cancel_rest)
{
  if (!a) return;
  while (a->captcha_queue_head) {
    app_captcha_queued *item = a->captcha_queue_head;
    a->captcha_queue_head = item->next;
    if (a->captcha_queue_count > 0) a->captcha_queue_count--;
    if (cancel_rest && item->challenge_id != 0) (void)ccord_rest_captcha_cancel(item->challenge_id);
    ccord_secure_wipe(item, sizeof *item);
    ccord_free(item);
  }
  a->captcha_queue_tail = nullptr;
  a->captcha_queue_count = 0;
}

static bool captcha_open_rest(app *a, uint64_t challenge_id, const ccord_captcha_challenge *ch, const char *route_path)
{
  if (!a || !ch || !ch->sitekey[0] || challenge_id == 0) return false;
  if (!app_captcha_open_ex(a, ch->service[0] ? ch->service : "hcaptcha", ch->sitekey, "discord.com",
                           ch->rqdata[0] ? ch->rqdata : nullptr, false)) {
    return false;
  }
  a->captcha_rest_id = challenge_id;
  snprintf(a->captcha_rest_rqtoken, sizeof a->captcha_rest_rqtoken, "%s", ch->rqtoken);
  snprintf(a->captcha_rest_session_id, sizeof a->captcha_rest_session_id, "%s", ch->session_id);
  snprintf(a->captcha_rest_route, sizeof a->captcha_rest_route, "%s", route_path ? route_path : "");
  a->media_redraw = true;
  CCORD_LOG_I("captcha: opened global modal for %s (id=%llu, rqdata=%zu B)",
              a->captcha_rest_route[0] ? a->captcha_rest_route : "request",
              (unsigned long long)challenge_id, strlen(a->captcha_rqdata));
  return true;
}

static void captcha_open_next_queued(app *a)
{
  if (!a) return;
  while (a->captcha_queue_head) {
    app_captcha_queued *item = a->captcha_queue_head;
    a->captcha_queue_head = item->next;
    if (!a->captcha_queue_head) a->captcha_queue_tail = nullptr;
    if (a->captcha_queue_count > 0) a->captcha_queue_count--;
    bool still_live = ccord_rest_captcha_pending(item->challenge_id);
    bool opened = still_live && captcha_open_rest(a, item->challenge_id, &item->challenge, item->route_path);
    if (still_live && !opened) (void)ccord_rest_captcha_cancel(item->challenge_id);
    ccord_secure_wipe(item, sizeof *item);
    ccord_free(item);
    if (opened) break;
  }
}

static bool on_rest_captcha(void *ud, uint64_t challenge_id, const ccord_captcha_challenge *ch, const char *route_path)
{
  app *a = ud;
  if (!a || !ch || !ch->sitekey[0] || !a->renderer || a->virtual_clock) return false;
  if (a->opt && (a->opt->headless || a->opt->screenshot || a->opt->verify_slice)) return false;
  if (ch->service[0] && strcmp(ch->service, "hcaptcha") != 0) {
    CCORD_LOG_W("captcha: unsupported provider '%s' on %s; passing through",
                ch->service, route_path ? route_path : "request");
    return false;
  }
  if (!app_captcha_active(a)) {
    return captcha_open_rest(a, challenge_id, ch, route_path);
  }
  if (a->captcha_queue_count >= CAPTCHA_QUEUE_MAX) return false;
  app_captcha_queued *item = ccord_calloc(1, sizeof *item);
  if (!item) return false;
  item->challenge_id = challenge_id;
  item->challenge = *ch;
  snprintf(item->route_path, sizeof item->route_path, "%s", route_path ? route_path : "");
  if (a->captcha_queue_tail) a->captcha_queue_tail->next = item;
  else a->captcha_queue_head = item;
  a->captcha_queue_tail = item;
  a->captcha_queue_count++;
  CCORD_LOG_I("captcha: queued global challenge for %s (id=%llu, queued=%zu)",
              item->route_path[0] ? item->route_path : "request",
              (unsigned long long)challenge_id, a->captcha_queue_count);
  return true;
}

void app_captcha_register_rest_handler(app *a)
{
  if (!a) return;
  ccord_rest_set_global_captcha_handler(on_rest_captcha, a);
}

void app_captcha_unregister_rest_handler(app *a)
{
  ccord_rest_set_global_captcha_handler(nullptr, nullptr);
  captcha_clear_queue(a, true);
}
#else
void app_captcha_register_rest_handler(app *a) { (void)a; }
void app_captcha_unregister_rest_handler(app *a) { (void)a; }
#endif

bool app_captcha_open_ex(app *a, const char *service, const char *sitekey, const char *host,
                         const char *rqdata, bool test_mode)
{
  if (!a) return false;
  // Save params before closing in case Reload calls us with our own buffers.
  char svc[32], sk[128], hst[128], rqd[8192];
  snprintf(svc, sizeof svc, "%s", (service && service[0]) ? service : "hcaptcha");
  snprintf(sk, sizeof sk, "%s", sitekey ? sitekey : "");
  snprintf(hst, sizeof hst, "%s", (host && host[0]) ? host : "discord.com");
  snprintf(rqd, sizeof rqd, "%s", rqdata ? rqdata : "");

  captcha_destroy_active(a);
  captcha_reset_view_state(a, false);
  if (a->platform) {
    ccord_platform_text_input(a->platform, false, 0.0f, 0.0f, 0.0f, 0.0f);
    ccord_platform_dismiss_keyboard(a->platform);
  }
  a->captcha_test_mode = test_mode;
  snprintf(a->captcha_service, sizeof a->captcha_service, "%s", svc);
  snprintf(a->captcha_sitekey, sizeof a->captcha_sitekey, "%s", sk);
  snprintf(a->captcha_host, sizeof a->captcha_host, "%s", hst);
  snprintf(a->captcha_rqdata, sizeof a->captcha_rqdata, "%s", rqd);
  a->captcha_url[0] = '\0';

  ccord_rect card = captcha_modal_card_rect(a);
  ccord_rect wr = captcha_web_rect(a, card);
  bool dark = captcha_theme_is_dark(a);
  ccord_color page_bg = a->theme ? a->theme->c.bg_primary : (dark ? (ccord_color){0x1e, 0x1f, 0x22, 255}
                                                                  : (ccord_color){0xf2, 0xf3, 0xf5, 255});
  ccord_color page_fg = a->theme ? a->theme->c.text_normal : (dark ? (ccord_color){0xdb, 0xde, 0xe1, 255}
                                                                   : (ccord_color){0x1e, 0x1f, 0x22, 255});
  ccord_web_config wcfg = {
    .viewport_w = wr.w,
    .viewport_h = wr.h,
    .dark_theme = dark,
    .touch_enabled = captcha_is_touch(a),
    .allow_network = true,
    .allow_loopback_http = test_mode,
    .captcha_host = hst,
    .page_bg = page_bg,
    .page_fg = page_fg,
    .on_msg = on_captcha_web_msg,
    .on_msg_ud = a,
  };
  ccord_error err = {0};
  a->captcha_web = ccord_web_view_create(&wcfg, &err);
  if (!a->captcha_web) {
    CCORD_LOG_W("captcha: failed to create native web view: %s", err.message);
    ccord_secure_wipe(rqd, sizeof rqd);
    return false;
  }
  snprintf(a->captcha_modal_status, sizeof a->captcha_modal_status, "Loading verification challenge...");

  ccord_status st = ccord_web_view_load_captcha(a->captcha_web, svc, sk, rqd[0] ? rqd : nullptr, dark, &err);
  ccord_secure_wipe(rqd, sizeof rqd);
  if (st != CCORD_OK) {
    CCORD_LOG_W("captcha: failed to load challenge: %s", err.message);
    captcha_destroy_active(a);
    return false;
  }
  a->media_redraw = true;
  return true;
}

bool app_captcha_open(app *a, const char *service, const char *sitekey, const char *rqdata, bool test_mode)
{
  return app_captcha_open_ex(a, service, sitekey, "discord.com", rqdata, test_mode);
}

bool app_captcha_open_url(app *a, const char *url)
{
  if (!a || !url || !url[0]) return false;
  char ucopy[512];
  snprintf(ucopy, sizeof ucopy, "%s", url);

  captcha_destroy_active(a);
  captcha_reset_view_state(a, true);
  if (a->platform) {
    ccord_platform_text_input(a->platform, false, 0.0f, 0.0f, 0.0f, 0.0f);
    ccord_platform_dismiss_keyboard(a->platform);
  }
  a->captcha_test_mode = true;
  snprintf(a->captcha_url, sizeof a->captcha_url, "%s", ucopy);

  ccord_rect card = captcha_modal_card_rect(a);
  ccord_rect wr = captcha_web_rect(a, card);
  bool dark = captcha_theme_is_dark(a);
  ccord_web_config wcfg = {
    .viewport_w = wr.w,
    .viewport_h = wr.h,
    .dark_theme = dark,
    .touch_enabled = captcha_is_touch(a),
    .allow_network = true,
    .allow_loopback_http = true,
    .on_msg = on_captcha_web_msg,
    .on_msg_ud = a,
  };
  ccord_error err = {0};
  a->captcha_web = ccord_web_view_create(&wcfg, &err);
  if (!a->captcha_web) return false;
  snprintf(a->captcha_modal_status, sizeof a->captcha_modal_status, "Loading %.240s", ucopy);
  if (ccord_web_view_load_url(a->captcha_web, ucopy, &err) != CCORD_OK) {
    CCORD_LOG_W("web: failed to load %s: %s", ucopy, err.message);
    captcha_destroy_active(a);
    return false;
  }
  a->media_redraw = true;
  return true;
}

void app_captcha_close(app *a, bool user_cancelled)
{
  if (!a) return;
  uint64_t rest_id = a->captcha_rest_id;
  bool was_test = a->captcha_test_mode;
  captcha_destroy_active(a);
  if (rest_id != 0) {
#if CCORD_LIVE_ACCOUNT
    (void)ccord_rest_captcha_cancel(rest_id);
    if (user_cancelled) {
      captcha_open_next_queued(a);
    } else {
      captcha_clear_queue(a, true);
    }
#endif
    return;
  }
  if (user_cancelled && !was_test) {
#if CCORD_LIVE_ACCOUNT
    ccord_live_wipe(&a->captcha_challenge, sizeof a->captcha_challenge);
    a->captcha_attempts = 0;
    a->auth_pending = false;
#endif
    if (a->ui) ccord_ui_set_login_error(a->ui, "Verification cancelled. Try again or use Token Sign-In.", false);
#if CCORD_LIVE_ACCOUNT
    captcha_open_next_queued(a);
#endif
  } else if (!user_cancelled) {
#if CCORD_LIVE_ACCOUNT
    captcha_clear_queue(a, true);
#endif
  }
}

bool app_captcha_active(const app *a)
{
  return a && a->captcha_web != nullptr;
}

static bool captcha_is_animating(const app *a, uint64_t now_ms)
{
  if (!a || !a->captcha_web) return false;
  if (!a->captcha_widget_ready && !a->captcha_is_error && now_ms < a->captcha_open_ms + 8000u) return true;
  if (fabsf(a->captcha_well_h_cur - a->captcha_well_h_target) > 0.5f) return true;
  return false;
}

bool app_captcha_pump(app *a)
{
  if (!a || !a->captcha_web) return false;
#if CCORD_LIVE_ACCOUNT
  if (a->captcha_rest_id != 0 && !ccord_rest_captcha_pending(a->captcha_rest_id)) {
    captcha_destroy_active(a);
    captcha_open_next_queued(a);
    return true;
  }
#endif
  uint64_t now = app_now_ms(a);
  ccord_web_view_pump(a->captcha_web, now);
  bool redraw = ccord_web_view_needs_redraw(a->captcha_web);

  // Inspect embedded CAPTCHA widget state for loading reveal & adaptive checkbox <-> challenge sizing.
  if (!a->captcha_is_url_mode) {
    ccord_web_captcha_state cs = {0};
    ccord_web_view_captcha_state(a->captcha_web, &cs);
    if (cs.checkbox_ready && !a->captcha_widget_ready) {
      a->captcha_widget_ready = true;
      if (!a->captcha_is_error && !a->captcha_solved_banner) {
        snprintf(a->captcha_modal_status, sizeof a->captcha_modal_status,
                 "Complete the verification below to continue.");
      }
      redraw = true;
    }
    bool just_opened = cs.challenge_open && !a->captcha_challenge_open;
    if (cs.challenge_open != a->captcha_challenge_open) {
      a->captcha_challenge_open = cs.challenge_open;
      if (cs.challenge_open) {
        a->captcha_widget_ready = true;
        if (!a->captcha_is_error && !a->captcha_solved_banner) {
          snprintf(a->captcha_modal_status, sizeof a->captcha_modal_status,
                   "Complete the challenge below, then click Verify.");
        }
      }
      redraw = true;
    }
    float max_wh = captcha_max_well_h(a);
    float want_h = cs.challenge_open
                     ? fminf(max_wh, fmaxf(508.0f, cs.challenge_h + 24.0f))
                     : fminf(max_wh, CAPTCHA_WELL_COMPACT_H);
    if (fabsf(want_h - a->captcha_well_h_target) > 1.0f) {
      a->captcha_well_h_target = want_h;
      redraw = true;
    }
    // Snap immediately when the challenge opens so api.js measures the full well and centers the popup cleanly.
    if (just_opened) {
      a->captcha_well_h_cur = a->captcha_well_h_target;
      ccord_rect card = captcha_modal_card_rect(a);
      ccord_rect wr = captcha_web_rect(a, card);
      ccord_web_view_set_viewport(a->captcha_web, wr.w, wr.h);
      redraw = true;
    }
  }

  // Advance smooth well-height easing (and keep loading indicator animated while fetching).
  uint64_t dt_ms = now > a->captcha_last_anim_ms ? (now - a->captcha_last_anim_ms) : 0u;
  if (dt_ms > 100u) dt_ms = 100u;
  a->captcha_last_anim_ms = now;
  float diff = a->captcha_well_h_target - a->captcha_well_h_cur;
  if (fabsf(diff) > 0.5f) {
    float alpha = 1.0f - expf(-(float)dt_ms * 0.032f);
    a->captcha_well_h_cur += diff * alpha;
    if (fabsf(a->captcha_well_h_target - a->captcha_well_h_cur) <= 0.6f) {
      a->captcha_well_h_cur = a->captcha_well_h_target;
    }
    redraw = true;
  }
  if (captcha_is_animating(a, now)) redraw = true;

  if (a->captcha_has_solution) {
    if (a->captcha_test_mode) {
      a->captcha_has_solution = false;
      fprintf(stderr, "[ccord --test-captcha] SOLVED! token_len=%zu prefix=%.24s...\n",
              strlen(a->captcha_solved_token), a->captcha_solved_token);
      ccord_secure_wipe(a->captcha_solved_token, sizeof a->captcha_solved_token);
      return true;
    }
#if CCORD_LIVE_ACCOUNT
    if (a->captcha_rest_id != 0) {
      uint64_t rest_id = a->captcha_rest_id;
      char tok[CCORD_CAPTCHA_KEY_MAX];
      char rqt[CCORD_CAPTCHA_RQTOKEN_MAX];
      char sid[CCORD_CAPTCHA_SESSION_ID_MAX];
      snprintf(tok, sizeof tok, "%s", a->captcha_solved_token);
      snprintf(rqt, sizeof rqt, "%s", a->captcha_rest_rqtoken);
      snprintf(sid, sizeof sid, "%s", a->captcha_rest_session_id);
      captcha_destroy_active(a);
      ccord_captcha_solution sol = {
        .key = tok,
        .rqtoken = rqt[0] ? rqt : nullptr,
        .session_id = sid[0] ? sid : nullptr,
      };
      if (!ccord_rest_captcha_resolve(rest_id, &sol)) {
        (void)ccord_rest_captcha_cancel(rest_id);
      }
      ccord_secure_wipe(tok, sizeof tok);
      ccord_secure_wipe(rqt, sizeof rqt);
      ccord_secure_wipe(sid, sizeof sid);
      captcha_open_next_queued(a);
      return true;
    }
    char tok[CCORD_CAPTCHA_KEY_MAX];
    snprintf(tok, sizeof tok, "%s", a->captcha_solved_token);
    ccord_captcha_challenge ch = a->captcha_challenge;
    captcha_destroy_active(a);
    app_auth_retry_with_captcha(a, tok, ch.rqtoken, ch.session_id);
    ccord_live_wipe(tok, sizeof tok);
    ccord_live_wipe(&ch, sizeof ch);
#else
    captcha_destroy_active(a);
#endif
    return true;
  }
  return redraw;
}

uint32_t app_captcha_next_wait_ms(const app *a, uint64_t now_ms)
{
  if (!a || !a->captcha_web) return UINT32_MAX;
  if (captcha_is_animating(a, now_ms)) return 16u;
  return ccord_web_view_next_wait_ms(a->captcha_web, now_ms);
}

static void draw_native_btn(ccord_renderer *r, ccord_rect rc, const char *label,
                            bool hovered, bool pressed, float rad, float fs,
                            ccord_color bg_rest, ccord_color bg_hover, ccord_color bg_press,
                            ccord_color border, ccord_color fg)
{
  ccord_color fill = pressed ? bg_press : (hovered ? bg_hover : bg_rest);
  ccord_draw_rrect(r, rc, rad, fill);
  ccord_draw_rrect_stroke(r, rc, rad, 1.0f, border);
  ccord_text_style ts = {.font = CCORD_FONT_BOLD, .size = fs, .color = fg};
  ccord_vec2 m = ccord_text_measure(r, label, strlen(label), &ts, 0.0f);
  ccord_text_draw(r, label, strlen(label),
                  floorf(rc.x + (rc.w - m.x) * 0.5f),
                  floorf(rc.y + (rc.h - m.y) * 0.5f), &ts, 0.0f);
}

static void draw_loading_veil(const app *a, ccord_renderer *r, ccord_rect well, float r_surface,
                              ccord_color bg, ccord_color brand, ccord_color track, ccord_color muted, float fs_sm)
{
  ccord_draw_rrect(r, well, r_surface, bg);
  uint64_t elapsed = app_now_ms(a) - a->captcha_open_ms;
  float phase = fmodf((float)elapsed / 1200.0f, 1.0f);

  // Indeterminate blurple top progress bar inside well
  ccord_rect bar_track = {well.x + 12.0f, well.y + 10.0f, fmaxf(20.0f, well.w - 24.0f), 3.0f};
  ccord_draw_rrect(r, bar_track, 1.5f, track);
  float seg_w = bar_track.w * 0.34f;
  float travel = bar_track.w + seg_w;
  float sx = bar_track.x - seg_w + phase * travel;
  float x0 = fmaxf(bar_track.x, sx);
  float x1 = fminf(bar_track.x + bar_track.w, sx + seg_w);
  if (x1 > x0 + 1.0f) {
    ccord_draw_rrect(r, (ccord_rect){x0, bar_track.y, x1 - x0, bar_track.h}, 1.5f, brand);
  }

  // 3 softly pulsing dots
  float cx = well.x + well.w * 0.5f;
  float cy = well.y + well.h * 0.44f;
  for (int i = -1; i <= 1; i++) {
    float wave = 0.5f + 0.5f * sinf(((float)elapsed * 0.0065f) - (float)(i + 1) * 0.9f);
    uint8_t da = (uint8_t)(85.0f + wave * 170.0f);
    float rad = 3.5f + wave * 1.5f;
    ccord_draw_circle(r, cx + (float)i * 16.0f, cy, rad, c_alpha(brand, da));
  }

  const char *msg = "Connecting to verification service...";
  ccord_text_style ms = {.font = CCORD_FONT_REGULAR, .size = fs_sm, .color = muted};
  ccord_vec2 mm = ccord_text_measure(r, msg, strlen(msg), &ms, 0.0f);
  ccord_text_draw(r, msg, strlen(msg), floorf(cx - mm.x * 0.5f), floorf(cy + 16.0f), &ms, 0.0f);
}

static const char *captcha_subtitle(const app *a)
{
  if (!a) return "Discord requested a quick security check to continue.";
  if (a->captcha_is_url_mode) return a->captcha_url;
  if (a->captcha_rest_id == 0) return "Discord requested a quick security check to finish signing in.";
  const char *r = a->captcha_rest_route;
  if (strncmp(r, "/invites/", 9) == 0) return "Discord requested a security check to accept this invite.";
  if (strstr(r, "/relationships") != nullptr) return "Discord requested a security check to update this friend request.";
  if (strcmp(r, "/guilds") == 0) return "Discord requested a security check to create this server.";
  if (strncmp(r, "/users/@me", 10) == 0) return "Discord requested a security check to save your account changes.";
  if (strstr(r, "/messages") != nullptr) return "Discord requested a security check to send this message.";
  if (strstr(r, "/channels") != nullptr) return "Discord requested a security check to complete this channel action.";
  return "Discord requested a quick security check to continue.";
}

void app_captcha_draw(app *a)
{
  if (!a || !a->captcha_web || !a->renderer) return;
  ccord_renderer *r = a->renderer;
  ccord_vec2 vp = ccord_render_size(r);
  if (vp.x > 100.0f && vp.y > 100.0f) {
    a->viewport_w = vp.x;
    a->viewport_h = vp.y;
  }

  const ccord_theme *th = a->theme;
  ccord_color bg_primary   = th ? th->c.bg_primary      : (ccord_color){0x1e, 0x1f, 0x22, 255};
  ccord_color bg_secondary = th ? th->c.bg_secondary    : (ccord_color){0x2b, 0x2d, 0x31, 255};
  ccord_color bg_tertiary  = th ? th->c.bg_tertiary     : (ccord_color){0x14, 0x15, 0x18, 255};
  ccord_color bg_hover     = th ? th->c.bg_item_hover   : (ccord_color){0x35, 0x37, 0x3c, 255};
  ccord_color bg_press     = th ? th->c.bg_item_selected: (ccord_color){0x40, 0x42, 0x49, 255};
  ccord_color border       = th ? th->c.border_subtle   : (ccord_color){0x38, 0x3a, 0x40, 255};
  ccord_color brand        = th ? th->c.brand           : (ccord_color){0x58, 0x65, 0xf2, 255};
  ccord_color danger       = th ? th->c.danger          : (ccord_color){0xda, 0x37, 0x3c, 255};
  ccord_color ok_green     = th ? th->c.status_online   : (ccord_color){0x23, 0xa5, 0x5a, 255};
  ccord_color text_header  = th ? th->c.text_header     : (ccord_color){0xf2, 0xf3, 0xf5, 255};
  ccord_color text_sec     = th ? th->c.text_secondary  : (ccord_color){0xb5, 0xba, 0xc1, 255};
  ccord_color text_muted   = th ? th->c.text_muted      : (ccord_color){0x94, 0x9b, 0xa4, 255};
  ccord_color text_on_br   = th ? th->c.text_on_brand   : (ccord_color){0xff, 0xff, 0xff, 255};

  float r_surface = (th && th->m.composer_radius > 0.0f) ? th->m.composer_radius : 8.0f;
  float r_control = (th && th->m.channel_item_radius > 0.0f) ? th->m.channel_item_radius : 4.0f;
  float fs_title  = (th && th->m.font_size_large > 0.0f) ? th->m.font_size_large : 20.0f;
  float fs_ctrl   = (th && th->m.font_size_small > 0.0f) ? th->m.font_size_small : 14.0f;
  float fs_cap    = fmaxf(11.0f, fs_ctrl - 2.0f);

  // Scrim matching C-Cord's Login MFA popup (bg_tertiary @ 74%)
  ccord_draw_rect(r, (ccord_rect){0.0f, 0.0f, vp.x, vp.y}, c_alpha(bg_tertiary, 188u));

  ccord_rect card = captcha_modal_card_rect(a);

  // Soft 3-layer modal elevation shadow matching uis_shadow(..., UIS_ELEV_MODAL)
  static const uint8_t k_sh_a[3] = {18u, 26u, 38u};
  for (int i = 2; i >= 0; i--) {
    float exp_px = (float)(i + 1) * 3.0f;
    ccord_rect sh = {card.x - exp_px, card.y - exp_px + exp_px, card.w + 2.0f * exp_px, card.h + 2.0f * exp_px};
    ccord_draw_rrect(r, sh, r_surface + exp_px * 0.5f, (ccord_color){0, 0, 0, k_sh_a[2 - i]});
  }

  // 1px outset border ring + bg_secondary card surface (exact match to st_draw_login_screen)
  ccord_draw_rrect(r, (ccord_rect){card.x - 1.0f, card.y - 1.0f, card.w + 2.0f, card.h + 2.0f},
                   r_surface + 1.0f, border);
  ccord_draw_rrect(r, card, r_surface, bg_secondary);
  ccord_push_clip(r, card);

  // Top Discord blurple accent stripe
  ccord_draw_rrect(r, (ccord_rect){card.x, card.y, card.w, 6.0f}, 3.0f, brand);

  bool compact_hdr = captcha_use_compact_header(a);
  float pad_x = captcha_pad_x(a);

  // Subtle top-right close button (geometric X)
  ccord_rect xbtn = captcha_close_btn_rect(a, card);
  bool x_hov = pt_in_rect(a->captcha_mx, a->captcha_my, xbtn);
  bool x_prs = a->captcha_pressed_btn == 1 && x_hov;
  if (x_hov || x_prs) {
    ccord_draw_rrect(r, xbtn, r_control, x_prs ? bg_press : bg_hover);
  }
  ccord_color xcol = x_hov ? text_header : text_muted;
  float xcx = xbtn.x + xbtn.w * 0.5f, xcy = xbtn.y + xbtn.h * 0.5f;
  ccord_draw_line(r, (ccord_vec2){xcx - 4.5f, xcy - 4.5f}, (ccord_vec2){xcx + 4.5f, xcy + 4.5f}, 1.75f, xcol);
  ccord_draw_line(r, (ccord_vec2){xcx + 4.5f, xcy - 4.5f}, (ccord_vec2){xcx - 4.5f, xcy + 4.5f}, 1.75f, xcol);

  const char *title = a->captcha_is_url_mode ? "Native Web Preview" : "Verify you're human";
  ccord_rect banner;
  if (compact_hdr) {
    ccord_text_style ts = {.font = CCORD_FONT_BOLD, .size = fmaxf(15.0f, fs_title - 2.0f), .color = text_header};
    ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, 0.0f);
    ccord_text_draw(r, title, strlen(title), floorf(card.x + (card.w - tt.x) * 0.5f), card.y + 13.0f, &ts, 0.0f);
    banner = (ccord_rect){card.x + pad_x, card.y + 44.0f, card.w - 2.0f * pad_x, 32.0f};
  } else {
    // Centered VERIFICATION pill badge (matches login card's DISCORD pill)
    const char *badge_txt = a->captcha_is_url_mode ? "WEB PREVIEW" : "VERIFICATION";
    ccord_text_style bs = {.font = CCORD_FONT_BOLD, .size = fs_cap - 0.5f, .color = text_on_br};
    ccord_vec2 bt = ccord_text_measure(r, badge_txt, strlen(badge_txt), &bs, 0.0f);
    float badge_w = bt.x + 22.0f;
    ccord_rect badge = {floorf(card.x + (card.w - badge_w) * 0.5f), card.y + 14.0f, badge_w, 20.0f};
    ccord_draw_rrect(r, badge, 10.0f, brand);
    ccord_text_draw(r, badge_txt, strlen(badge_txt),
                    floorf(badge.x + (badge.w - bt.x) * 0.5f),
                    floorf(badge.y + (badge.h - bt.y) * 0.5f), &bs, 0.0f);

    // Centered title & subtitle (identical typography hierarchy to st_draw_login_screen / st_draw_mfa_popup)
    ccord_text_style ts = {.font = CCORD_FONT_BOLD, .size = fs_title, .color = text_header};
    ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, 0.0f);
    ccord_text_draw(r, title, strlen(title), floorf(card.x + (card.w - tt.x) * 0.5f), card.y + 40.0f, &ts, 0.0f);

    const char *sub = captcha_subtitle(a);
    ccord_text_style ss = {.font = CCORD_FONT_REGULAR, .size = fs_ctrl - 0.5f, .color = text_sec};
    ccord_vec2 st = ccord_text_measure(r, sub, strlen(sub), &ss, 0.0f);
    float sub_max_w = card.w - 2.0f * pad_x;
    float sub_x = st.x < sub_max_w ? floorf(card.x + (card.w - st.x) * 0.5f) : (card.x + pad_x);
    ccord_text_draw_ellipsis(r, sub, strlen(sub), sub_x, card.y + 68.0f, sub_max_w, &ss);

    banner = (ccord_rect){card.x + pad_x, card.y + 94.0f, card.w - 2.0f * pad_x, 34.0f};
  }

  // Status banner with 4px left accent bar (identical to login & MFA banners)
  ccord_color accent = a->captcha_is_error ? danger : (a->captcha_solved_banner ? ok_green : brand);
  ccord_draw_rrect(r, banner, r_control, bg_primary);
  ccord_draw_rrect(r, (ccord_rect){banner.x, banner.y, 4.0f, banner.h}, 2.0f, accent);
  const char *bmsg = a->captcha_modal_status[0] ? a->captcha_modal_status : "Loading verification challenge...";
  ccord_color bfg = a->captcha_is_error ? danger : (a->captcha_solved_banner ? ok_green : text_header);
  ccord_text_style bts = {.font = CCORD_FONT_BOLD, .size = fs_ctrl - 0.5f, .color = bfg};
  ccord_vec2 bm = ccord_text_measure(r, bmsg, strlen(bmsg), &bts, 0.0f);
  ccord_text_draw_ellipsis(r, bmsg, strlen(bmsg), banner.x + 12.0f,
                           floorf(banner.y + (banner.h - bm.y) * 0.5f), banner.w - 20.0f, &bts);

  // Seamless theme-matched recessed verification well
  ccord_rect well = captcha_well_rect(a, card);
  ccord_rect wr = captcha_web_rect(a, card);
  ccord_draw_rrect(r, well, r_surface, bg_primary);
  ccord_web_view_draw(a->captcha_web, r, wr);

  // Native animated loading veil while api.js + checkbox iframe are still bootstrapping
  uint64_t now = app_now_ms(a);
  if (!a->captcha_is_url_mode && !a->captcha_widget_ready && !a->captcha_is_error &&
      now < a->captcha_open_ms + 8000u) {
    draw_loading_veil(a, r, well, r_surface, bg_primary, brand, c_mix(bg_primary, border, 0.7f), text_muted,
                      fs_ctrl - 1.0f);
  }
  ccord_draw_rrect_stroke(r, well, r_surface, 1.0f, border);

  // Footer action buttons: Reload + Back to Login
  ccord_rect rbtn = captcha_reload_btn_rect(a, card);
  bool r_hov = pt_in_rect(a->captcha_mx, a->captcha_my, rbtn);
  draw_native_btn(r, rbtn, "Reload", r_hov, a->captcha_pressed_btn == 2 && r_hov,
                  r_control, fs_ctrl - 0.5f, bg_tertiary, bg_hover, bg_press, border, text_header);

  ccord_rect cbtn = captcha_cancel_btn_rect(a, card);
  bool c_hov = pt_in_rect(a->captcha_mx, a->captcha_my, cbtn);
  const char *cancel_lbl = a->captcha_is_url_mode ? "Close" : (a->captcha_rest_id != 0 ? "Cancel" : "Back to Login");
  draw_native_btn(r, cbtn, cancel_lbl, c_hov, a->captcha_pressed_btn == 3 && c_hov,
                  r_control, fs_ctrl - 0.5f, bg_tertiary, bg_hover, bg_press, border, text_header);

  ccord_pop_clip(r);
}

static void captcha_reload(app *a)
{
  if (!a) return;
  if (a->captcha_is_url_mode && a->captcha_url[0]) {
    (void)app_captcha_open_url(a, a->captcha_url);
  } else if (a->captcha_sitekey[0]) {
    uint64_t rest_id = a->captcha_rest_id;
    char rqt[2048], sid[128], route[128];
    snprintf(rqt, sizeof rqt, "%s", a->captcha_rest_rqtoken);
    snprintf(sid, sizeof sid, "%s", a->captcha_rest_session_id);
    snprintf(route, sizeof route, "%s", a->captcha_rest_route);
    bool ok = app_captcha_open_ex(a, a->captcha_service, a->captcha_sitekey,
                                  a->captcha_host[0] ? a->captcha_host : "discord.com",
                                  a->captcha_rqdata[0] ? a->captcha_rqdata : nullptr, a->captcha_test_mode);
    if (ok && rest_id != 0) {
      a->captcha_rest_id = rest_id;
      snprintf(a->captcha_rest_rqtoken, sizeof a->captcha_rest_rqtoken, "%s", rqt);
      snprintf(a->captcha_rest_session_id, sizeof a->captcha_rest_session_id, "%s", sid);
      snprintf(a->captcha_rest_route, sizeof a->captcha_rest_route, "%s", route);
    } else if (!ok && rest_id != 0) {
#if CCORD_LIVE_ACCOUNT
      (void)ccord_rest_captcha_cancel(rest_id);
      captcha_open_next_queued(a);
#endif
    }
    ccord_secure_wipe(rqt, sizeof rqt);
    ccord_secure_wipe(sid, sizeof sid);
  }
}

bool app_captcha_handle_input(app *a, const ccord_input_event *ev)
{
  if (!a || !a->captcha_web || !ev) return false;
  if (ev->type == CCORD_IN_KEY_DOWN && (ev->key == CCORD_KEY_ESCAPE || ev->key == CCORD_KEY_BACK)) {
    app_captcha_close(a, true);
    a->media_redraw = true;
    return true;
  }
  ccord_rect card = captcha_modal_card_rect(a);
  ccord_rect xbtn = captcha_close_btn_rect(a, card);
  ccord_rect rbtn = captcha_reload_btn_rect(a, card);
  ccord_rect cbtn = captcha_cancel_btn_rect(a, card);

  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    bool prev_any = pt_in_rect(a->captcha_mx, a->captcha_my, xbtn) ||
                    pt_in_rect(a->captcha_mx, a->captcha_my, rbtn) ||
                    pt_in_rect(a->captcha_mx, a->captcha_my, cbtn);
    a->captcha_mx = ev->x;
    a->captcha_my = ev->y;
    bool now_any = pt_in_rect(ev->x, ev->y, xbtn) ||
                   pt_in_rect(ev->x, ev->y, rbtn) ||
                   pt_in_rect(ev->x, ev->y, cbtn);
    if (prev_any || now_any) a->media_redraw = true;
  } else if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    a->captcha_mx = ev->x;
    a->captcha_my = ev->y;
    if (pt_in_rect(ev->x, ev->y, xbtn)) { a->captcha_pressed_btn = 1; a->media_redraw = true; return true; }
    if (pt_in_rect(ev->x, ev->y, rbtn)) { a->captcha_pressed_btn = 2; a->media_redraw = true; return true; }
    if (pt_in_rect(ev->x, ev->y, cbtn)) { a->captcha_pressed_btn = 3; a->media_redraw = true; return true; }
    a->captcha_pressed_btn = 0;
  } else if (ev->type == CCORD_IN_MOUSE_UP && ev->button == 1) {
    int p = a->captcha_pressed_btn;
    a->captcha_pressed_btn = 0;
    a->media_redraw = true;
    if (p == 1 && pt_in_rect(ev->x, ev->y, xbtn)) { app_captcha_close(a, true); return true; }
    if (p == 2 && pt_in_rect(ev->x, ev->y, rbtn)) { captcha_reload(a); return true; }
    if (p == 3 && pt_in_rect(ev->x, ev->y, cbtn)) { app_captcha_close(a, true); return true; }
  } else if (ev->type == CCORD_IN_TOUCH_DOWN) {
    if (!a->captcha_touch_btn_active) {
      int hit = pt_in_rect(ev->x, ev->y, xbtn) ? 1
              : pt_in_rect(ev->x, ev->y, rbtn) ? 2
              : pt_in_rect(ev->x, ev->y, cbtn) ? 3 : 0;
      if (hit != 0) {
        a->captcha_touch_btn_active = true;
        a->captcha_touch_btn_finger = ev->finger_id;
        a->captcha_pressed_btn = hit;
        a->captcha_mx = ev->x;
        a->captcha_my = ev->y;
        a->media_redraw = true;
        return true;
      }
    }
  } else if (ev->type == CCORD_IN_TOUCH_MOVE) {
    if (a->captcha_touch_btn_active && ev->finger_id == a->captcha_touch_btn_finger) {
      a->captcha_mx = ev->x;
      a->captcha_my = ev->y;
      a->media_redraw = true;
      return true;
    }
  } else if (ev->type == CCORD_IN_TOUCH_UP) {
    if (a->captcha_touch_btn_active && ev->finger_id == a->captcha_touch_btn_finger) {
      int p = a->captcha_pressed_btn;
      a->captcha_touch_btn_active = false;
      a->captcha_pressed_btn = 0;
      a->captcha_mx = -1.0f;
      a->captcha_my = -1.0f;
      a->media_redraw = true;
      if (p == 1 && pt_in_rect(ev->x, ev->y, xbtn)) { app_captcha_close(a, true); return true; }
      if (p == 2 && pt_in_rect(ev->x, ev->y, rbtn)) { captcha_reload(a); return true; }
      if (p == 3 && pt_in_rect(ev->x, ev->y, cbtn)) { app_captcha_close(a, true); return true; }
      return true;
    }
  } else if (ev->type == CCORD_IN_TOUCH_CANCEL) {
    if (a->captcha_touch_btn_active && ev->finger_id == a->captcha_touch_btn_finger) {
      a->captcha_touch_btn_active = false;
      a->captcha_pressed_btn = 0;
      a->captcha_mx = -1.0f;
      a->captcha_my = -1.0f;
      a->media_redraw = true;
    }
  }

  ccord_rect wr = captcha_web_rect(a, card);
  if (a->captcha_widget_ready || a->captcha_is_url_mode) {
    (void)ccord_web_view_handle_input(a->captcha_web, wr, ev);
  }
  // Modal blocks all mouse, touch, wheel, and keyboard input from reaching widgets behind it
  return ev->type == CCORD_IN_MOUSE_MOVE || ev->type == CCORD_IN_MOUSE_DOWN ||
         ev->type == CCORD_IN_MOUSE_UP || ev->type == CCORD_IN_WHEEL ||
         ev->type == CCORD_IN_TOUCH_DOWN || ev->type == CCORD_IN_TOUCH_MOVE ||
         ev->type == CCORD_IN_TOUCH_UP || ev->type == CCORD_IN_TOUCH_CANCEL ||
         ev->type == CCORD_IN_KEY_DOWN || ev->type == CCORD_IN_TEXT;
}

#else

void app_captcha_register_rest_handler(app *a) { (void)a; }
void app_captcha_unregister_rest_handler(app *a) { (void)a; }
bool app_captcha_open(app *a, const char *service, const char *sitekey, const char *rqdata, bool test_mode)
{
  (void)a; (void)service; (void)sitekey; (void)rqdata; (void)test_mode;
  return false;
}
bool app_captcha_open_ex(app *a, const char *service, const char *sitekey, const char *host,
                         const char *rqdata, bool test_mode)
{
  (void)a; (void)service; (void)sitekey; (void)host; (void)rqdata; (void)test_mode;
  return false;
}
bool app_captcha_open_url(app *a, const char *url) { (void)a; (void)url; return false; }
void app_captcha_close(app *a, bool user_cancelled) { (void)a; (void)user_cancelled; }
bool app_captcha_active(const app *a) { (void)a; return false; }
bool app_captcha_pump(app *a) { (void)a; return false; }
uint32_t app_captcha_next_wait_ms(const app *a, uint64_t now_ms) { (void)a; (void)now_ms; return UINT32_MAX; }
void app_captcha_draw(app *a) { (void)a; }
bool app_captcha_handle_input(app *a, const ccord_input_event *ev) { (void)a; (void)ev; return false; }

#endif
