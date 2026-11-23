// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ftw.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int ccord_test_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(fpath);
}
static char *ccord_test_data_dir = NULL;
static void ccord_test_cleanup_dir(void) {
    if (ccord_test_data_dir) {
        nftw(ccord_test_data_dir, ccord_test_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
    }
}
[[maybe_unused]] static const char *ccord_test_init_dir(const char *prefix) {
    if (ccord_test_data_dir) return ccord_test_data_dir;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    size_t len = strlen(tmp) + strlen(prefix) + 16;
    ccord_test_data_dir = malloc(len);
    if (!ccord_test_data_dir) abort();
    snprintf(ccord_test_data_dir, len, "%s/%s_XXXXXX", tmp, prefix);
    if (!mkdtemp(ccord_test_data_dir)) {
        free(ccord_test_data_dir);
        ccord_test_data_dir = NULL;
        abort();
    }
    atexit(ccord_test_cleanup_dir);
    return ccord_test_data_dir;
}

// Offscreen UI test for Components v2 (message flag IS_COMPONENTS_V2): the tree renders as one card stack under the
// message, its buttons and selects dispatch interactions by custom_id, and the message's own attachments / embeds /
// stickers / poll stay suppressed (the attachments only feed attachment:// media inside the tree).
// Real store + mock transport + session + renderer, fake OS host, injected input events (same harness as
// test_ui_interaction.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <ccord/core/alloc.h>
#include <ccord/core/paths.h>
#include <ccord/model/rich.h>
#include <ccord/net/admin.h>
#include <ccord/net/mock_transport.h>
#include <ccord/platform/platform.h>
#include <ccord/ui/ui.h>

#include "../ui_internal.h"

#include "ccord_test.h"

typedef struct fixture {
  ccord_theme theme;
  ccord_bus *bus;
  ccord_store *store;
  ccord_session *session;
  ccord_transport *transport;
  ccord_contrib *contrib;
  ccord_platform *platform;
  ccord_renderer *renderer;
  ccord_ui *ui;
  uint64_t now;
  int open_url_calls;
  char last_url[512];
  ccord_id lab_ch;
} fixture;

static fixture F;

static uint64_t fake_clock(void *ud)
{
  (void)ud;
  return F.now;
}

static void host_clip_set(void *ud, const char *s)
{
  (void)ud, (void)s;
}

static void host_text_input(void *ud, bool active, float x, float y, float w, float h)
{
  (void)ud, (void)active, (void)x, (void)y, (void)w, (void)h;
}

static bool host_open_url(void *ud, const char *url)
{
  (void)ud;
  F.open_url_calls++;
  snprintf(F.last_url, sizeof F.last_url, "%s", url ? url : "");
  return true;
}

static void frame(int n)
{
  for (int i = 0; i < n; i++) {
    F.now += 50;
    (void)ccord_session_pump(F.session);
    (void)ccord_bus_drain(F.bus);
    (void)ccord_ui_update(F.ui, F.now);
    ccord_render_begin(F.renderer, F.theme.c.bg_primary);
    ccord_ui_draw(F.ui, F.renderer);
    ccord_render_end(F.renderer);
  }
}

static void send(ccord_input_event ev)
{
  (void)ccord_ui_handle_input(F.ui, &ev);
}

static void mouse(ccord_input_type t, uint8_t button, float x, float y)
{
  send((ccord_input_event){.type = t, .button = button, .clicks = 1, .x = x, .y = y});
}

static void click(float x, float y)
{
  mouse(CCORD_IN_MOUSE_MOVE, 0, x, y);
  mouse(CCORD_IN_MOUSE_DOWN, 1, x, y);
  mouse(CCORD_IN_MOUSE_UP, 1, x, y);
  frame(2);
}

static void key(ccord_key k, uint8_t mods)
{
  send((ccord_input_event){.type = CCORD_IN_KEY_DOWN, .key = k, .mods = mods});
}

static ccord_ui_debug dbg(void)
{
  ccord_ui_debug d;
  ccord_ui_get_debug(F.ui, &d);
  return d;
}

static bool setup(const char *argv0)
{
  ccord_error err;
  ccord_capture_set_fake(true);
  if (ccord_paths_init(argv0, &err) != CCORD_OK) return false;
    ccord_paths_set_data_dir(ccord_test_init_dir("ui_cv2_test_data"));
remove("ui_cv2_test_data/settings.ini");
  remove("ui_cv2_test_data/drafts.json");
  remove("ui_cv2_test_data");
  char path[1024];
  if (ccord_paths_join(path, sizeof path, ccord_paths_asset_dir(), "themes/c-cord-dark.theme", &err) != CCORD_OK) return false;
  if (ccord_theme_load(&F.theme, path, &err) != CCORD_OK) return false;
  F.bus = ccord_bus_create(&err);
  F.store = ccord_store_create(F.bus, &err);
  F.contrib = ccord_contrib_create(&err);
  ccord_mock_config mc = {.big_channel_messages = 3000, .now_ms = fake_clock};
  F.transport = ccord_mock_transport_create(&mc, &err);
  F.session = ccord_session_create(F.bus, F.store, F.transport, &err);
  if (!F.bus || !F.store || !F.contrib || !F.transport || !F.session) return false;
  ccord_platform_config pc = {.title = "ui-cv2-test", .width = 1280, .height = 800, .headless = true};
  F.platform = ccord_platform_create(&pc, &err);
  if (!F.platform) {
    fprintf(stderr, "skip: %s\n", err.message);
    return false;
  }
  F.renderer = ccord_render_create(F.platform, &F.theme, &err);
  if (!F.renderer) return false;
  ccord_ui_deps d = {.bus = F.bus, .store = F.store, .session = F.session, .contrib = F.contrib, .theme = &F.theme,
                     .host = {.clipboard_set = host_clip_set, .text_input = host_text_input, .open_url = host_open_url},
                     .fixed_now_unix_ms = 1793534400000LL + 30 * 60 * 1000};
  F.ui = ccord_ui_create(&d, &err);
  if (!F.ui) return false;
  ccord_ui_set_viewport(F.ui, 1280, 800);
  if (ccord_session_start(F.session, &err) != CCORD_OK) return false;
  frame(2);

  // A dedicated empty channel in C-Cord HQ (2001) both the mock and the store know about.
  ccord_admin_request areq;
  if (ccord_admin_create_channel(&areq, 2001, "cv2-lab", 0, 3110, nullptr, nullptr, &err) != CCORD_OK) return false;
  if (ccord_session_admin(F.session, &areq, &err) != CCORD_OK) return false;
  frame(3);
  for (size_t i = 0; i < ccord_store_channel_count(F.store, 2001); i++) {
    const ccord_channel *c = ccord_store_channel_at(F.store, 2001, i);
    if (c && strcmp(c->name, "cv2-lab") == 0) {
      F.lab_ch = c->id;
      break;
    }
  }
  if (F.lab_ch == CCORD_ID_NONE) return false;
  if (ccord_session_select_channel(F.session, F.lab_ch, &err) != CCORD_OK) return false;
  frame(3);
  return true;
}

// The cached timeline height of a message's row (the measure pass ran while it was on screen).
static float row_height(ccord_id mid)
{
  size_t n = ccord_store_message_count(F.store, F.lab_ch);
  for (size_t i = 0; i < n; i++) {
    const ccord_message *m = ccord_store_message_at(F.store, F.lab_ch, i);
    if (m && m->id == mid) return ccord_vlist_row_height(&F.ui->vlist, i);
  }
  return -1.0f;
}

static const ui_comp_hit *find_hit(const char *custom_id)
{
  for (size_t i = 0; i < F.ui->ncomp_hits; i++) {
    if (strcmp(F.ui->comp_hits[i].custom_id, custom_id) == 0) return &F.ui->comp_hits[i];
  }
  return nullptr;
}

static void click_hit(const char *custom_id)
{
  const ui_comp_hit *h = find_hit(custom_id);
  CT_CHECK(h != nullptr);
  if (!h) return;
  CT_CHECK(h->rect.w > 0.0f && h->rect.h > 0.0f);
  click(h->rect.x + h->rect.w * 0.5f, h->rect.y + h->rect.h * 0.5f);
}

static ccord_id post_v2(const ccord_message_rich *rich)
{
  ccord_error err;
  ccord_id mid = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(F.transport, F.lab_ch, 1011, "", rich, &mid, &err) == CCORD_OK);
  frame(3);
  CT_CHECK(mid != CCORD_ID_NONE);
  return mid;
}

