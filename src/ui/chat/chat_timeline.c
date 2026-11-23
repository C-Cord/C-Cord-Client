// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "chat_internal.h"


float dc_content_wrap(const ccord_ui *ui, float width)
{
  return maxf(width - M(message_text_x) - M(message_pad_x), 40.0f);
}

// The newest message gets breathing room above the composer (like the real client's scroller spacer).
float dc_tail_pad(const ccord_ui *ui, size_t i)
{
  return i + 1 == ui->vlist.count ? M(message_group_gap) : 0.0f;
}

bool dc_message_edited(const ccord_message *m)
{
  return m && (m->edited || m->edited_timestamp_ms != 0);
}

// ---- chat motion -------------------------------------------------------------------------------------------------
// Only drawing moves here: row heights, the layout and every other row's y stay as measured.

#define CHAT_SEEN_PROBE 16u

static size_t chat_seen_bucket(uint64_t key)
{
  return (size_t)((key * 0x9E3779B97F4A7C15ULL) >> 56) & (UI_CHAT_SEEN_MAX - 1u);
}

static ui_chat_seen *chat_seen_peek(ccord_ui *ui, uint64_t key)
{
  if (key == 0) return nullptr;
  size_t base = chat_seen_bucket(key);
  const size_t mask = UI_CHAT_SEEN_MAX - 1u;
  for (size_t i = 0; i < CHAT_SEEN_PROBE; i++) {
    ui_chat_seen *e = &ui->chat_seen[(base + i) & mask];
    if (e->key == key) return e;
  }
  return nullptr;
}

static ui_chat_seen *chat_seen_find(ccord_ui *ui, uint64_t key)
{
  ui_chat_seen *e = chat_seen_peek(ui, key);
  if (e) e->stamp = ui->chat_seq;
  return e;
}

// Takes a free slot in the probe window, else the least recently drawn one that was not drawn this frame, so a frame's
// own rows and pills keep their memory. Nullptr only when every probed slot was drawn this frame.
static ui_chat_seen *chat_seen_put(ccord_ui *ui, uint64_t key)
{
  if (key == 0) return nullptr;
  size_t base = chat_seen_bucket(key);
  const size_t mask = UI_CHAT_SEEN_MAX - 1u;
  ui_chat_seen *victim = nullptr;
  for (size_t i = 0; i < CHAT_SEEN_PROBE; i++) {
    ui_chat_seen *e = &ui->chat_seen[(base + i) & mask];
    if (e->key == 0) {
      victim = e;
      break;
    }
    if (e->stamp >= ui->chat_seq) continue;
    if (!victim || e->stamp < victim->stamp) victim = e;
  }
  if (!victim) return nullptr;
  memset(victim, 0, sizeof *victim);
  victim->key = key;
  victim->stamp = ui->chat_seq;
  return victim;
}

// Strength of the jump highlight: full for UI_JUMP_HOLD_MS, then an eased fade to 0 at UI_JUMP_FLASH_MS.
// Reduced motion shows it at full strength and then drops it at once.
static float jump_flash_strength(const ccord_ui *ui)
{
  if (ui->now_ms >= ui->jump_highlight_until_ms) return 0.0f;
  if (ui->anim.reduced_motion) return 1.0f;
  float left = (float)(ui->jump_highlight_until_ms - ui->now_ms);
  float elapsed = (float)UI_JUMP_FLASH_MS - left;
  if (elapsed <= (float)UI_JUMP_HOLD_MS) return 1.0f;
  float t = (elapsed - (float)UI_JUMP_HOLD_MS) / ((float)UI_JUMP_FLASH_MS - (float)UI_JUMP_HOLD_MS);
  return 1.0f - ccord_ease_out(clampf(t, 0.0f, 1.0f));
}

// A pill that appears on a row that was on screen last frame pops in, including one taken off and put back while its
// row stayed on screen; a count change on such a row ticks once. Anything else (history, a row scrolled back into view)
// is recorded silently. Pills are walked on every frame their row is drawn, so the stamps tell which frame saw what.
static void reaction_pill_motion(ccord_ui *ui, const ccord_message *m, uint64_t pid, unsigned count, float *pop,
                                 float *tick)
{
  *pop = 1.0f;
  *tick = 1.0f;
  const ui_chat_seen *me = chat_seen_peek(ui, m->id);
  const ui_chat_seen *pe = chat_seen_peek(ui, pid);
  bool shown = me && me->stamp + 1 == ui->chat_seq;
  bool pill_was_shown = pe && pe->stamp + 1 == ui->chat_seq;
  bool pop_now = false, tick_now = false;
  ui_chat_seen *e = chat_seen_find(ui, pid);
  if (!e) {
    e = chat_seen_put(ui, pid);
    if (!e) return;   // every slot was drawn this frame: this pill gets no motion
    pop_now = shown;
  } else if (!pill_was_shown && shown) {
    pop_now = true;
  } else if (e->count != count) {
    tick_now = shown;
  }
  e->count = count;
  if (pop_now) {
    e->event = UI_CHAT_EV_NEW;
    e->event_ms = ui->now_ms;
    ccord_anim_reset(&ui->anim, ccord_anim_key("reactpop", pid));
  } else if (tick_now) {
    e->event = UI_CHAT_EV_TICK;
    e->event_ms = ui->now_ms;
    ccord_anim_reset(&ui->anim, ccord_anim_key("reacttick", pid));
  }
  if (e->event == UI_CHAT_EV_NEW) {
    *pop = ui->now_ms < e->event_ms + UI_REACT_POP_MS ? ui_enter(ui, "reactpop", pid, UI_REACT_POP_MS) : 1.0f;
    ui->dbg_reaction_pop = *pop;
  } else if (e->event == UI_CHAT_EV_TICK && ui->now_ms < e->event_ms + UI_REACT_TICK_MS) {
    *tick = ui_enter(ui, "reacttick", pid, UI_REACT_TICK_MS);
  }
}

bool dc_message_has_reactions(const ccord_message *m)
{
  if (!m || !m->reactions || m->reaction_kind_count == 0) return false;
  for (size_t i = 0; i < m->reaction_kind_count; i++) {
    if (m->reactions[i].emoji && m->reactions[i].count > 0) return true;
  }
  return false;
}

static float reaction_badge_h(const ccord_ui *ui)
{
  return (ui && ui->mobile_layout) ? 34.0f : (UIS_H_CHIP + 2.0f);
}

typedef struct reaction_pill {
  const char *emoji;
  size_t elen;
  ccord_id emoji_id;
  bool animated;
  unsigned count;
  uint32_t burst_count;
  bool mine;
  bool me_burst;
} reaction_pill;

static void reaction_pill_fill(const ccord_reaction *r, reaction_pill *out)
{
  out->emoji = r->emoji;
  out->elen = strlen(r->emoji);
  out->emoji_id = r->emoji_id;
  out->animated = r->animated;
  if (out->emoji_id == CCORD_ID_NONE) {
    const char *colon = strrchr(r->emoji, ':');
    if (colon && colon > r->emoji && colon[1] != '\0') {
      ccord_id parsed = CCORD_ID_NONE;
      if (ccord_id_parse(colon + 1, strlen(colon + 1), &parsed)) {
        out->emoji_id = parsed;
      }
    }
  }
  out->count = r->count;
  out->burst_count = r->burst_count;
  out->me_burst = r->me_burst;
  out->mine = r->self_reacted || r->me_burst;
}

