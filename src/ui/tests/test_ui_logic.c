// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Pure-logic tests for the ui module: layout, virtual list, text edit, time formatting. No SDL, no display.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/theme/theme.h>
#include <ccord/ui/layout.h>
#include <ccord/ui/text_edit.h>
#include <ccord/ui/timefmt.h>
#include <ccord/ui/virtual_list.h>

#include "ccord_test.h"

static ccord_theme_metrics default_metrics(void)
{
  ccord_theme t;
  ccord_error err;
  char path[512];
  snprintf(path, sizeof path, "%s/assets/themes/c-cord-dark.theme", CCORD_SOURCE_DIR);
  if (ccord_theme_load(&t, path, &err) != CCORD_OK) {
    fprintf(stderr, "theme: %s\n", err.message);
    exit(1);
  }
  return t.m;
}

// ---- layout ----

static float area(ccord_layout_rect r)
{
  return r.w * r.h;
}

static bool inside(ccord_layout_rect a, ccord_layout_rect w)
{
  return a.x >= w.x - 0.01f && a.y >= w.y - 0.01f && a.x + a.w <= w.x + w.w + 0.01f && a.y + a.h <= w.y + w.h + 0.01f;
}

static bool overlap(ccord_layout_rect a, ccord_layout_rect b)
{
  return a.x < b.x + b.w - 0.01f && b.x < a.x + a.w - 0.01f && a.y < b.y + b.h - 0.01f && b.y < a.y + a.h - 0.01f;
}

static void test_layout(const ccord_theme_metrics *m)
{
  const float sizes[][2] = {{640, 420}, {1280, 800}, {1920, 1080}, {999, 700}, {1000, 700}};
  for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
    float w = sizes[s][0], h = sizes[s][1];
    ccord_layout l;
    ccord_layout_compute(m, w, h, m->composer_min_height, &l);
    ccord_layout_rect win = {0, 0, w, h};
    ccord_layout_rect top[] = {l.server_list, l.channel_list, l.chat_header, l.message_area, l.composer};
    float sum = 0;
    for (size_t i = 0; i < 5; i++) {
      CT_CHECK(inside(top[i], win));
      sum += area(top[i]);
      for (size_t j = i + 1; j < 5; j++) CT_CHECK(!overlap(top[i], top[j]));
    }
    if (l.member_list_visible) {
      CT_CHECK(inside(l.member_list, win));
      sum += area(l.member_list);
      for (size_t i = 0; i < 5; i++) CT_CHECK(!overlap(top[i], l.member_list));
    }
    CT_CHECK(fabsf(sum - w * h) < 1.0f);   // the panels tile the window exactly
    CT_CHECK(!overlap(l.guild_header, l.user_panel) && inside(l.channel_scroll, l.channel_list));
    CT_CHECK(l.member_list_visible == (w >= m->hide_members_below_width));
  }
  // composer height is clamped
  ccord_layout a, b, c;
  ccord_layout_compute(m, 1280, 800, 0, &a);
  ccord_layout_compute(m, 1280, 800, 100000, &b);
  ccord_layout_compute(m, 1280, 800, m->composer_min_height, &c);
  CT_CHECK(a.composer.h == c.composer.h);
  CT_CHECK(fabsf(b.composer.h - (m->composer_max_height + m->composer_margin_bottom)) < 0.01f);
  CT_CHECK(b.message_area.h < a.message_area.h);
  // degenerate window sizes never produce negative rects
  ccord_layout d;
  ccord_layout_compute(m, 50, 30, 44, &d);
  CT_CHECK(d.message_area.w >= 0 && d.message_area.h >= 0 && d.composer.h >= 0 && d.channel_scroll.h >= 0);
}

// ---- virtual list ----

typedef struct fake {
  size_t calls;
} fake;

static float fake_measure(size_t i, float width, void *ud)
{
  (void)width;
  ((fake *)ud)->calls++;
  return 20.0f + (float)(i % 7) * 4.0f;   // 20..44
}

// The newest row is padded by the UI, so its height depends on whether it is the last one.
typedef struct tailfake {
  const ccord_vlist *v;
} tailfake;

static float tail_measure(size_t i, float width, void *ud)
{
  (void)width;
  const tailfake *t = ud;
  return 30.0f + (i + 1 == t->v->count ? 16.0f : 0.0f);
}

static float frac_measure(size_t i, float width, void *ud)
{
  (void)width;
  (void)ud;
  return 23.3f + (float)(i % 5) * 4.7f;
}