static void remove_message(ccord_id id)
{
  ccord_error err;
  CT_CHECK(ccord_store_delete_message(F.store, F.lab_ch, id, &err) == CCORD_OK);
  frame(2);
}

// All seven v2 kinds in one message: a Container (accent bar) holding TextDisplay, Section + Thumbnail, Separator,
// MediaGallery, File and an ActionRow with a button, a select and a disabled button; outside it a TextDisplay and a
// Section with a Button accessory. The message also carries attachments, an embed, a sticker and a poll, none of which
// may draw on their own: the attachments only feed the tree's attachment:// media.
static void test_v2_rendering(void)
{
  ccord_select_option sel_opts[2] = {
    {.label = "Alpha", .value = "v2_alpha"},
    {.label = "Beta", .value = "v2_beta"},
  };
  ccord_component row_items[3] = {
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Nested", .custom_id = "v2:btn"},
    {.kind = CCORD_COMP_STRING_SELECT, .custom_id = "v2:sel", .placeholder = "Pick one",
     .min_values = 1, .max_values = 1, .options = sel_opts, .option_count = 2},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_DANGER, .label = "Locked", .custom_id = "v2:off", .disabled = true},
  };
  ccord_component sec_text = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "Section **body** text"};
  ccord_component thumb = {.kind = CCORD_COMP_THUMBNAIL,
                           .media = {.url = "https://cdn.discordapp.com/attachments/1/thumb.png",
                                     .content_type = "image/png", .width = 128, .height = 128}};
  ccord_component section = {.kind = CCORD_COMP_SECTION, .children = &sec_text, .child_count = 1, .accessory = &thumb};
  ccord_component heading = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "# Components v2"};
  ccord_component sep = {.kind = CCORD_COMP_SEPARATOR, .divider = true, .spacing = 2};
  ccord_media_ref gallery[2] = {
    {.url = "attachment://shot.png", .content_type = "image/png", .width = 320, .height = 200},
    {.url = "https://cdn.discordapp.com/attachments/1/g2.png", .content_type = "image/png", .width = 320, .height = 200},
  };
  ccord_component media = {.kind = CCORD_COMP_MEDIA_GALLERY, .gallery = gallery, .gallery_count = 2};
  ccord_component file = {.kind = CCORD_COMP_FILE, .file_name = "report.pdf", .file_size = 2048,
                          .media = {.url = "attachment://report.pdf", .content_type = "application/pdf"}};
  ccord_component row = {.kind = CCORD_COMP_ACTION_ROW, .children = row_items, .child_count = 3};
  ccord_component kids[6] = {heading, section, sep, media, file, row};
  ccord_component container = {.kind = CCORD_COMP_CONTAINER, .children = kids, .child_count = 6,
                               .accent_color = 0x5865F2u, .has_accent_color = true};
  ccord_component out_text = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "outside the container"};
  ccord_component acc_btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_SECONDARY, .label = "Accessory",
                             .custom_id = "v2:accbtn"};
  ccord_component out_sec_text = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "button accessory section"};
  ccord_component out_section = {.kind = CCORD_COMP_SECTION, .children = &out_sec_text, .child_count = 1,
                                 .accessory = &acc_btn};
  ccord_component top[3] = {container, out_text, out_section};

  ccord_attachment atts[2] = {
    {.id = 9101, .filename = "shot.png", .content_type = "image/png",
     .url = "https://cdn.discordapp.com/attachments/1/shot.png", .width = 320, .height = 200},
    {.id = 9102, .filename = "report.pdf", .content_type = "application/pdf",
     .url = "https://cdn.discordapp.com/attachments/1/report.pdf", .size = 2048},
  };
  ccord_embed emb = {.title = "must not draw"};
  ccord_msg_sticker stk = {.id = 8001, .name = "WumpusHi", .format = 1};
  ccord_message_rich rich = {
    .present = CCORD_RICH_ALL,
    .flags = CCORD_MSGFLAG_IS_COMPONENTS_V2,
    .application_id = 888001,
    .attachments = atts, .attachment_count = 2,
    .embeds = &emb, .embed_count = 1,
    .stickers = &stk, .sticker_count = 1,
    .poll = {.present = true, .question = "must not draw?", .answer_count = 1,
             .answers = (ccord_poll_answer[]){{.id = 1, .text = "no"}}},
    .v2_components = top, .v2_component_count = 3,
  };
  ccord_id plain_mid = post_v2(&(ccord_message_rich){.present = CCORD_RICH_ALL});   // a text-less baseline row
  ccord_id mid = post_v2(&rich);
  ccord_ui_debug d = dbg();

  // One v2 part: the gallery and the file card draw, the message-level media does not draw a second time.
  CT_CHECK(d.gallery_cards == 1);
  CT_CHECK(d.attachment_cards == 3);          // 2 gallery tiles + 1 file card, nothing else
  CT_CHECK(d.embed_cards == 0 && d.sticker_cards == 0 && d.poll_cards == 0);
  CT_CHECK(d.component_rows_drawn == 1 && d.component_buttons_drawn == 3 && d.component_selects_drawn == 1);
  CT_CHECK(d.component_rect_count == 4);

  // All seven kinds (and the ActionRow) went through the walker with a laid-out height.
  const ccord_component_kind kinds[] = {CCORD_COMP_CONTAINER, CCORD_COMP_SECTION, CCORD_COMP_TEXT_DISPLAY,
                                        CCORD_COMP_THUMBNAIL, CCORD_COMP_MEDIA_GALLERY, CCORD_COMP_FILE,
                                        CCORD_COMP_SEPARATOR, CCORD_COMP_ACTION_ROW};
  for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
    CT_CHECK(F.ui->dbg_cv2_drawn[kinds[i]] >= 1);
    CT_CHECK(F.ui->dbg_cv2_height[kinds[i]] > 0.0f);
  }
  // The Section accessory button (the row's buttons are drawn by the row), and the four TextDisplays.
  CT_CHECK(F.ui->dbg_cv2_drawn[CCORD_COMP_BUTTON] == 1);
  CT_CHECK(F.ui->dbg_cv2_drawn[CCORD_COMP_TEXT_DISPLAY] == 4);
  CT_CHECK(F.ui->dbg_cv2_height[CCORD_COMP_SEPARATOR] == 16.0f);   // spacing 2 (large)

  // Every kind laid out: the v2 row is far taller than the empty baseline row.
  float plain_h = row_height(plain_mid);
  float v2_h = row_height(mid);
  CT_CHECK(plain_h > 0.0f && v2_h > plain_h + 200.0f);

  // Thumbnail + both gallery tiles are clickable images; the gallery resolved attachment://shot.png.
  CT_CHECK(F.ui->nimg_hits == 3);
  CT_CHECK(strcmp(F.ui->img_hits[1].urls[0], "https://cdn.discordapp.com/attachments/1/shot.png") == 0);
  CT_CHECK(F.ui->img_hits[1].count == 2);
  CT_CHECK(strcmp(F.ui->img_hits[2].urls[1], "https://cdn.discordapp.com/attachments/1/g2.png") == 0);
  // The file card keeps its download link.
  bool file_link = false;
  for (size_t i = 0; i < F.ui->nlinks; i++) {
    if (strcmp(F.ui->links[i].url, "https://cdn.discordapp.com/attachments/1/report.pdf") == 0) file_link = true;
  }
  CT_CHECK(file_link);

  // The hits carry the v2 flag and no v1 row index; the select kept its bounds and placeholder.
  const ui_comp_hit *btn = find_hit("v2:btn");
  const ui_comp_hit *sel = find_hit("v2:sel");
  CT_CHECK(btn && sel);
  if (btn && sel) {
    CT_CHECK((btn->message_flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) != 0);
    CT_CHECK(btn->row_idx == SIZE_MAX && btn->application_id == 888001);
    CT_CHECK(sel->min_values == 1 && sel->max_values == 1);
    CT_CHECK(strcmp(sel->placeholder, "Pick one") == 0);
  }

  // Clicking the gallery tile opens the lightbox with both resolved urls.
  click(F.ui->img_hits[1].rect.x + 10.0f, F.ui->img_hits[1].rect.y + 10.0f);
  d = dbg();
  CT_CHECK(d.lightbox_open && d.lightbox_count == 2);
  CT_CHECK(strcmp(d.lightbox_url, "https://cdn.discordapp.com/attachments/1/shot.png") == 0);
  key(CCORD_KEY_ESCAPE, 0);
  frame(2);
  CT_CHECK(!dbg().lightbox_open);

  remove_message(mid);
  remove_message(plain_mid);
}