// One pill per emoji (emoji + count), wrapping to new rows. Returns total height; draws when asked.
float dc_reaction_pills_walk(ccord_ui *ui, ccord_renderer *r, const ccord_message *m, float x, float y,
                                 float wrap, bool draw)
{
  if (!dc_message_has_reactions(m)) return 0.0f;
  float bh = reaction_badge_h(ui);
  float gap_x = 6.0f, gap_y = 4.0f;
  float cx = x, cy = y;
  bool any = false;
  for (size_t ri = 0; ri < m->reaction_kind_count; ri++) {
    const ccord_reaction *rc = &m->reactions[ri];
    if (!rc->emoji || rc->count == 0) continue;
    reaction_pill p;
    reaction_pill_fill(rc, &p);
    char label[24];
    int ll = snprintf(label, sizeof label, "%u", p.count);
    if (ll < 0) ll = 0;
    bool is_custom = (p.emoji_id != CCORD_ID_NONE);
    bool is_uemoji = !is_custom && p.elen > 0 && ccord_emoji_match_prefix(p.emoji, p.elen) == p.elen;
    ccord_text_style es = ui_style(ui, is_uemoji ? CCORD_FONT_EMOJI : CCORD_FONT_REGULAR,
                                   is_uemoji ? M(font_size_small) : (M(font_size_small) - 2.0f),
                                   p.mine ? COL(text_on_brand) : COL(text_normal));
    ccord_text_style cs = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 3.0f,
                                    p.mine ? COL(text_on_brand) : COL(text_secondary));
    size_t custom_nlen = p.elen;
    if (is_custom) {
      const char *colon = strrchr(p.emoji, ':');
      custom_nlen = colon ? (size_t)(colon - p.emoji) : p.elen;
    }
    float ew = is_custom ? (bh - 6.0f) : ccord_text_measure(r, p.emoji, p.elen, &es, 0.0f).x;
    float cw = ccord_text_measure(r, label, (size_t)ll, &cs, 0.0f).x;
    float pw = maxf(ew + cw + 20.0f, ui->mobile_layout ? 42.0f : 34.0f);
    if (any && cx > x && cx + pw > x + wrap) {
      cx = x;
      cy += bh + gap_y;
    }
    if (draw) {
      ccord_rect pr = {cx, cy, pw, bh};
      // A pill eases into "mine" when you react and out when you take it back, and warms up under the pointer.
      uint64_t pid = m->id ^ (ui_name_id(p.emoji) * 31u);
      float mf = ui_fade(ui, "reactm", pid, p.mine);
      float hf = ui_fade(ui, "reacth", pid, ui->mouse_in && ui_in_rect(ui->mx, ui->my, pr)) * (1.0f - mf);
      float pop, tick;
      reaction_pill_motion(ui, m, pid, p.count, &pop, &tick);
      if (pop < 1.0f) {
        ccord_push_alpha(r, pop);
        ccord_push_offset(r, 0.0f, (1.0f - pop) * 4.0f);
      }
      if (p.burst_count > 0) {
        ccord_color gold = (ccord_color){245, 178, 50, 235};
        ccord_draw_rrect(r, (ccord_rect){pr.x - 1.0f, pr.y - 1.0f, pr.w + 2.0f, pr.h + 2.0f}, bh * 0.5f + 1.0f, gold);
        ccord_color base_bg = ui_mix(COL(bg_tertiary), (ccord_color){70, 52, 24, 255}, p.me_burst ? 0.75f : 0.4f);
        ccord_draw_rrect(r, pr, bh * 0.5f, ui_mix(base_bg, COL(bg_item_hover), hf));
        es.color = COL(text_header);
        cs.color = gold;
        ccord_draw_circle(r, pr.x + pr.w - 4.0f, pr.y + 4.0f, 2.2f, gold);
        ui->dbg_burst_reaction_pills++;
      } else {
        uis_chip(ui, r, pr, "", (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, pr), .selected = p.mine},
                 "reactchip", pid);
        es.color = ui_mix(COL(text_normal), COL(text_on_brand), mf);
        cs.color = ui_mix(COL(text_secondary), COL(text_on_brand), mf);
      }
      ccord_rect emoji_box;
      if (is_custom) {
        ccord_rect er = {cx + 6.0f, cy + 3.0f, ew, ew};
        ui_expr_draw_custom_emoji(ui, r, p.emoji_id, p.emoji, custom_nlen, p.animated, er);
        emoji_box = er;
      } else {
        ccord_vec2 et = ccord_text_measure(r, p.emoji, p.elen, &es, 0.0f);
        ccord_text_draw(r, p.emoji, p.elen, cx + 7.0f, cy + (bh - et.y) * 0.5f, &es, 0.0f);
        emoji_box = (ccord_rect){cx + 7.0f, cy + (bh - et.y) * 0.5f, et.x, et.y};
      }
      if (ui->ndbg_reaction_draws < UI_REACTION_DRAW_MAX) {
        ui->dbg_reaction_draws[ui->ndbg_reaction_draws++] =
            (ui_reaction_draw){.message_id = m->id, .pill = pr, .emoji = emoji_box, .custom = is_custom,
                               .burst = p.burst_count > 0};
      }
      if (tick < 1.0f) ccord_push_offset(r, 0.0f, -(1.0f - tick) * 3.0f);   // the count nudges up and settles
      ccord_text_draw(r, label, (size_t)ll, cx + 9.0f + ew, cy + (bh - ui_line_h(ui, cs.size)) * 0.5f, &cs, 0.0f);
      if (tick < 1.0f) ccord_pop_offset(r);
      if (pop < 1.0f) {
        ccord_pop_offset(r);
        ccord_pop_alpha(r);
      }
      if (ui->nreaction_hits < UI_REACTION_HIT_MAX && p.elen < sizeof ui->reaction_hits[0].emoji) {
        ui_reaction_hit *hit = &ui->reaction_hits[ui->nreaction_hits++];
        hit->rect = pr;
        hit->channel_id = m->channel_id;
        hit->message_id = m->id;
        memcpy(hit->emoji, p.emoji, p.elen);
        hit->emoji[p.elen] = '\0';
        hit->is_add_btn = false;
      }
    }
    cx += pw + gap_x;
    any = true;
  }
  if (any && draw && ui->nreaction_hits < UI_REACTION_HIT_MAX) {
    float add_w = ui->mobile_layout ? 34.0f : 26.0f;
    if (cx + add_w <= x + wrap) {
      ccord_rect ar = {cx, cy, add_w, bh};
      uis_chip(ui, r, ar, "+", (uis_state){.hover = ui->mouse_in && ui_in_rect(ui->mx, ui->my, ar)}, "reactadd", m->id);
      ui_reaction_hit *hit = &ui->reaction_hits[ui->nreaction_hits++];
      hit->rect = ar;
      hit->channel_id = m->channel_id;
      hit->message_id = m->id;
      hit->emoji[0] = '\0';
      hit->is_add_btn = true;
    }
  }
  return any ? cy - y + bh : 0.0f;
}


static void draw_chat_divider(ccord_ui *ui, ccord_renderer *r, float x, float y, float w,
                              int64_t ts_ms, bool has_date, bool has_new)
{
  float cy = y + UI_CHAT_DIVIDER_H * 0.5f + 2.0f;
  ccord_color line_c = has_new ? COL(danger) : COL(border_subtle);
  ccord_draw_rect(r, (ccord_rect){x, cy, w, 1.0f}, line_c);
  if (has_new) {
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
    ccord_vec2 nt = ccord_text_measure(r, "NEW", 3, &ns, 0.0f);
    ccord_rect pill = {x + w - (nt.x + 10.0f), cy - 7.5f, nt.x + 10.0f, 15.0f};
    ccord_draw_rrect(r, pill, 3.0f, COL(danger));
    ccord_text_draw(r, "NEW", 3, pill.x + 5.0f, pill.y + (pill.h - nt.y) * 0.5f, &ns, 0.0f);
  }
  if (has_date && ts_ms > 0) {
    char dbuf[48];
    size_t dl = ccord_timefmt_date(dbuf, sizeof dbuf, ts_ms, ui->use_utc);
    if (dl > 0) {
      ccord_text_style ds = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f,
                                     has_new ? COL(danger) : COL(text_muted));
      ccord_vec2 dt = ccord_text_measure(r, dbuf, dl, &ds, 0.0f);
      ccord_rect bg = {x + (w - (dt.x + 16.0f)) * 0.5f, cy - (dt.y + 4.0f) * 0.5f, dt.x + 16.0f, dt.y + 4.0f};
      ccord_draw_rect(r, bg, COL(bg_primary));
      ccord_text_draw(r, dbuf, dl, bg.x + 8.0f, bg.y + 2.0f, &ds, 0.0f);
    }
  }
}

bool dc_is_dm_intro_row(const ccord_ui *ui, ccord_id channel_id, size_t store_i)
{
  if (store_i != 0) return false;
  const ccord_channel *ch = ccord_store_channel(ui->d.store, channel_id);
  if (!ch || ch->guild_id != CCORD_DM_GUILD_ID) return false;
  if (ui->d.session && ccord_session_history_has_more(ui->d.session, channel_id)) return false;
  return true;
}

static void draw_dm_intro(ccord_ui *ui, ccord_renderer *r, const ccord_channel *ch, float x, float y, float w)
{
  if (!ch) return;
  const ccord_user *u = ch->user_id != CCORD_ID_NONE ? ccord_store_user(ui->d.store, ch->user_id) : nullptr;
  const char *dname = (u && u->display_name && u->display_name[0]) ? u->display_name : ch->name;
  const char *uname = (u && u->handle && u->handle[0]) ? u->handle
                      : ((u && u->name && u->name[0]) ? u->name : ch->name);
  ccord_user dm_u = {
    .id = u ? u->id : (ch->user_id != CCORD_ID_NONE ? ch->user_id : ch->id),
    .name = dname,
    .avatar_url = (u && u->avatar_url && u->avatar_url[0]) ? u->avatar_url : (ch->icon_url ? ch->icon_url : ""),
    .avatar_decoration_url = (u && u->avatar_decoration_url) ? u->avatar_decoration_url : "",
    .presence = u ? u->presence : CCORD_PRESENCE_OFFLINE,
    .avatar_palette = u ? u->avatar_palette : (uint8_t)(ch->id % 6),
  };
  ui_draw_avatar(ui, r, &dm_u, x + 40.0f, y + 48.0f, 80.0f, (ccord_color){0, 0, 0, 0}, false);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 10.0f, COL(text_header));
  float dname_w = ccord_text_draw_ellipsis(r, dname, strlen(dname), x, y + 96.0f, w, &ts);
  ccord_id dm_uid = u ? u->id : ch->user_id;
  if (!ui_channel_is_group_dm(ch) && dm_uid != CCORD_ID_NONE) {
    dc_record_user_hit(ui, (ccord_rect){x, y + 8.0f, 80.0f, 80.0f}, dm_uid);
    dc_record_user_hit(ui, (ccord_rect){x, y + 96.0f, maxf(dname_w, 4.0f), ui_line_h(ui, ts.size)}, dm_uid);
  }
  char handle[160];
  snprintf(handle, sizeof handle, "@%s", uname);
  ccord_text_style hs = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_secondary));
  ccord_text_draw_ellipsis(r, handle, strlen(handle), x, y + 124.0f, w, &hs);
  char sub[256];
  snprintf(sub, sizeof sub, "This is the beginning of your direct message history with @%s.", uname);
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_muted));
  ccord_text_draw_ellipsis(r, sub, strlen(sub), x, y + 148.0f, w, &ss);
  ccord_draw_rect(r, (ccord_rect){x, y + UI_DM_INTRO_H - 8.0f, w, 1.0f}, COL(border_subtle));
}