static void test_vlist_tail_row(void)
{
  ccord_vlist v;
  ccord_vlist_init(&v);
  ccord_error err;
  ccord_vlist_view view;
  tailfake t = {&v};
  CT_CHECK(ccord_vlist_set_count(&v, 10, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 100, 100, tail_measure, &t, &view, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_row_height(&v, 9) == 46.0f && ccord_vlist_row_height(&v, 8) == 30.0f);
  // append: the old last row loses its padding, the new one gets it
  CT_CHECK(ccord_vlist_set_count(&v, 11, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 100, 100, tail_measure, &t, &view, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_row_height(&v, 9) == 30.0f && ccord_vlist_row_height(&v, 10) == 46.0f);
  // shrink: the new last row gets the padding
  CT_CHECK(ccord_vlist_set_count(&v, 9, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 100, 100, tail_measure, &t, &view, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_row_height(&v, 8) == 46.0f);
  ccord_vlist_free(&v);

  // 0 -> 50 history load with fractional heights preserves stick_bottom and anchors to count - 1
  ccord_vlist_init(&v);
  CT_CHECK(ccord_vlist_set_count(&v, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 317.5f, 500, frac_measure, nullptr, &view, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_set_count(&v, 50, &err) == CCORD_OK);
  CT_CHECK(v.stick_bottom && v.anchor_index == 49);
  CT_CHECK(ccord_vlist_layout(&v, 317.5f, 500, frac_measure, nullptr, &view, &err) == CCORD_OK);
  CT_CHECK(v.stick_bottom && view.last == 49);
  CT_CHECK(ccord_vlist_set_count(&v, 75, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 317.5f, 500, frac_measure, nullptr, &view, &err) == CCORD_OK);
  CT_CHECK(v.stick_bottom && view.last == 74);
  ccord_vlist_free(&v);
}

static void test_vlist(void)
{
  ccord_vlist v;
  ccord_vlist_init(&v);
  ccord_error err;
  fake f = {0};
  ccord_vlist_view view;
  // empty list
  CT_CHECK(ccord_vlist_set_count(&v, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 300, 500, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(view.first > view.last);

  // 100000 rows: layout measures only about a viewport worth of rows
  const size_t N = 100000;
  CT_CHECK(ccord_vlist_set_count(&v, N, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&v);
  CT_CHECK(ccord_vlist_layout(&v, 400, 500, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(view.last == N - 1);
  size_t visible = view.last - view.first + 1;
  CT_CHECK(visible >= 400 / 44 && visible <= 400 / 20 + 2);
  CT_CHECK(view.measured <= visible + 8);
  CT_CHECK(v.stick_bottom);
  // last row bottom == viewport bottom
  float bottom = view.first_top;
  for (size_t i = view.first; i <= view.last; i++) bottom += ccord_vlist_row_height(&v, i);
  CT_CHECK(fabsf(bottom - 400.0f) < 0.01f);

  // scrolling up un-sticks; scrolling a long way costs O(rows crossed)
  f.calls = 0;
  ccord_vlist_scroll_by(&v, -1000.0f);
  CT_CHECK(!v.stick_bottom);
  CT_CHECK(ccord_vlist_layout(&v, 400, 500, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(view.last < N - 1 && view.measured <= visible + 8 + 1000 / 20 + 2);
  size_t first_after = view.first;
  CT_CHECK(first_after < N - visible);

  // same position again: nothing is measured (cache hit)
  f.calls = 0;
  CT_CHECK(ccord_vlist_layout(&v, 400, 500, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(f.calls == 0 && view.measured == 0);

  // width change invalidates the cache lazily
  ccord_vlist_invalidate_all(&v);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(f.calls > 0 && view.measured <= visible + 8);

  // scrolling back down to the end re-sticks
  ccord_vlist_scroll_by(&v, 100000.0f);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(v.stick_bottom && view.last == N - 1);
  // scrolling past the end is clamped
  ccord_vlist_scroll_by(&v, 5000.0f);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(view.last == N - 1);

  // scroll to the very top: clamps at row 0, first_top == 0
  for (int i = 0; i < 4000 && view.first > 0; i++) {
    ccord_vlist_scroll_by(&v, -2000.0f);
    CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
    if (i > 3000) break;
  }
  ccord_vlist_scroll_by(&v, -1.0e7f);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(view.first == 0 && fabsf(view.first_top) < 0.01f);

  // stick-bottom follows appends
  ccord_vlist_reset_bottom(&v);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_set_count(&v, N + 3, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(view.last == N + 2 && v.stick_bottom);
  // ...but a scrolled-up list stays put
  ccord_vlist_scroll_by(&v, -500.0f);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  size_t keep = view.first;
  CT_CHECK(ccord_vlist_set_count(&v, N + 10, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(view.first == keep && !v.stick_bottom);

  // short content is bottom-aligned
  ccord_vlist_free(&v);
  ccord_vlist_init(&v);
  CT_CHECK(ccord_vlist_set_count(&v, 3, &err) == CCORD_OK);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  CT_CHECK(view.first == 0 && view.last == 2 && view.first_top > 200.0f);
  float th, ts;
  ccord_vlist_thumb(&v, 400, &th, &ts);
  CT_CHECK(ts == 1.0f && th == 0.0f);
  ccord_vlist_free(&v);

  // thumb stays inside the track for a long list
  ccord_vlist_init(&v);
  CT_CHECK(ccord_vlist_set_count(&v, 5000, &err) == CCORD_OK);
  ccord_vlist_reset_bottom(&v);
  CT_CHECK(ccord_vlist_layout(&v, 400, 640, fake_measure, &f, &view, &err) == CCORD_OK);
  ccord_vlist_thumb(&v, 400, &th, &ts);
  CT_CHECK(ts > 0.0f && ts < 0.1f && th >= 0.0f && th + ts <= 1.0001f && th + ts > 0.99f);
  // set_count refuses nothing silently: layout before set_count reports an error
  ccord_vlist w;
  ccord_vlist_init(&w);
  w.count = 5;
  CT_CHECK(ccord_vlist_layout(&w, 100, 100, fake_measure, &f, &view, &err) == CCORD_ERR_INVALID);
  ccord_vlist_free(&v);
}

// ---- text edit ----

static void test_text_edit(void)
{
  ccord_text_edit t;
  ccord_text_edit_init(&t, 16);
  ccord_error err;
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "") == 0);
  CT_CHECK(ccord_text_edit_insert(&t, "ab", 2, &err) == CCORD_OK);
  CT_CHECK(ccord_text_edit_insert(&t, "\xC3\xA9", 2, &err) == CCORD_OK);   // é
  CT_CHECK(ccord_text_edit_insert(&t, "\xE2\x82\xAC", 3, &err) == CCORD_OK);   // €
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "ab\xC3\xA9\xE2\x82\xAC") == 0 && t.caret == 7);
  ccord_text_edit_move(&t, -1, false);
  CT_CHECK(t.caret == 4);
  ccord_text_edit_move(&t, -1, false);
  CT_CHECK(t.caret == 2);
  ccord_text_edit_backspace(&t);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "a\xC3\xA9\xE2\x82\xAC") == 0 && t.caret == 1);
  ccord_text_edit_delete(&t);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "a\xE2\x82\xAC") == 0);
  // invalid UTF-8 is rejected without change
  CT_CHECK(ccord_text_edit_insert(&t, "\xC3", 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "a\xE2\x82\xAC") == 0);
  // selection replace
  ccord_text_edit_select_all(&t);
  char *sel = ccord_text_edit_copy_selection(&t);
  CT_CHECK(sel && strcmp(sel, "a\xE2\x82\xAC") == 0);
  ccord_free(sel);
  CT_CHECK(ccord_text_edit_insert(&t, "xyz", 3, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "xyz") == 0);
  CT_CHECK(ccord_text_edit_copy_selection(&t) == nullptr);
  // home / end / extend
  ccord_text_edit_move(&t, -2, false);
  CT_CHECK(t.caret == 0);
  ccord_text_edit_move(&t, 2, true);
  CT_CHECK(t.caret == 3 && t.anchor == 0);
  ccord_text_edit_move(&t, -1, false);   // collapses to the left edge
  CT_CHECK(t.caret == 0 && t.anchor == 0);
  // max_bytes: nothing inserted past the limit
  ccord_text_edit_clear(&t);
  CT_CHECK(ccord_text_edit_insert(&t, "0123456789abcdef", 16, &err) == CCORD_OK);
  CT_CHECK(ccord_text_edit_insert(&t, "g", 1, &err) == CCORD_ERR_RANGE);
  CT_CHECK(t.len == 16);
  // replacing a selection frees room
  ccord_text_edit_select_all(&t);
  CT_CHECK(ccord_text_edit_insert(&t, "0123456789abcdefg", 17, &err) == CCORD_ERR_RANGE);
  CT_CHECK(t.len == 16);
  CT_CHECK(ccord_text_edit_insert(&t, "z", 1, &err) == CCORD_OK && t.len == 1);
  // set_caret snaps to a code point boundary
  ccord_text_edit_clear(&t);
  CT_CHECK(ccord_text_edit_insert(&t, "\xC3\xA9x", 3, &err) == CCORD_OK);
  ccord_text_edit_set_caret(&t, 1, false);
  CT_CHECK(t.caret == 0);
  ccord_text_edit_set_caret(&t, 99, false);
  CT_CHECK(t.caret == 3);
  // backspace/delete on boundaries are no-ops
  ccord_text_edit_set_caret(&t, 0, false);
  ccord_text_edit_backspace(&t);
  CT_CHECK(t.len == 3);
  ccord_text_edit_set_caret(&t, 3, false);
  ccord_text_edit_delete(&t);
  CT_CHECK(t.len == 3);
  // OOM leaves the text unchanged
  ccord_text_edit_clear(&t);
  ccord_alloc_test_fail_after(0);
  ccord_text_edit_free(&t);
  ccord_text_edit_init(&t, 16);
  CT_CHECK(ccord_text_edit_insert(&t, "a", 1, &err) == CCORD_ERR_NOMEM);
  ccord_alloc_test_fail_after(-1);
  CT_CHECK(t.len == 0);
  ccord_text_edit_free(&t);

  // Word navigation, word deletion, undo/redo, wrap_selection, smart URL paste, word/line selection
  ccord_text_edit_init(&t, 128);
  CT_CHECK(ccord_text_edit_insert(&t, "hello world\nsecond line", 23, &err) == CCORD_OK);
  // Line home (-4) and line end (+4)
  ccord_text_edit_move(&t, -4, false);
  CT_CHECK(t.caret == 12);
  ccord_text_edit_move(&t, 4, false);
  CT_CHECK(t.caret == 23);
  // Word left (-3) and word right (+3)
  ccord_text_edit_move(&t, -3, false);
  CT_CHECK(t.caret == 19);   // before "line"
  ccord_text_edit_backspace_word(&t);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "hello world\nline") == 0);
  CT_CHECK(ccord_text_edit_undo(&t));
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "hello world\nsecond line") == 0);
  CT_CHECK(ccord_text_edit_redo(&t));
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "hello world\nline") == 0);
  // Select word at offset 1 ("hello") and wrap with "**"
  ccord_text_edit_select_word_at(&t, 1);
  CT_CHECK(t.anchor == 0 && t.caret == 5);
  CT_CHECK(ccord_text_edit_wrap_selection(&t, "**", "**", &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "**hello** world\nline") == 0);
  // Smart URL paste on selected word "world"
  ccord_text_edit_select_word_at(&t, 11);
  CT_CHECK(ccord_text_edit_paste(&t, "https://discord.com", 19, &err) == CCORD_OK);
  CT_CHECK(strcmp(ccord_text_edit_cstr(&t), "**hello** [world](https://discord.com)\nline") == 0);
  // Select line at offset 0
  ccord_text_edit_select_line_at(&t, 2);
  CT_CHECK(t.anchor == 0 && t.caret == 38);
  ccord_text_edit_free(&t);
}

// ---- time formatting ----

static void test_timefmt(void)
{
  // 2026-11-01T12:00:00Z
  const int64_t base = 1793534400000LL;
  char out[64];
  ccord_timefmt_clock(out, sizeof out, base, true);
  CT_CHECK(strcmp(out, "12:00 PM") == 0);
  ccord_timefmt_clock(out, sizeof out, base + 14 * 3600 * 1000LL + 34 * 60 * 1000LL, true);
  CT_CHECK(strcmp(out, "2:34 AM") == 0);   // next day 02:34
  ccord_timefmt_clock(out, sizeof out, base - 12 * 3600 * 1000LL, true);
  CT_CHECK(strcmp(out, "12:00 AM") == 0);
  ccord_timefmt_message(out, sizeof out, base, base + 60000, true);
  CT_CHECK(strcmp(out, "Today at 12:00 PM") == 0);
  ccord_timefmt_message(out, sizeof out, base, base + 24 * 3600 * 1000LL, true);
  CT_CHECK(strcmp(out, "Yesterday at 12:00 PM") == 0);
  ccord_timefmt_message(out, sizeof out, base, base + 3 * 24 * 3600 * 1000LL, true);
  CT_CHECK(strcmp(out, "11/01/2026") == 0);
  // calendar days, not 24 h windows: 23:59 yesterday vs 00:01 today
  ccord_timefmt_message(out, sizeof out, base + 11 * 3600 * 1000LL + 59 * 60 * 1000LL, base + 12 * 3600 * 1000LL + 60 * 1000LL, true);
  CT_CHECK(strncmp(out, "Yesterday at 11:59 PM", 21) == 0);
  // a timestamp in the future falls back to the date
  ccord_timefmt_message(out, sizeof out, base + 2 * 24 * 3600 * 1000LL, base, true);
  CT_CHECK(strcmp(out, "11/03/2026") == 0);
  // truncation never overflows
  char tiny[6];
  size_t n = ccord_timefmt_message(tiny, sizeof tiny, base, base, true);
  CT_CHECK(n == 5 && tiny[5] == '\0');
  CT_CHECK(ccord_timefmt_message(tiny, 0, base, base, true) == 0);

  // same_day, date ("November 1, 2026"), and full ("Sunday, November 1, 2026 12:00 PM")
  CT_CHECK(ccord_timefmt_same_day(base, base + 11 * 3600 * 1000LL + 59 * 60 * 1000LL + 59999LL, true));
  CT_CHECK(!ccord_timefmt_same_day(base + 11 * 3600 * 1000LL + 59 * 60 * 1000LL + 59999LL,
                                   base + 12 * 3600 * 1000LL, true));
  // leap year Feb 28 vs Feb 29 (2024-02-28T23:59:59Z = 1709164799000, 2024-02-29T00:00:00Z = 1709164800000)
  CT_CHECK(!ccord_timefmt_same_day(1709164799000LL, 1709164800000LL, true));
  CT_CHECK(ccord_timefmt_same_day(1709164800000LL, 1709251199000LL, true));
  CT_CHECK(!ccord_timefmt_same_day(1709251199000LL, 1709251200000LL, true));

  size_t dl = ccord_timefmt_date(out, sizeof out, base, true);
  CT_CHECK(dl == strlen("November 1, 2026") && strcmp(out, "November 1, 2026") == 0);
  dl = ccord_timefmt_date(out, sizeof out, 1709164800000LL, true);
  CT_CHECK(strcmp(out, "February 29, 2024") == 0);
  CT_CHECK(ccord_timefmt_date(tiny, sizeof tiny, base, true) == 5 && strcmp(tiny, "Novem") == 0);
  CT_CHECK(ccord_timefmt_date(tiny, 0, base, true) == 0);

  size_t fl = ccord_timefmt_full(out, sizeof out, base, true);
  CT_CHECK(fl == strlen("Sunday, November 1, 2026 12:00 PM") &&
           strcmp(out, "Sunday, November 1, 2026 12:00 PM") == 0);
  ccord_timefmt_full(out, sizeof out, base + 2 * 24 * 3600 * 1000LL + 2 * 3600 * 1000LL + 34 * 60 * 1000LL, true);
  CT_CHECK(strcmp(out, "Tuesday, November 3, 2026 2:34 PM") == 0);
  // pre-1970 negative timestamp
  ccord_timefmt_full(out, sizeof out, -1000LL, true);
  CT_CHECK(strcmp(out, "Wednesday, December 31, 1969 11:59 PM") == 0);
  CT_CHECK(ccord_timefmt_full(tiny, sizeof tiny, base, true) == 5 && strcmp(tiny, "Sunda") == 0);
  CT_CHECK(ccord_timefmt_full(tiny, 0, base, true) == 0);

  // 24-hour / 12-hour / Auto clock modes
  CT_CHECK(ccord_timefmt_get_mode() == CCORD_TIMEFMT_AUTO);
  ccord_timefmt_set_mode(CCORD_TIMEFMT_24H);
  CT_CHECK(ccord_timefmt_get_mode() == CCORD_TIMEFMT_24H);
  ccord_timefmt_clock(out, sizeof out, base, true);
  CT_CHECK(strcmp(out, "12:00") == 0);
  ccord_timefmt_clock(out, sizeof out, base + 14 * 3600 * 1000LL + 34 * 60 * 1000LL, true);
  CT_CHECK(strcmp(out, "02:34") == 0);
  ccord_timefmt_clock(out, sizeof out, base - 12 * 3600 * 1000LL, true);
  CT_CHECK(strcmp(out, "00:00") == 0);
  ccord_timefmt_message(out, sizeof out, base + 2 * 3600 * 1000LL + 5 * 60 * 1000LL, base, true);
  CT_CHECK(strcmp(out, "Today at 14:05") == 0);
  ccord_timefmt_set_mode(CCORD_TIMEFMT_12H);
  ccord_timefmt_clock(out, sizeof out, base + 2 * 3600 * 1000LL + 5 * 60 * 1000LL, true);
  CT_CHECK(strcmp(out, "2:05 PM") == 0);
  ccord_timefmt_set_mode(CCORD_TIMEFMT_AUTO);
}

int main(void)
{
  ccord_theme_metrics m = default_metrics();
  test_layout(&m);
  test_vlist();
  test_vlist_tail_row();
  test_text_edit();
  test_timefmt();
  return CT_RESULT("test_ui_logic");
}