// Nested buttons and selects dispatch through the same interaction path as v1, addressed by custom_id.
static void test_v2_interactions(void)
{
  ccord_select_option sel_opts[2] = {
    {.label = "Alpha", .value = "v2_alpha"},
    {.label = "Beta", .value = "v2_beta"},
  };
  ccord_component row_items[2] = {
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Nested", .custom_id = "v2:btn"},
    {.kind = CCORD_COMP_STRING_SELECT, .custom_id = "v2:sel", .placeholder = "Pick one",
     .min_values = 1, .max_values = 1, .options = sel_opts, .option_count = 2},
  };
  ccord_component row = {.kind = CCORD_COMP_ACTION_ROW, .children = row_items, .child_count = 2};
  ccord_component container = {.kind = CCORD_COMP_CONTAINER, .children = &row, .child_count = 1};
  ccord_component acc_btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_SUCCESS, .label = "Go",
                             .custom_id = "v2:accbtn"};
  ccord_component sec_text = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "accessory"};
  ccord_component section = {.kind = CCORD_COMP_SECTION, .children = &sec_text, .child_count = 1, .accessory = &acc_btn};
  ccord_component top[2] = {container, section};
  ccord_message_rich rich = {.present = CCORD_RICH_ALL, .flags = CCORD_MSGFLAG_IS_COMPONENTS_V2,
                             .application_id = 888001, .v2_components = top, .v2_component_count = 2};
  ccord_id mid = post_v2(&rich);
  size_t ic0 = ccord_mock_interaction_count(F.transport);

  // The button nested in Container > ActionRow sends its custom_id.
  click_hit("v2:btn");
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);
  CT_CHECK(ccord_mock_last_interaction_type(F.transport) == CCORD_INTERACTION_MESSAGE_COMPONENT);
  CT_CHECK(ccord_mock_last_interaction_component_type(F.transport) == CCORD_COMP_BUTTON);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "v2:btn") == 0);

  // The Section accessory button too.
  click_hit("v2:accbtn");
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 2);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "v2:accbtn") == 0);

  // The nested select opens with its own options (found by custom_id, not by v1 row index); picking one submits.
  click_hit("v2:sel");
  ccord_ui_debug d = dbg();
  CT_CHECK(d.select_dropdown_open && d.select_dropdown_option_count == 2);
  click(d.select_option_rects[1].x + 20.0f, d.select_option_rects[1].y + 12.0f);
  CT_CHECK(!dbg().select_dropdown_open);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 3);
  CT_CHECK(ccord_mock_last_interaction_component_type(F.transport) == CCORD_COMP_STRING_SELECT);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "v2:sel") == 0);
  CT_CHECK(ccord_mock_last_interaction_value_count(F.transport) == 1);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(F.transport, 0), "v2_beta") == 0);
  remove_message(mid);

  // A multi-select nested in a Container: two picks + Apply send both values.
  ccord_component multi = {.kind = CCORD_COMP_STRING_SELECT, .custom_id = "v2:multi", .placeholder = "Pick two",
                           .min_values = 2, .max_values = 2, .options = sel_opts, .option_count = 2};
  ccord_component mrow = {.kind = CCORD_COMP_ACTION_ROW, .children = &multi, .child_count = 1};
  ccord_component mtop = {.kind = CCORD_COMP_CONTAINER, .children = &mrow, .child_count = 1};
  ccord_message_rich mrich = {.present = CCORD_RICH_ALL, .flags = CCORD_MSGFLAG_IS_COMPONENTS_V2,
                              .application_id = 888001, .v2_components = &mtop, .v2_component_count = 1};
  mid = post_v2(&mrich);
  click_hit("v2:multi");
  d = dbg();
  CT_CHECK(d.select_dropdown_open && d.select_dropdown_option_count == 2 && d.select_confirm_btn.w > 0.0f);
  click(d.select_option_rects[0].x + 20.0f, d.select_option_rects[0].y + 12.0f);
  click(dbg().select_option_rects[1].x + 20.0f, dbg().select_option_rects[1].y + 12.0f);
  click(dbg().select_confirm_btn.x + 16.0f, dbg().select_confirm_btn.y + 10.0f);
  CT_CHECK(!dbg().select_dropdown_open);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 4);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "v2:multi") == 0);
  CT_CHECK(ccord_mock_last_interaction_value_count(F.transport) == 2);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(F.transport, 0), "v2_alpha") == 0);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(F.transport, 1), "v2_beta") == 0);
  remove_message(mid);
}