// M0-3 local echoes: a pending row is dimmed and carries a clock/status line, a failed row is tinted and
// exposes a Retry button. Both are visibly unconfirmed; confirmed messages draw exactly as before.
static bool message_send_unconfirmed(const ccord_message *m)
{
  return m && m->send_state != CCORD_MSG_SEND_CONFIRMED;
}

float dc_send_status_extra_h(const ccord_ui *ui, const ccord_message *m)
{
  return message_send_unconfirmed(m) ? M(message_line_gap) * 0.5f + UI_SEND_STATUS_H : 0.0f;
}

static void draw_send_status_clock(ccord_renderer *r, float cx, float cy, float rad, ccord_color c, ccord_color bg)
{
  ccord_draw_circle(r, cx, cy, rad, c);
  ccord_draw_circle(r, cx, cy, rad * 0.74f, bg);
  ccord_draw_line(r, (ccord_vec2){cx, cy}, (ccord_vec2){cx, cy - rad * 0.48f}, 1.4f, c);
  ccord_draw_line(r, (ccord_vec2){cx, cy}, (ccord_vec2){cx + rad * 0.42f, cy + rad * 0.15f}, 1.4f, c);
}

static void draw_send_status(ccord_ui *ui, ccord_renderer *r, const ccord_message *m, float x, float y)
{
  if (!message_send_unconfirmed(m)) return;
  float size = M(font_size_small) - 2.0f;
  float ty = y + (UI_SEND_STATUS_H - ui_line_h(ui, size)) * 0.5f;
  if (m->send_state == CCORD_MSG_SEND_PENDING) {
    draw_send_status_clock(r, x + 6.0f, y + UI_SEND_STATUS_H * 0.5f, 5.5f, COL(text_muted), COL(bg_primary));
    ccord_text_style ps = ui_style(ui, CCORD_FONT_REGULAR, size, COL(text_muted));
    ccord_text_draw(r, "Sending...", 10, x + 16.0f, ty, &ps, 0.0f);
    ui->dbg_pending_echoes++;
    return;
  }
  ccord_text_style fs = ui_style(ui, CCORD_FONT_REGULAR, size, COL(danger));
  ccord_text_draw(r, "Failed to send", 14, x, ty, &fs, 0.0f);
  float fw = ccord_text_measure(r, "Failed to send", 14, &fs, 0.0f).x;
  ccord_text_style rs = ui_style(ui, CCORD_FONT_BOLD, size, COL(danger));
  ccord_vec2 rt = ccord_text_measure(r, "Retry", 5, &rs, 0.0f);
  ccord_rect pill = {x + fw + 10.0f, y, rt.x + 16.0f, UI_SEND_STATUS_H};
  bool hov = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, pill);
  ccord_draw_rrect(r, pill, 4.0f, hov ? COL(bg_item_hover) : COL(bg_tertiary));
  ccord_text_draw(r, "Retry", 5, pill.x + 8.0f, y + (UI_SEND_STATUS_H - rt.y) * 0.5f, &rs, 0.0f);
  if (ui->nretry_hits < UI_RETRY_HIT_MAX) {
    ui->retry_hits[ui->nretry_hits].rect = pill;
    ui->retry_hits[ui->nretry_hits].channel_id = m->channel_id;
    ui->retry_hits[ui->nretry_hits].message_id = m->id;
    ui->nretry_hits++;
  }
  ui->dbg_failed_echoes++;
}