// Spoiler containers and files hide behind a spoiler tile until clicked; v1 rows keep rendering exactly as before.
static void test_v2_spoilers_and_v1_regression(void)
{
  ccord_component hidden_btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Revealed",
                                .custom_id = "v2:hidden"};
  ccord_component hrow = {.kind = CCORD_COMP_ACTION_ROW, .children = &hidden_btn, .child_count = 1};
  ccord_component htext = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "secret text"};
  ccord_component hkids[2] = {htext, hrow};
  ccord_component hidden_box = {.kind = CCORD_COMP_CONTAINER, .children = hkids, .child_count = 2, .spoiler = true};
  ccord_component secret_file = {.kind = CCORD_COMP_FILE, .file_name = "secret.pdf", .file_size = 99,
                                 .spoiler = true,
                                 .media = {.url = "attachment://secret.pdf", .content_type = "application/pdf"}};
  ccord_component top[2] = {hidden_box, secret_file};
  ccord_attachment att = {.id = 9201, .filename = "secret.pdf", .content_type = "application/pdf",
                          .url = "https://cdn.discordapp.com/attachments/1/secret.pdf", .size = 99};
  ccord_message_rich rich = {.present = CCORD_RICH_ALL, .flags = CCORD_MSGFLAG_IS_COMPONENTS_V2,
                             .application_id = 888001, .attachments = &att, .attachment_count = 1,
                             .v2_components = top, .v2_component_count = 2};
  ccord_id mid = post_v2(&rich);
  CT_CHECK(dbg().component_buttons_drawn == 0 && dbg().attachment_cards == 0);
  CT_CHECK(F.ui->nspoiler_hits == 2);

  // Reveal both tiles; the container's button and the file card appear.
  click(F.ui->spoiler_hits[0].rect.x + 10.0f, F.ui->spoiler_hits[0].rect.y + 10.0f);
  click(F.ui->spoiler_hits[0].rect.x + 10.0f, F.ui->spoiler_hits[0].rect.y + 10.0f);
  CT_CHECK(dbg().component_buttons_drawn == 1 && dbg().attachment_cards == 1);
  size_t ic0 = ccord_mock_interaction_count(F.transport);
  click_hit("v2:hidden");
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 1);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "v2:hidden") == 0);
  remove_message(mid);

  // v1 regression: an ordinary action row still draws and dispatches by row index.
  ccord_select_option opts[2] = {{.label = "One", .value = "v1_one"}, {.label = "Two", .value = "v1_two"}};
  ccord_component items[2] = {
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "V1", .custom_id = "v1:btn"},
    {.kind = CCORD_COMP_STRING_SELECT, .custom_id = "v1:sel", .options = opts, .option_count = 2,
     .min_values = 1, .max_values = 1},
  };
  ccord_component_row v1row = {.components = items, .component_count = 2};
  ccord_message_rich v1rich = {.present = CCORD_RICH_ALL, .application_id = 888001,
                               .components = &v1row, .component_row_count = 1};
  mid = post_v2(&v1rich);
  ccord_ui_debug d = dbg();
  CT_CHECK(d.component_rows_drawn == 1 && d.component_buttons_drawn == 1 && d.component_selects_drawn == 1);
  const ui_comp_hit *h = find_hit("v1:btn");
  CT_CHECK(h && h->row_idx == 0 && (h->message_flags & CCORD_MSGFLAG_IS_COMPONENTS_V2) == 0);
  click_hit("v1:btn");
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 2);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "v1:btn") == 0);
  click_hit("v1:sel");
  d = dbg();
  CT_CHECK(d.select_dropdown_open && d.select_dropdown_option_count == 2);
  click(d.select_option_rects[0].x + 20.0f, d.select_option_rects[0].y + 12.0f);
  CT_CHECK(ccord_mock_interaction_count(F.transport) == ic0 + 3);
  CT_CHECK(strcmp(ccord_mock_last_interaction_custom_id(F.transport), "v1:sel") == 0);
  CT_CHECK(strcmp(ccord_mock_last_interaction_value(F.transport, 0), "v1_one") == 0);
  remove_message(mid);
}

// Spoiler reveal keys are per node: a hidden Container does not reveal its own first child when it opens, and two
// galleries in one message reveal independently.
static void test_v2_spoiler_keys(void)
{
  ccord_component inner_file = {.kind = CCORD_COMP_FILE, .file_name = "inner.bin", .file_size = 7, .spoiler = true,
                                .media = {.url = "https://cdn.discordapp.com/attachments/1/inner.bin",
                                          .content_type = "application/octet-stream"}};
  ccord_component box = {.kind = CCORD_COMP_CONTAINER, .children = &inner_file, .child_count = 1, .spoiler = true};
  ccord_media_ref g1[1] = {{.url = "https://cdn.discordapp.com/attachments/1/s1.png", .content_type = "image/png",
                            .width = 64, .height = 64, .spoiler = true}};
  ccord_media_ref g2[1] = {{.url = "https://cdn.discordapp.com/attachments/1/s2.png", .content_type = "image/png",
                            .width = 64, .height = 64, .spoiler = true}};
  ccord_component gal1 = {.kind = CCORD_COMP_MEDIA_GALLERY, .gallery = g1, .gallery_count = 1};
  ccord_component gal2 = {.kind = CCORD_COMP_MEDIA_GALLERY, .gallery = g2, .gallery_count = 1};
  ccord_component top[3] = {box, gal1, gal2};
  ccord_message_rich rich = {.present = CCORD_RICH_ALL, .flags = CCORD_MSGFLAG_IS_COMPONENTS_V2,
                             .application_id = 888001, .v2_components = top, .v2_component_count = 3};
  ccord_id mid = post_v2(&rich);
  CT_CHECK(F.ui->nspoiler_hits == 3);   // the container tile + one tile per gallery
  CT_CHECK(F.ui->dbg_cv2_drawn[CCORD_COMP_FILE] == 0);   // the file is inside the still-hidden container

  // Reveal the container: its file is still behind its own tile.
  click(F.ui->spoiler_hits[0].rect.x + 10.0f, F.ui->spoiler_hits[0].rect.y + 10.0f);
  CT_CHECK(F.ui->nspoiler_hits == 3);   // file tile + two gallery tiles
  // Reveal the first gallery: the second stays hidden.
  size_t before = F.ui->nspoiler_hits;
  click(F.ui->spoiler_hits[1].rect.x + 10.0f, F.ui->spoiler_hits[1].rect.y + 10.0f);
  CT_CHECK(F.ui->nspoiler_hits == before - 1u);
  remove_message(mid);
}

int main(int argc, char **argv)
{
  if (!setup(argc > 0 ? argv[0] : "test_ui_components_v2")) {
    fprintf(stderr, "ui components v2 test: setup failed or no display driver; skipping\n");
    return 77;
  }
  test_v2_rendering();
  test_v2_interactions();
  test_v2_spoilers_and_v1_regression();
  test_v2_spoiler_keys();
  ccord_ui_destroy(F.ui);
  ccord_render_destroy(F.renderer);
  ccord_platform_destroy(F.platform);
  ccord_session_destroy(F.session);
  ccord_contrib_destroy(F.contrib);
  ccord_store_destroy(F.store);
  ccord_bus_destroy(F.bus);
  remove("ui_cv2_test_data/settings.ini");
  remove("ui_cv2_test_data/drafts.json");
  remove("ui_cv2_test_data");
  return CT_RESULT("test_ui_components_v2");
}