static void draw_row(msg_ctx *c, size_t i, float y, float h, const ccord_rect *ma,
                     ccord_rect *out_avatar_rect, ccord_rect *out_author_rect)
{
  ccord_ui *ui = c->ui;
  ccord_renderer *r = c->r;
  if (out_avatar_rect) *out_avatar_rect = (ccord_rect){0, 0, 0, 0};
  if (out_author_rect) *out_author_rect = (ccord_rect){0, 0, 0, 0};
  size_t store_i = 0;
  const ccord_message *m = dc_view_message_at(ui, c->channel, i, &store_i);
  if (!m) return;
  if (dc_is_dm_intro_row(ui, c->channel, store_i)) {
    draw_dm_intro(ui, r, ccord_store_channel(ui->d.store, c->channel),
                  ma->x + M(message_pad_x), y, ma->w - 2.0f * M(message_pad_x));
    y += UI_DM_INTRO_H;
    h -= UI_DM_INTRO_H;
  }
  bool has_date = store_i != SIZE_MAX && ui_row_has_date_divider(ui, c->channel, store_i, m);
  bool has_new = store_i != SIZE_MAX && ui_row_has_new_divider(ui, c->channel, store_i, m);
  if (has_date || has_new) {
    draw_chat_divider(ui, r, ma->x + M(message_pad_x), y, ma->w - 2.0f * M(message_pad_x),
                      m->timestamp_ms, has_date, has_new);
    if (has_date) ui->dbg_date_dividers++;
    if (has_new) ui->dbg_new_dividers++;
    y += UI_CHAT_DIVIDER_H;
    h -= UI_CHAT_DIVIDER_H;
  }
  blocked_run br = dc_blocked_run_at(ui, c->channel, store_i, m);
  if (br.blocked) {
    if (br.first) {
      dc_draw_blocked_strip(ui, r, ma, y, &br);
      y += UI_BLOCKED_ROW_H;
      h -= UI_BLOCKED_ROW_H;
    }
    if (!br.revealed) return;
    ui->dbg_blocked_text_drawn++;
  }
  bool compact = store_i != SIZE_MAX && ui_is_compact(ui, c->channel, store_i, m);
  decor_info d;
  dc_decor_scan(ui, m, compact, &d);
  bool menu_target = ui->menu.open && ui->menu.ctx.target == CCORD_MENU_MESSAGE && ui->menu.ctx.message_id == m->id;
  bool hover = menu_target || (m->id == ui->hover_message_id && ui->mouse_in && !ui->settings_open && !ui->menu.open);
  float lh = ui_line_h(ui, M(font_size_base));
  bool dense = ui_messages_compact(ui);
  float top = compact ? M(message_line_gap)
                      : (dense ? M(message_line_gap) + (M(message_group_gap) - M(message_line_gap)) * 0.35f
                               : M(message_group_gap));
  float reply_y = y + top;   // a reply's reference line takes the top of the row, the header follows it
  top += dc_decor_reply_h(&d);
  float skip = compact ? 0.0f : M(message_group_gap) * 0.5f;
  float row_h_vis = h - skip - dc_tail_pad(ui, i);
  bool ephem = ccord_message_is_ephemeral(m);
  ccord_color dec_accent = {0}, dec_tint = {0};
  bool dec_collapsed = false;
  const char *dec_collapse_label = nullptr;
  bool has_dec_style = ui_decor_message_style(ui, m->id, &dec_accent, &dec_tint, &dec_collapsed, &dec_collapse_label);
  if (dec_collapsed) {
    ccord_rect crow = {ma->x + M(message_pad_x), y + 2.0f, maxf(ma->w - M(message_pad_x) * 2.0f, 40.0f), 24.0f};
    ccord_draw_rrect(r, crow, 4.0f, COL(bg_secondary));
    const char *clbl = (dec_collapse_label && dec_collapse_label[0]) ? dec_collapse_label : "Message collapsed by plugin";
    ccord_text_style cst = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 1.0f, COL(text_muted));
    ccord_text_draw_ellipsis(r, clbl, strlen(clbl), crow.x + 10.0f, crow.y + 4.0f, crow.w - 20.0f, &cst);
    return;
  }
  if (ephem) {
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, ui_alpha(COL(brand), 0.08f));
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, 2.5f, row_h_vis}, COL(brand));
  } else if (dc_message_mentions_self(ui, m)) {
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, ui_alpha(COL(status_idle), 0.10f));
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, 2.0f, row_h_vis}, COL(status_idle));
  }
  if (has_dec_style) {
    if (dec_tint.a > 0) ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, dec_tint);
    if (dec_accent.a > 0) ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, 3.0f, row_h_vis}, dec_accent);
  }
  if (ui->jump_highlight_msg_id == m->id && ui->now_ms < ui->jump_highlight_until_ms) {
    float flash = jump_flash_strength(ui);
    ui->dbg_jump_flash = flash;
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, ui_alpha(COL(brand), 0.22f * flash));
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, 2.0f, row_h_vis}, ui_alpha(COL(brand), flash));
  }
  float row_hv = ui_fade(ui, "msg", m->id, hover);   // the row's wash and its action bar ease in and out with the pointer
  if (row_hv > 0.004f) {
    ccord_draw_rect(r, (ccord_rect){ma->x, y + skip, ma->w, row_h_vis}, ui_alpha(COL(bg_message_hover), row_hv));
  }
  float text_x = ma->x + M(message_text_x);
  float content_y = y + top;
  ccord_id guild = ccord_store_selected_guild(ui->d.store);
  if (dc_decor_reply_h(&d) > 0.0f) {
    // The connector rises from the top of the avatar column; a grouped or dense row has no avatar, so it stays a stub.
    float sx = ma->x + M(message_pad_x) + M(avatar_size) * 0.5f;
    float y1 = (!compact && !dense) ? y + top + 2.0f : reply_y + UI_REPLY_LINE_H;
    dc_draw_reply_line(ui, r, &d, guild, sx, y1, text_x, reply_y,
                    maxf(ma->w - M(message_text_x) - M(message_pad_x), 40.0f));
  }
  if (!compact) {
    const ccord_user *u = ccord_store_user(ui->d.store, m->author_id);
    const ccord_user_profile *prof = ccord_store_user_profile(ui->d.store, m->author_id);
    const char *name = (u && u->name && u->name[0]) ? u->name
                       : ((prof && prof->display_name && prof->display_name[0]) ? prof->display_name : "Unknown User");
    const ccord_member *gm = (guild != CCORD_ID_NONE && guild != CCORD_DM_GUILD_ID)
                               ? ccord_store_find_member(ui->d.store, guild, m->author_id)
                               : nullptr;
    if (gm && gm->nick[0] != '\0') name = gm->nick;
    const char *row_deco = (gm && gm->avatar_decoration_url[0] != '\0')
                             ? gm->avatar_decoration_url
                             : ((u && u->avatar_decoration_url && u->avatar_decoration_url[0])
                                  ? u->avatar_decoration_url
                                  : ((prof && prof->avatar_decoration_url) ? prof->avatar_decoration_url : ""));
    ccord_user row_u = {
      .id = m->author_id,
      .name = name,
      .handle = (u && u->handle && u->handle[0]) ? u->handle : ((prof && prof->handle) ? prof->handle : ""),
      .status_text = (u && u->status_text) ? u->status_text : "",
      .avatar_url = (u && u->avatar_url && u->avatar_url[0]) ? u->avatar_url
                    : ((prof && prof->avatar_url) ? prof->avatar_url : ""),
      .avatar_decoration_url = row_deco,
      .presence = u ? u->presence : CCORD_PRESENCE_OFFLINE,
      .is_bot = u ? u->is_bot : false,
      .avatar_palette = u ? u->avatar_palette : (uint8_t)(m->author_id % 6u),
    };
    if (!dense) {
      float ax = ma->x + M(message_pad_x);
      float ay = y + top + 2.0f;
      float asz = M(avatar_size);
      ui_draw_avatar(ui, r, &row_u, ax + asz * 0.5f, ay + asz * 0.5f, asz, (ccord_color){0, 0, 0, 0}, false);
      ccord_rect av_rect = {ax, ay, asz, asz};
      if (out_avatar_rect) *out_avatar_rect = av_rect;
      dc_record_user_hit(ui, av_rect, m->author_id);
    }
    bool has_role;
    ccord_color rc = ui_role_color(ui, guild, m->author_id, &has_role);
    ccord_color name_col = (has_role && ui->settings_role_color_mode == 0) ? rc : COL(text_header);
    ccord_text_style ns = ui_style(ui, CCORD_FONT_BOLD, M(font_size_base), name_col);
    float nw = ccord_text_measure(r, name, strlen(name), &ns, 0.0f).x;
    float max_name = maxf(ma->w - M(message_text_x) - M(message_pad_x) - 160.0f, 60.0f);
    dc_chat_sel_process_run(ui, r, name, strlen(name), &ns, text_x, y + top, fminf(nw, max_name), lh, 0.0f);
    nw = ccord_text_draw_ellipsis(r, name, strlen(name), text_x, y + top, max_name, &ns);
    ccord_rect au_rect = {text_x, y + top, maxf(nw, 4.0f), lh};
    if (out_author_rect) *out_author_rect = au_rect;
    dc_record_user_hit(ui, au_rect, m->author_id);
    float x = text_x + nw + 8.0f;
    const char *role_icon = ui_member_role_icon(ui, guild, m->author_id);
    bool show_role_dot = has_role && ui->settings_role_color_mode == 1;
    {   // the blank between the name and the time (badges included) is part of a selected header line
      float x_time = x;
      if (show_role_dot) x_time += 14.0f;
      if (role_icon && role_icon[0]) {
        ccord_text_style is = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_header));
        x_time += ccord_text_measure(r, role_icon, strlen(role_icon), &is, 0.0f).x + 6.0f;
      }
      if (u && u->is_bot) {
        ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
        x_time += ccord_text_measure(r, "BOT", 3, &bs, 0.0f).x + 9.0f + 6.0f;
      }
      if (m->tts) {
        ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_secondary));
        x_time += ccord_text_measure(r, "TTS", 3, &bs, 0.0f).x + 9.0f + 6.0f;
      }
      if (u && u->clan_tag[0] != '\0') {
        x_time += ui_clan_badge_width(ui, r, u->clan_tag,
                                      u->clan_guild_id != CCORD_ID_NONE && u->clan_badge[0] != '\0') +
                  6.0f;
      }
      dc_chat_sel_process_gap(ui, r, text_x + nw, y + top, 1, x_time - (text_x + nw), lh);
    }
    if (show_role_dot) {
      ccord_draw_circle(r, x + 4.0f, y + top + lh * 0.5f, 4.0f, rc);
      x += 14.0f;
    }
    if (role_icon && role_icon[0]) {
      ccord_text_style is = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small), COL(text_header));
      ccord_vec2 im = ccord_text_measure(r, role_icon, strlen(role_icon), &is, 0.0f);
      ccord_text_draw(r, role_icon, strlen(role_icon), x, y + top + (lh - im.y) * 0.5f, &is, 0.0f);
      x += im.x + 6.0f;
    }
    if (u && u->is_bot) {
      ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_on_brand));
      ccord_vec2 bt = ccord_text_measure(r, "BOT", 3, &bs, 0.0f);
      ccord_rect tag = {x, y + top + (lh - 15.0f) * 0.5f + 0.5f, bt.x + 9.0f, 15.0f};
      ccord_draw_rrect(r, tag, 3.0f, COL(brand));
      ccord_text_draw(r, "BOT", 3, tag.x + 4.5f, tag.y + (tag.h - bt.y) * 0.5f, &bs, 0.0f);
      x += tag.w + 6.0f;
    }
    if (m->tts) {   // text-to-speech messages carry a TTS badge next to the author
      ccord_text_style bs = ui_style(ui, CCORD_FONT_BOLD, 10.0f, COL(text_secondary));
      ccord_vec2 bt = ccord_text_measure(r, "TTS", 3, &bs, 0.0f);
      ccord_rect tag = {x, y + top + (lh - 15.0f) * 0.5f + 0.5f, bt.x + 9.0f, 15.0f};
      ccord_draw_rrect(r, tag, 3.0f, COL(bg_tertiary));
      ccord_text_draw(r, "TTS", 3, tag.x + 4.5f, tag.y + (tag.h - bt.y) * 0.5f, &bs, 0.0f);
      x += tag.w + 6.0f;
      ui->dbg_tts_badges++;
    }
    if (u && u->clan_tag[0] != '\0') {
      x += ui_draw_clan_badge_ex(ui, r, u->clan_guild_id, u->clan_tag, u->clan_badge, x, y + top, lh,
                                 UI_CLAN_LAYER_CHAT) + 6.0f;
    }
    x += ui_decor_draw_message_badges(ui, r, m->id, x, y + top, lh, COL(bg_primary));
    char when[48];
    size_t wl = ccord_timefmt_message(when, sizeof when, m->timestamp_ms, ui->now_unix_ms, ui->use_utc);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    float wy = y + top + (lh - ui_line_h(ui, ts.size)) * 0.5f;
    ccord_vec2 when_sz = ccord_text_measure(r, when, wl, &ts, 0.0f);
    dc_chat_sel_process_run(ui, r, when, wl, &ts, x, y + top, when_sz.x, lh, 0.0f);
    ccord_text_draw(r, when, wl, x, wy, &ts, 0.0f);
    if (m->timestamp_ms > 0) dc_record_ts_hit(ui, (ccord_rect){x, y + top, when_sz.x, lh}, m->timestamp_ms, false);
    float header_end = x + when_sz.x;
    if (dc_message_edited(m)) {
      dc_chat_sel_process_gap(ui, r, header_end, y + top, 1, 6.0f, lh);
      ccord_vec2 et = ccord_text_measure(r, "(edited)", 8, &ts, 0.0f);
      dc_chat_sel_process_run(ui, r, "(edited)", 8, &ts, x + when_sz.x + 6.0f, y + top, et.x, lh, 0.0f);
      ccord_text_draw(r, "(edited)", 8, x + when_sz.x + 6.0f, wy, &ts, 0.0f);
      if (m->edited_timestamp_ms > 0) {
        dc_record_ts_hit(ui, (ccord_rect){x + when_sz.x + 6.0f, y + top, et.x, lh}, m->edited_timestamp_ms, true);
      }
      header_end += 6.0f + et.x;
    }
    dc_chat_sel_line_end(ui, r, header_end, y + top, ts.size * 0.4f, lh, true);
    content_y += lh;
  }
  bool no_body = d.body_len == 0 && dc_decor_has_body_cards(&d);
  ui_jumbo_item jumbo_items[UI_JUMBO_EMOJI_MAX];
  size_t jumbo_n = no_body ? 0u : dc_chat_message_jumbo(ui, m, jumbo_items);
  if (compact && hover) {
    char clock[24];
    size_t cl = ccord_timefmt_clock(clock, sizeof clock, m->timestamp_ms, ui->use_utc);
    ccord_text_style ts = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 3.0f, COL(text_muted));
    ccord_vec2 tw = ccord_text_measure(r, clock, cl, &ts, 0.0f);
    float gutter = M(message_text_x) - M(message_pad_x);
    float clock_x = ma->x + M(message_pad_x) + (gutter - tw.x) * 0.5f - 4.0f;
    float first_h = jumbo_n > 0u ? (M(font_size_base) * 3.0f + 8.0f) : lh;
    ccord_text_draw(r, clock, cl, clock_x, content_y + (first_h - ui_line_h(ui, ts.size)) * 0.5f, &ts, 0.0f);
    if (m->timestamp_ms > 0) dc_record_ts_hit(ui, (ccord_rect){clock_x, content_y, tw.x, first_h}, m->timestamp_ms, false);
  }
  float wrap = dc_content_wrap(ui, ma->w);
  ui->current_drawing_msg_id = m->id;
  ccord_color body_c = COL(text_normal);
  if (m->send_state == CCORD_MSG_SEND_PENDING) body_c = ui_alpha(COL(text_normal), 0.55f);
  else if (m->send_state == CCORD_MSG_SEND_FAILED) body_c = COL(danger);
  if (compact && m->tts) ui->dbg_tts_badges++;   // the "(tts)" body marker is this row's badge
  float body_h = no_body
                     ? 0.0f
                     : (jumbo_n > 0u
                            ? dc_chat_jumbo_emoji_walk(ui, r, jumbo_items, jumbo_n, text_x, content_y, wrap,
                                                    body_c, compact && dc_message_edited(m),
                                                    compact && m->tts, true)
                            : dc_md_body_walk(ui, r, d.body, d.body_len, text_x, content_y, wrap, body_c, true));
  ui->current_drawing_msg_id = CCORD_ID_NONE;
  float card_y = content_y + body_h + M(message_line_gap) * 0.5f;
  float card_w = maxf(ma->w - M(message_text_x) - M(message_pad_x), 40.0f);
  float after_cards_y = card_y + dc_draw_decor_cards(ui, r, &d, text_x, card_y, card_w);
  after_cards_y += ui_decor_draw_message_accessories(ui, r, m->id, text_x, after_cards_y, card_w, COL(bg_primary));
  if (dc_message_has_reactions(m)) {
    float by = after_cards_y + M(message_line_gap) * 0.5f;
    after_cards_y = by + dc_reaction_pills_walk(ui, r, m, text_x, by, wrap, true);
  }
  if (message_send_unconfirmed(m)) {
    float sy = after_cards_y + M(message_line_gap) * 0.5f;
    draw_send_status(ui, r, m, text_x, sy);
    after_cards_y = sy + UI_SEND_STATUS_H;
  }
  if (ephem) {
    float ey = after_cards_y + 3.0f;
    const char *eprefix = "Only you can see this \xE2\x80\x94 ";
    const char *elink = "Dismiss message";
    ccord_text_style eps = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_muted));
    ccord_text_style els = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_link));
    ccord_vec2 ept = ccord_text_measure(r, eprefix, strlen(eprefix), &eps, 0.0f);
    ccord_vec2 elt = ccord_text_measure(r, elink, strlen(elink), &els, 0.0f);
    ccord_text_draw(r, eprefix, strlen(eprefix), text_x, ey, &eps, 0.0f);
    float lx = text_x + ept.x;
    ccord_rect lrect = {lx, ey - 1.0f, elt.x + 4.0f, maxf(elt.y + 2.0f, 16.0f)};
    ccord_text_draw(r, elink, strlen(elink), lx, ey, &els, 0.0f);
    if (ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, lrect)) {
      ccord_draw_rect(r, (ccord_rect){lx, ey + elt.y - 1.0f, elt.x, 1.0f}, COL(text_link));
    }
    if (ui->nephem_hits < UI_EPHEM_HIT_MAX) {
      ui->ephem_hits[ui->nephem_hits].rect = lrect;
      ui->ephem_hits[ui->nephem_hits].channel_id = m->channel_id;
      ui->ephem_hits[ui->nephem_hits].message_id = m->id;
      ui->nephem_hits++;
    }
  }
  if (!ui->touch_mode && row_hv > 0.004f && !message_send_unconfirmed(m)) {
    // Unconfirmed local echoes have no server-side message yet: reactions, reply, pin and the overflow
    // menu would all fail against their synthetic ids, so only Retry is offered.
    ccord_push_alpha(r, row_hv);
    ccord_push_offset(r, 0.0f, (1.0f - row_hv) * 4.0f);
    float bw = UIS_ICON_BTN, bh = UIS_ICON_BTN;
    bool edit_btn = !ephem && ui_message_editable_by_self(ui, m);   // own messages: Edit sits right after react
    float nbtn = ephem ? 1.0f : (edit_btn ? 5.0f : 4.0f);
    float bar_w = nbtn * bw + nbtn + 3.0f, bar_h = bh + 4.0f;   // 1 px between buttons, 2 px inset at each end
    float bar_x = ma->x + ma->w - bar_w - 16.0f;
    float bar_y = maxf(y + skip - 20.0f, ma->y + 2.0f);   // hangs over the row top: 32 px buttons clear the author line
    ccord_rect bar = {bar_x, bar_y, bar_w, bar_h};
    uis_frame(ui, r, bar, uis_tokens_of(ui).r_control, COL(bg_primary), UIS_ELEV_POPOVER);
    ccord_text_style ps = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small), COL(text_secondary));
    if (ephem) {
      ccord_rect b3 = {bar.x + 2.0f, bar.y + 2.0f, bw, bh};
      ps.color = uis_icon_button(ui, r, b3, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b3)}, "barbtn", 3);
      ccord_vec2 mt = ccord_text_measure(r, "...", 3, &ps, 0.0f);
      ccord_text_draw(r, "...", 3, b3.x + (b3.w - mt.x) * 0.5f, b3.y + (b3.h - mt.y) * 0.5f - 2.0f, &ps, 0.0f);
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
      if (hover) {
        ui->hover_bar_msg_id = m->id;
        ui->hover_bar_rect = bar;
        ui->hover_bar_reply_rect = (ccord_rect){0, 0, 0, 0};
        ui->hover_bar_pin_rect = (ccord_rect){0, 0, 0, 0};
        ui->hover_bar_more_rect = b3;
      }
    } else {
      // Slots after react: Edit (own messages only) comes first, so reply, pin and overflow move one slot right.
      float slot = bw + 1.0f, after_react = edit_btn ? 2.0f : 1.0f;
      ccord_rect b0 = {bar.x + 2.0f, bar.y + 2.0f, bw, bh};
      ccord_rect be = {bar.x + 2.0f + slot, bar.y + 2.0f, bw, bh};
      ccord_rect b1 = {bar.x + 2.0f + after_react * slot, bar.y + 2.0f, bw, bh};
      ccord_rect b2 = {bar.x + 2.0f + (after_react + 1.0f) * slot, bar.y + 2.0f, bw, bh};
      ccord_rect b3 = {bar.x + 2.0f + (after_react + 2.0f) * slot, bar.y + 2.0f, bw, bh};
      ccord_color c0 = uis_icon_button(ui, r, b0, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b0)}, "barbtn", 0);
      ccord_color c1 = uis_icon_button(ui, r, b1, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b1)}, "barbtn", 1);
      ccord_color c2 = uis_icon_button(ui, r, b2, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b2)}, "barbtn", 2);
      ccord_color c3 = uis_icon_button(ui, r, b3, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, b3)}, "barbtn", 3);
      ui_expr_draw_smiley(r, b0.x + b0.w * 0.5f, b0.y + b0.h * 0.5f, 14.0f, c0);
      ui_icon_reply(r, b1.x + b1.w * 0.5f, b1.y + b1.h * 0.5f, 14.0f, c1);
      ui_icon_pin(r, b2.x + b2.w * 0.5f, b2.y + b2.h * 0.5f, 14.0f, c2);
      ps.color = c3;
      ccord_vec2 mt = ccord_text_measure(r, "...", 3, &ps, 0.0f);
      ccord_text_draw(r, "...", 3, b3.x + (b3.w - mt.x) * 0.5f, b3.y + (b3.h - mt.y) * 0.5f - 2.0f, &ps, 0.0f);
      if (edit_btn) {
        ccord_color ce = uis_icon_button(ui, r, be, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, be)}, "barbtn", 4);
        ui_icon_edit(r, be.x + be.w * 0.5f, be.y + be.h * 0.5f, 14.0f, ce);
      }
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
      if (hover) {   // only the row under the pointer has a live bar; a fading one is for the eye
        ui->hover_bar_msg_id = m->id;
        ui->hover_bar_rect = bar;
        ui->hover_bar_reply_rect = b1;
        ui->hover_bar_pin_rect = b2;
        ui->hover_bar_more_rect = b3;
        ui->hover_bar_edit_rect = edit_btn ? be : (ccord_rect){0, 0, 0, 0};
        if (ui->nreaction_hits < UI_REACTION_HIT_MAX) {
          ui_reaction_hit *hit = &ui->reaction_hits[ui->nreaction_hits++];
          hit->rect = b0;
          hit->channel_id = m->channel_id;
          hit->message_id = m->id;
          hit->emoji[0] = '\0';
          hit->is_add_btn = true;
        }
        if (ui->d.contrib && ccord_contrib_action_btn_count(ui->d.contrib, CCORD_ACTBTN_MESSAGE_HOVER) > 0 &&
            bar.x > ma->x + 40.0f) {
          ccord_menu_context mctx = {
            .target = CCORD_MENU_MESSAGE,
            .guild_id = guild,
            .channel_id = m->channel_id,
            .message_id = m->id,
            .user_id = m->author_id,
          };
          ccord_rect pab = {ma->x, bar.y + 2.0f, bar.x - ma->x - 4.0f, bh};
          (void)ui_actbtns_draw_slot(ui, r, CCORD_ACTBTN_MESSAGE_HOVER, pab, true, &mctx);
        }
      }
    }
  }
}

static void draw_empty_state(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  const ccord_channel *ch = ui_selected_channel(ui);
  if (!ch) return;
  if (ui->search_open && ui->search_edit.len > 0) {
    char msg[192];
    snprintf(msg, sizeof msg, "No messages match \"%s\"", ccord_text_edit_cstr(&ui->search_edit));
    ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    ccord_vec2 t = ccord_text_measure(r, msg, strlen(msg), &st, ma->w - 40.0f);
    ccord_text_draw(r, msg, strlen(msg), ma->x + (ma->w - t.x) * 0.5f, ma->y + ma->h * 0.5f - t.y * 0.5f, &st, ma->w - 40.0f);
    return;
  }
  if (!ccord_store_history_loaded(ui->d.store, ch->id)) {
    ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_muted));
    const char *msg = "Loading messages...";
    ccord_vec2 t = ccord_text_measure(r, msg, strlen(msg), &st, 0.0f);
    ccord_text_draw(r, msg, strlen(msg), ma->x + (ma->w - t.x) * 0.5f, ma->y + ma->h * 0.5f - t.y * 0.5f, &st, 0.0f);
    return;
  }
  if (ch->guild_id == CCORD_DM_GUILD_ID) {
    float iy = maxf(ma->y + 16.0f, ma->y + ma->h - UI_DM_INTRO_H - 16.0f);
    draw_dm_intro(ui, r, ch, ma->x + M(message_pad_x), iy, ma->w - 2.0f * M(message_pad_x));
    return;
  }
  char title[160], sub[240];
  snprintf(title, sizeof title, "Welcome to #%s!", ch->name);
  snprintf(sub, sizeof sub, "This is the start of the #%s channel.", ch->name);
  ccord_text_style ts = ui_style(ui, CCORD_FONT_BOLD, M(font_size_large) + 12.0f, COL(text_header));
  ccord_text_style ss = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_base), COL(text_secondary));
  float wrap = ma->w - 2.0f * M(message_pad_x);
  ccord_vec2 tt = ccord_text_measure(r, title, strlen(title), &ts, wrap);
  ccord_vec2 st = ccord_text_measure(r, sub, strlen(sub), &ss, wrap);
  float y = ma->y + ma->h - 16.0f - st.y - 8.0f - tt.y;
  ccord_text_draw(r, title, strlen(title), ma->x + M(message_pad_x), y, &ts, wrap);
  ccord_text_draw(r, sub, strlen(sub), ma->x + M(message_pad_x), y + tt.y + 8.0f, &ss, wrap);
}

bool dc_friend_matches_tab(const ccord_relationship *rel, const ccord_user *u, int tab)
{
  if (!rel) return false;
  switch (tab) {
    case 0: return rel->kind == CCORD_RELATIONSHIP_FRIEND && u && u->presence != CCORD_PRESENCE_OFFLINE;
    case 1: return rel->kind == CCORD_RELATIONSHIP_FRIEND;
    case 2: return rel->kind == CCORD_RELATIONSHIP_PENDING_INCOMING || rel->kind == CCORD_RELATIONSHIP_PENDING_OUTGOING;
    case 3: return rel->kind == CCORD_RELATIONSHIP_BLOCKED;
  }
  return false;
}

// Shared fading scrollbar for the clipped lists outside the timeline (Friends, stage tiles, forum,
// pins, inbox, reactions, message requests, quick switcher). Same look as the timeline thumb: it
// appears while hovered or shortly after scrolling, then fades out. Returns the drawn thumb rect
// (zero size when the list fits or the thumb is invisible) so tests can assert it.
ccord_rect ui_draw_list_scrollbar(ccord_ui *ui, ccord_renderer *r, ccord_rect view, float content_h,
                                  float scroll, const char *ns, uint64_t id)
{
  if (view.w <= 0.0f || view.h <= 0.0f || content_h <= view.h + 0.5f) return (ccord_rect){0, 0, 0, 0};
  bool over = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, view);
  bool recent = ui->last_scroll_ms && ui->now_ms < ui->last_scroll_ms + 1200;
  bool shown = over || recent;
  float vis = ui_fade_ms(ui, ns, id, shown, 160);
  if (vis < 0.01f) return (ccord_rect){0, 0, 0, 0};
  float sbw = M(scrollbar_width);
  float thumb_h = maxf(view.h * (view.h / content_h), M(scrollbar_min_thumb));
  if (thumb_h > view.h) thumb_h = view.h;
  float max_s = content_h - view.h;
  float norm = max_s > 0.0f ? clampf(scroll / max_s, 0.0f, 1.0f) : 0.0f;
  float top = view.y + norm * (view.h - thumb_h);
  float x = view.x + view.w - sbw - 3.0f;
  ccord_draw_rrect(r, (ccord_rect){x, view.y + 2.0f, sbw, view.h - 4.0f}, sbw * 0.5f, ui_alpha(COL(scrollbar_track), vis));
  ccord_draw_rrect(r, (ccord_rect){x, top, sbw, thumb_h}, sbw * 0.5f, ui_alpha(COL(scrollbar_thumb), vis));
  return (ccord_rect){x, top, sbw, thumb_h};
}


static void draw_scrollbar(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  ui->thumb_h = 0.0f;
  float top01, size01;
  ccord_vlist_thumb(&ui->vlist, ma->h, &top01, &size01);
  if (size01 >= 0.999f) return;
  bool over = ui->mouse_in && !ui->menu.open && !ui->settings_open && ui_in_rect(ui->mx, ui->my, *ma);
  bool recent = ui->last_scroll_ms && ui->now_ms < ui->last_scroll_ms + 1200;
  bool shown = over || recent || ui->drag == DRAG_SCROLLBAR;
  float vis = ui_fade_ms(ui, "vscroll", 0, shown, 160);   // fades in and out instead of blinking
  if (vis < 0.01f) return;
  float sbw = M(scrollbar_width);
  float thumb_h = maxf(size01 * ma->h, M(scrollbar_min_thumb));
  if (thumb_h > ma->h) thumb_h = ma->h;
  float norm = size01 < 0.999f ? top01 / (1.0f - size01) : 0.0f;
  float top = ma->y + clampf(norm, 0.0f, 1.0f) * (ma->h - thumb_h);
  float x = ma->x + ma->w - sbw - 3.0f;
  ccord_draw_rrect(r, (ccord_rect){x, ma->y + 2.0f, sbw, ma->h - 4.0f}, sbw * 0.5f, ui_alpha(COL(scrollbar_track), vis));
  ccord_draw_rrect(r, (ccord_rect){x, top, sbw, thumb_h}, sbw * 0.5f, ui_alpha(COL(scrollbar_thumb), vis));
  ui->thumb_x = x;
  ui->thumb_w = sbw;
  ui->thumb_top = top;
  ui->thumb_h = shown ? thumb_h : 0.0f;   // a bar that is on its way out cannot be grabbed
}

// Typing expiry is unix ms from live transports but the mock can run on a small virtual clock.
static bool typing_notice_active(const ccord_ui *ui, int64_t expires_ms)
{
  if (expires_ms <= 0) return false;
  int64_t now = expires_ms > 1000000000000LL ? ui->now_unix_ms : (int64_t)ui->now_ms;
  return expires_ms > now;
}

static size_t active_typing_count(const ccord_ui *ui, ccord_id ch)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  size_t total = 0;
  size_t n = ccord_store_typing_count(ui->d.store, ch);
  for (size_t i = 0; i < n; i++) {
    ccord_id user = ccord_store_typing_user_at(ui->d.store, ch, i);
    if (user == CCORD_ID_NONE || user == self) continue;
    if (typing_notice_active(ui, ccord_store_typing_expires_at(ui->d.store, ch, i))) total++;
  }
  return total;
}

static float typing_reserve_h(const ccord_ui *ui)
{
  return ui_line_h(ui, M(font_size_small) - 2.0f) + 14.0f;
}

static void draw_typing(ccord_ui *ui, ccord_renderer *r, const ccord_rect *ma)
{
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return;
  ccord_id self = ccord_store_self_id(ui->d.store);
  char names[3][CCORD_NAME_MAX_BYTES + 1];
  size_t shown = 0, total = 0;
  size_t n = ccord_store_typing_count(ui->d.store, ch);
  for (size_t i = 0; i < n; i++) {
    ccord_id user = ccord_store_typing_user_at(ui->d.store, ch, i);
    if (user == CCORD_ID_NONE || user == self) continue;
    if (!typing_notice_active(ui, ccord_store_typing_expires_at(ui->d.store, ch, i))) continue;
    total++;
    if (shown < 3) {
      const ccord_user *u = ccord_store_user(ui->d.store, user);
      snprintf(names[shown], sizeof names[shown], "%s", u ? u->name : "Someone");
      shown++;
    }
  }
  if (total == 0) {
    ui->typing_next_ms = 0;
    ui->typing_anim_only = false;
    return;
  }
  char text[3 * (CCORD_NAME_MAX_BYTES + 1) + 32];
  if (total == 1) snprintf(text, sizeof text, "%s is typing...", names[0]);
  else if (total == 2) snprintf(text, sizeof text, "%s and %s are typing...", names[0], names[1]);
  else if (total == 3) snprintf(text, sizeof text, "%s, %s and %s are typing...", names[0], names[1], names[2]);
  else snprintf(text, sizeof text, "Several people are typing...");
  ccord_text_style st = ui_style(ui, CCORD_FONT_REGULAR, M(font_size_small) - 2.0f, COL(text_secondary));
  ccord_vec2 t = ccord_text_measure(r, text, strlen(text), &st, ma->w - 2.0f * M(message_pad_x));
  float h = ui_line_h(ui, st.size) + 8.0f;
  const float dots_w = 24.0f;   // the three bouncing dots in front of the text
  ccord_rect box = {ma->x + M(message_pad_x), ma->y + ma->h - h - 6.0f, t.x + 20.0f + dots_w, h};
  if (box.w > ma->w - 2.0f * M(message_pad_x)) box.w = ma->w - 2.0f * M(message_pad_x);
  ui_popup_begin(ui, r, "typing", 0, UI_POPUP_MS, 6.0f);
  ccord_draw_rrect(r, box, h * 0.5f, COL(bg_tertiary));
  for (int d = 0; d < 3; d++) {
    float wave = 0.0f;
    if (!ui->anim.reduced_motion) {
      float phase = (float)(ui->now_ms % 1200u) / 1200.0f * 6.2831853f - (float)d * 0.9f;
      wave = fmaxf(0.0f, sinf(phase));   // each dot rises in turn, then rests
    }
    ccord_draw_circle(r, box.x + 12.0f + (float)d * 6.0f, box.y + h * 0.5f - wave * 2.5f, 2.0f,
                      ui_alpha(COL(text_secondary), ui->anim.reduced_motion ? 0.65f : (0.45f + 0.55f * wave)));
  }
  ccord_text_draw(r, text, strlen(text), box.x + 10.0f + dots_w, box.y + (h - t.y) * 0.5f, &st, box.w - 20.0f - dots_w);
  ui_popup_end(r);
  if (ui->anim.reduced_motion) {
    ui->typing_next_ms = 0;
  } else {
    // Throttle typing-dot redraws to ~20 Hz (50 ms) instead of pegging 144 Hz;
    // ui_draw_frame finalizes typing_anim_only & anim.pending after all other animations run.
    ui->typing_next_ms = ui->now_ms + 50u;
  }
}

// Reaching the top while the user scrolls up asks the session for the previous page (before=<id>).
static void maybe_request_older(ccord_ui *ui, size_t first_visible)
{
  // Search keeps the full timeline, so paging older history works while the panel is open too.
  if (!ui->scroll_up_pending || !ui->d.session) return;
  if (first_visible > 5 || ui->vlist.stick_bottom || ui->vlist.count == 0) return;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return;
  if (!ccord_session_history_has_more(ui->d.session, ch)) {
    ui->scroll_up_pending = false;
    return;
  }
  const ccord_message *first = ccord_store_message_at(ui->d.store, ch, 0);
  if (!first) return;
  if (ui->hist_request_ms && ui->now_ms < ui->hist_request_ms + UI_HISTORY_RETRY_MS) return;
  if (ui->hist_channel == ch && ui->hist_before == first->id) {
    ui->scroll_up_pending = false;
    return;
  }
  ccord_error err;
  if (ccord_session_request_history_before(ui->d.session, ch, first->id, &err) == CCORD_OK) {
    ui->hist_channel = ch;
    ui->hist_before = first->id;
    ui->hist_request_ms = ui->now_ms ? ui->now_ms : 1;
    ui->scroll_up_pending = false;
  } else {
    ccord_error_clear(&err);
  }
}


// Clears every message-timeline hit / debug counter. The timeline repopulates these as it draws;
// the stage view must clear them too, or stale hits from a previously drawn transcript would
// intercept clicks meant for the stage (e.g. the Text Chat button).
void dc_reset_message_hits(ccord_ui *ui)
{
  ui->nvis = 0;
  ui->ndm_request_hits = 0;
  ui->nlinks = 0;
  ui->ntext_hits = 0;
  ui->nuser_hits = 0;
  ui->nreaction_hits = 0;
  ui->ndbg_reaction_draws = 0;
  ui->ncode_copy_hits = 0;
  ui->nspoiler_hits = 0;
  ui->nchan_hits = 0;
  ui->nreply_hits = 0;
  ui->nts_hits = 0;
  ui->nimg_hits = 0;
  ui->nmedia_hits = 0;
  ui->npoll_hits = 0;
  ui->ncomp_hits = 0;
  ui->nephem_hits = 0;
  ui->nretry_hits = 0;
  ui->dbg_pending_echoes = ui->dbg_failed_echoes = 0;
  ui->nthread_hits = 0;
  ui->ninvite_hits = 0;
  ui->dbg_invite_cards = 0;
  ui->nfriend_row_hits = 0;
  ui->jump_present_rect = (ccord_rect){0, 0, 0, 0};
  ui->new_bar_rect = (ccord_rect){0, 0, 0, 0};
  ui->new_bar_ack_rect = (ccord_rect){0, 0, 0, 0};
  ui->new_bar_target_row = SIZE_MAX;
  ui->dbg_date_dividers = 0;
  ui->dbg_new_dividers = 0;
  ui->nblocked_hits = 0;
  ui->dbg_blocked_text_drawn = 0;
  ui->ndbg_code_runs = 0;
  ui->ndbg_md_runs = 0;
  ui->dbg_gallery_cards = 0;
  ui->dbg_media_player_cards = 0;
  ui->dbg_embed_fields_drawn = 0;
  ui->dbg_component_rows = 0;
  ui->dbg_component_buttons = 0;
  ui->dbg_component_selects = 0;
  memset(ui->dbg_cv2_drawn, 0, sizeof ui->dbg_cv2_drawn);
  memset(ui->dbg_cv2_height, 0, sizeof ui->dbg_cv2_height);
  ui->dbg_thread_cards = 0;
  ui->dbg_burst_reaction_pills = 0;
  ui->hover_bar_msg_id = CCORD_ID_NONE;
  ui->hover_bar_rect = (ccord_rect){0, 0, 0, 0};
  ui->hover_bar_reply_rect = (ccord_rect){0, 0, 0, 0};
  ui->hover_bar_pin_rect = (ccord_rect){0, 0, 0, 0};
  ui->hover_bar_more_rect = (ccord_rect){0, 0, 0, 0};
  ui->hover_bar_edit_rect = (ccord_rect){0, 0, 0, 0};
  ui->dbg_first = ui->dbg_last = ui->dbg_drawn = ui->dbg_measured = 0;
  ui->dbg_reply_cards = ui->dbg_attachment_cards = ui->dbg_embed_cards = 0;
  ui->dbg_sticker_cards = ui->dbg_forward_cards = ui->dbg_poll_cards = 0;
  ui->dbg_streamer_invite_shown[0] = '\0';
  ui->dbg_streamer_invite_masked[0] = '\0';
  ui->thumb_h = 0.0f;
}

// Notes the newest message of the selected channel. A newer one that arrives while the timeline is pinned to the bottom is
// a live arrival. Ids grow with arrival: a local echo has the top bit set, so the confirmation of an echo (a smaller
// snowflake that replaces it) and a deleted newest message are not new arrivals. The first sight of a channel is not one.
// Each live arrival keeps its own fade: a newer arrival does not restart or snap an older one that is still fading in.
static void chat_live_add(ccord_ui *ui, ccord_id msg)
{
  ui_chat_live *slot = &ui->chat_live[0];
  for (size_t i = 0; i < UI_CHAT_LIVE_MAX; i++) {
    if (ui->chat_live[i].until_ms < slot->until_ms) slot = &ui->chat_live[i];
  }
  slot->msg = msg;
  slot->until_ms = ui->now_ms + UI_MSG_ENTER_MS;
}

static bool chat_is_live(const ccord_ui *ui, ccord_id msg)
{
  for (size_t i = 0; i < UI_CHAT_LIVE_MAX; i++) {
    if (ui->chat_live[i].until_ms > ui->now_ms && ui->chat_live[i].msg == msg) return true;
  }
  return false;
}

// Notes the newest message of the selected channel. Messages newer than the old tail that arrive while the timeline is
// pinned to the bottom are live arrivals. Ids grow with arrival: a local echo has the top bit set, so the confirmation of
// an echo (a smaller snowflake that replaces it) and a deleted newest message are not new arrivals. The first sight of a
// channel is not one. After a gap (`resumed`: the timeline was not drawn last frame) the tail is only noted: messages
// that came in while it was hidden are old news when it is shown again.
static void chat_track_tail(ccord_ui *ui, ccord_id ch, bool resumed)
{
  size_t n = ccord_store_message_count(ui->d.store, ch);
  const ccord_message *last = n > 0 ? ccord_store_message_at(ui->d.store, ch, n - 1) : nullptr;
  ccord_id tail = last ? last->id : CCORD_ID_NONE;
  ccord_id prev = ui->chat_tail_channel == ch ? ui->chat_tail_id : CCORD_ID_NONE;
  ui->chat_tail_channel = ch;
  ui->chat_tail_id = tail;
  if (prev == CCORD_ID_NONE || tail <= prev || !ui->vlist.stick_bottom || resumed) return;
  // Several messages can arrive between two draws: the store is in id order, so walk back over the newer ones.
  for (size_t k = n, added = 0; k > 0 && added < UI_CHAT_LIVE_MAX; k--) {
    const ccord_message *m = ccord_store_message_at(ui->d.store, ch, k - 1);
    if (!m || m->id <= prev) break;
    chat_live_add(ui, m->id);
    added++;
  }
}

// Fade-in value of the row at view index i and the message it shows: 1 unless that message is a live arrival.
static float chat_row_enter(ccord_ui *ui, ccord_id ch, size_t i, ccord_id *msg_id)
{
  size_t store_i = 0;
  const ccord_message *m = dc_view_message_at(ui, ch, i, &store_i);
  *msg_id = m ? m->id : CCORD_ID_NONE;
  if (!m || !chat_is_live(ui, m->id)) return 1.0f;
  return ui_enter(ui, "msgin", m->id, UI_MSG_ENTER_MS);
}

// A message counts as seen once its row has been drawn; a reaction pill added later to it pops in.
static void chat_seen_note(ccord_ui *ui, ccord_id msg_id)
{
  if (msg_id != CCORD_ID_NONE && !chat_seen_find(ui, msg_id)) chat_seen_put(ui, msg_id);
}

// Messages that must lie below the viewport before the jump-to-present bar shows (settings, default 10).
static size_t jump_present_threshold(void)
{
  int n = ccord_ui_pref_jump_present_messages();
  return n < 1 ? 1u : (size_t)n;
}

void ui_draw_messages(ccord_ui *ui, ccord_renderer *r)
{
  (void)dc_chat_caches_ensure(ui);
  ccord_rect ma = ui_rect(ui->layout.message_area);
  dc_reset_message_hits(ui);
  if (ma.w <= 0.0f || ma.h <= 0.0f) return;
  if (ma.w != ui->vlist_width) {
    ui->vlist_width = ma.w;
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  ccord_push_clip(r, ma);
  ui_theme_bg_draw(ui, r, "chat", ma);
  if (ui_is_dm_requests_view(ui)) {
    dc_draw_dm_requests_view(ui, r, &ma);
    ccord_pop_clip(r);
    return;
  }
  if (ui_is_dm_friends_view(ui)) {
    dc_draw_dm_friends_view(ui, r, &ma);
    ccord_pop_clip(r);
    return;
  }
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ui->vlist.count == 0) {
    draw_empty_state(ui, r, &ma);
    ui_local_messages_draw(ui, r, ch, ma);
    draw_typing(ui, r, &ma);
    ccord_pop_clip(r);
    return;
  }
  msg_ctx ctx = {ui, r, ch};
  ccord_vlist_view view;
  ccord_error err;
  // Not drawn last frame (voice or stage view, Settings, another channel, a friends or empty view): the rows and pills
  // seen before the gap are stale and the live arrivals from before it are over, so the timeline starts settled.
  // A second draw in the same frame is not a gap.
  bool resumed = ui->chat_seq != ui->anim.frame && ui->chat_seq + 1 != ui->anim.frame;
  ui->chat_seq = ui->anim.frame;
  if (resumed) memset(ui->chat_live, 0, sizeof ui->chat_live);
  chat_track_tail(ui, ch, resumed);
  float layout_h = ma.h;
  if (active_typing_count(ui, ch) > 0) {
    layout_h = maxf(ma.h - typing_reserve_h(ui), 1.0f);
  }
  if (ccord_vlist_layout(&ui->vlist, layout_h, ma.w, dc_measure_row, &ctx, &view, &err) != CCORD_OK) {
    CCORD_LOG_E("ui: message layout failed: %s", err.message);
    ccord_pop_clip(r);
    return;
  }
  ui->dbg_nsel = 0;
  if (ui->chat_sel_active && ui->chat_sel_channel == ch) {
    ui->chat_sel_len = 0;
    ui->chat_sel_buf[0] = '\0';
  }
  float y = ma.y + view.first_top;
  size_t drawn = 0;
  for (size_t i = view.first; i <= view.last && i < ui->vlist.count; i++) {
    float h = ccord_vlist_row_height(&ui->vlist, i);
    ccord_rect av_rect = {0, 0, 0, 0}, au_rect = {0, 0, 0, 0};
    ccord_id row_msg = CCORD_ID_NONE;
    float enter = chat_row_enter(ui, ch, i, &row_msg);
    // A live arrival fades in and rises over its own drawing; the layout (y, h) of every row stays as measured.
    float dy = enter < 1.0f ? (1.0f - enter) * UI_MSG_ENTER_SLIDE : 0.0f;
    if (enter < 1.0f) {
      ccord_push_alpha(r, enter);
      ccord_push_offset(r, 0.0f, dy);
    }
    draw_row(&ctx, i, y, h, &ma, &av_rect, &au_rect);
    if (enter < 1.0f) {
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
    }
    chat_seen_note(ui, row_msg);
    if (ui->nvis < UI_VISIBLE_ROWS_MAX) ui->vis[ui->nvis++] = (vis_row){i, y, h, av_rect, au_rect, row_msg, enter, dy};
    y += h;
    drawn++;
  }
  ui->dbg_first = view.first;
  ui->dbg_last = view.last;
  ui->dbg_drawn = drawn;
  ui->dbg_measured = view.measured;
  maybe_request_older(ui, view.first);
  draw_scrollbar(ui, r, &ma);
  ui_nmb_draw(ui, r, &ma, ch, view.first, view.last);   // new_messages_bar.c: fades, draws and records the click rects

  // The bar waits until N messages are below the viewport. At the bottom of the loaded window there are none below, so it
  // stays hidden whatever the channel's last_message_id says.
  bool jump_on = ccord_vlist_rows_below(&ui->vlist, &view) >= jump_present_threshold();
  float jp_t = ui_fade_ms(ui, "jumpbar", 1u, jump_on, UI_BAR_MS);
  ui->dbg_jump_present_t = jp_t;
  // As with the new-messages bar, the click rect follows the bar's state, so a bar that is on is clickable at once.
  if (jp_t > 0.004f || jump_on) {
    const char *jtxt = "You're viewing older messages  ·  Jump to Present";
    ccord_text_style jst = ui_style(ui, CCORD_FONT_BOLD, M(font_size_small) - 2.0f, COL(text_on_brand));
    ccord_vec2 jt = ccord_text_measure(r, jtxt, strlen(jtxt), &jst, 0.0f);
    float jw = fminf(jt.x + 28.0f, ma.w - 32.0f);
    float jh = ui->mobile_layout ? 38.0f : 26.0f;
    ccord_rect jbar = {ma.x + (ma.w - jw) * 0.5f, ma.y + ma.h - jh - 8.0f, jw, jh};
    if (jump_on) ui->jump_present_rect = jbar;
    if (jp_t > 0.004f) {
      ccord_push_alpha(r, jp_t);
      ccord_push_offset(r, 0.0f, (1.0f - jp_t) * UI_BAR_SLIDE);   // it rises from below
      ccord_draw_rrect(r, jbar, jh * 0.5f, COL(brand));
      ccord_text_draw_ellipsis(r, jtxt, strlen(jtxt), jbar.x + 14.0f, jbar.y + (jbar.h - jt.y) * 0.5f, jbar.w - 28.0f, &jst);
      ccord_pop_offset(r);
      ccord_pop_alpha(r);
    }
  }
  ui_local_messages_draw(ui, r, ch, ma);
  draw_typing(ui, r, &ma);
  ccord_pop_clip(r);
}

