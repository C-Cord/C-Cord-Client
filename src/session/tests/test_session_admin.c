// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_test_harness.h"

static void test_session_m4a_guild_settings_safety_vanity_and_ownership(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 43, .big_channel_messages = 5, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  // 1. Local validation on CCORD_ADMIN_EDIT_GUILD before hitting transport:
  // system channel must exist in this guild and be plain text (not voice 3121, forum 3116, thread, or foreign 3211).
  size_t a0 = ccord_mock_admin_count(tr);
  ccord_admin_request req;
  ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_system_channel = true;
  ge.system_channel_id = 999999;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_NOT_FOUND);

  ge.system_channel_id = 3211; // foreign guild 2002
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  ge.system_channel_id = 3121; // voice channel
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  ge.system_channel_id = 3115; // forum channel (#design-forum)
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  // AFK channel must exist in this guild and be non-stage voice (not text 3111, foreign, or stage).
  ge = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_afk_channel = true;
  ge.afk_channel_id = 999999;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_NOT_FOUND);

  ge.afk_channel_id = 3111; // text channel
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  ccord_channel stage_ch = {
    .id = 3199, .guild_id = 2001, .kind = CCORD_CHANNEL_VOICE,
    .discord_type = CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE, .name = "Stage", .topic = "",
  };
  CT_CHECK(ccord_store_upsert_channel(store, &stage_ch, &err) == CCORD_OK);
  ge.afk_channel_id = 3199;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_mock_admin_count(tr) == a0); // none of those invalid requests reached Discord!

  // Valid extended edit + incident actions + vanity URL flow through session into store.
  ge = (ccord_admin_guild_edit)CCORD_ADMIN_GUILD_EDIT_INIT;
  ge.has_system_channel = true;
  ge.system_channel_id = 3112;
  ge.has_afk_channel = true;
  ge.afk_channel_id = 3121;
  ge.afk_timeout = 900;
  ge.verification_level = 2;
  ge.explicit_content_filter = 1;
  CT_CHECK(ccord_admin_edit_guild_ex(&req, 2001, &ge, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_set_incident_actions(&req, 2001, 1800005000000LL, 0, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_set_vanity_url(&req, 2001, "ccord-session", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  const ccord_guild *g2001 = ccord_store_guild(store, 2001);
  CT_CHECK(g2001 && g2001->system_channel_id == 3112 && g2001->afk_channel_id == 3121 &&
           g2001->afk_timeout == 900 && g2001->verification_level == 2 && g2001->explicit_content_filter == 1 &&
           g2001->invites_disabled_until_ms == 1800005000000LL &&
           strcmp(g2001->vanity_url_code, "ccord-session") == 0);

  // 2. Owner guards in session: create an owned server (owner_id == 1001).
  CT_CHECK(ccord_admin_create_guild(&req, "Session Owned", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  ccord_id owned_gid = ccord_store_selected_guild(store);
  CT_CHECK(owned_gid != 2001 && ccord_store_guild(store, owned_gid)->owner_id == 1001);

  size_t a1 = ccord_mock_admin_count(tr);
  // Owner cannot leave their own server; cannot transfer ownership to self.
  CT_CHECK(ccord_admin_leave_guild(&req, owned_gid, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  req.op = CCORD_ADMIN_TRANSFER_OWNERSHIP;
  req.guild_id = owned_gid;
  req.user_id = 1001;
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_INVALID);

  // Non-owner (on guild 2002 where owner_id == 1002 != 1001) cannot delete or transfer ownership.
  ccord_guild g2002_owned = {.id = 2002, .name = "Lua Lounge", .owner_id = 1002};
  CT_CHECK(ccord_store_upsert_guild(store, &g2002_owned, nullptr, 0, nullptr, 0, nullptr, 0, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_delete_guild(&req, 2002, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_transfer_ownership(&req, 2002, 1003, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == a1);

  // Owner CAN delete owned_gid; non-owner CAN leave 2003.
  CT_CHECK(ccord_admin_delete_guild(&req, owned_gid, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_leave_guild(&req, 2003, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_guild(store, owned_gid) == nullptr && ccord_store_guild(store, 2003) == nullptr);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_m4b_expressions(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_transport *tr = ccord_mock_transport_create(nullptr, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  // SNAPSHOT populated guild 2001 expressions, without admin_loaded or uploader IDs.
  CT_CHECK(ccord_store_guild_emoji_count(store, 2001) >= 2 && !ccord_store_guild_emojis_admin_loaded(store, 2001));
  CT_CHECK(ccord_store_guild_sticker_count(store, 2001) >= 2 && !ccord_store_guild_stickers_admin_loaded(store, 2001));
  CT_CHECK(ccord_store_guild_soundboard_count(store, 2001) >= 2 && !ccord_store_guild_soundboard_admin_loaded(store, 2001));

  ccord_admin_request req;
  CT_CHECK(ccord_admin_list_emojis(&req, 2001, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_list_stickers(&req, 2001, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_list_soundboard(&req, 2001, &err) == CCORD_OK && ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_guild_emojis_admin_loaded(store, 2001) &&
           ccord_store_guild_stickers_admin_loaded(store, 2001) &&
           ccord_store_guild_soundboard_admin_loaded(store, 2001));

  // Find one emoji uploaded by self (1001) and one uploaded by Ada (1002).
  ccord_id own_em = CCORD_ID_NONE, ada_em = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_guild_emoji_count(store, 2001); i++) {
    const ccord_emoji *e = ccord_store_guild_emoji_at(store, 2001, i);
    if (e->user_id == 1001) own_em = e->id;
    else if (e->user_id == 1002) ada_em = e->id;
  }
  CT_CHECK(own_em != CCORD_ID_NONE && ada_em != CCORD_ID_NONE);

  // Create + rename + delete across emoji, sticker, soundboard as Admin.
  CT_CHECK(ccord_admin_create_emoji(&req, 2001, "sess_em", "data:image/png;base64,iVBORw==", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  CT_CHECK(ccord_admin_create_sticker(&req, 2001, "Sess Stick", "", "ok", png, sizeof png, "image/png", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  CT_CHECK(ccord_admin_create_soundboard(&req, 2001, "Sess Sound", "data:audio/ogg;base64,T2dnUw==", 75,
                                         CCORD_ID_NONE, "🔔", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  const ccord_emoji *created_em = ccord_store_find_emoji_by_name(store, "sess_em");
  const ccord_sticker *created_st = ccord_store_find_sticker_by_name(store, "Sess Stick");
  CT_CHECK(created_em && created_st);

  // Now restrict self on guild 2001 to CREATE_EXPRESSIONS only (exact permissions, non-owner):
  // can edit own_em, CANNOT edit or delete ada_em!
  ccord_role roles[2] = {
    {.id = 2001, .guild_id = 2001, .name = "@everyone", .permissions = CCORD_PERM_VIEW_CHANNEL, .position = 0},
    {.id = 2901, .guild_id = 2001, .name = "Creator", .permissions = CCORD_PERM_CREATE_EXPRESSIONS, .position = 1},
  };
  ccord_id self_roles[1] = {2901};
  CT_CHECK(ccord_store_set_guild_roles(store, 2001, roles, 2, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_member_roles(store, 2001, 1001, self_roles, 1, &err) == CCORD_OK);

  size_t adm0 = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_admin_edit_emoji(&req, 2001, ada_em, "hijack_ada", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_delete_emoji(&req, 2001, ada_em, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0);   // blocked locally before hitting transport!

  // Own expression edit IS allowed by session precheck.
  CT_CHECK(ccord_admin_edit_emoji(&req, 2001, own_em, "my_own_ok", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(strcmp(ccord_store_find_emoji(store, own_em)->name, "my_own_ok") == 0);

  // Remove CREATE_EXPRESSIONS too -> list/create/edit all blocked locally!
  roles[1].permissions = 0;
  CT_CHECK(ccord_store_set_guild_roles(store, 2001, roles, 2, &err) == CCORD_OK);
  size_t adm1 = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_admin_list_emojis(&req, 2001, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_create_emoji(&req, 2001, "nope_em", "data:image/png;base64,iVBORw==", nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == adm1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// ---- Expression data: REST answers reach the session, failures fail the blob, and the picker is retryable ----

typedef struct fake_expr_transport {
  ccord_transport base;
  bool queued;
  bool fail_next;
  ccord_expr_kind kind;
  char arg[64];
  char body[8192];
} fake_expr_transport;

static ccord_status fake_expr_connect(ccord_transport *t, ccord_error *err)
{
  (void)t;
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool fake_expr_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_expr_transport *f = (fake_expr_transport *)t;
  if (!f->queued) return false;
  memset(out, 0, sizeof *out);
  out->type = CCORD_NET_EXPRESSION_DATA;
  out->expr_kind = f->kind;
  out->expr_arg = f->arg;
  out->expr_body = f->fail_next ? nullptr : f->body;
  out->expr_body_len = f->fail_next ? 0 : strlen(f->body);
  out->status = f->fail_next ? CCORD_ERR_IO : CCORD_OK;
  f->queued = false;
  return true;
}

static void fake_expr_destroy(ccord_transport *t)
{
  free(t);
}

static ccord_status fake_expr_request(ccord_transport *t, ccord_expr_kind kind, const char *arg, size_t arg_len,
                                      ccord_error *err)
{
  fake_expr_transport *f = (fake_expr_transport *)t;
  if (kind >= CCORD_EXPR_KIND_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "fake: bad expression kind");
  size_t n = arg ? arg_len : 0;
  if (n >= sizeof f->arg) n = sizeof f->arg - 1;
  if (n) memcpy(f->arg, arg, n);
  f->arg[n] = '\0';
  f->kind = kind;
  f->queued = true;
  return CCORD_OK;
}

static const ccord_transport_vtable fake_expr_vt = {
  .name = "fake-expr",
  .connect = fake_expr_connect,
  .poll = fake_expr_poll,
  .destroy = fake_expr_destroy,
  .request_expression_data = fake_expr_request,
};

static void test_session_expression_data_failure(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_expr_transport *ft = calloc(1, sizeof *ft);
  CT_CHECK(bus && store && ft);
  if (!bus || !store || !ft) return;
  ft->base.vt = &fake_expr_vt;
  ft->fail_next = true;
  ccord_session *s = ccord_session_create(bus, store, &ft->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  // A queued fetch is PENDING; the transport failing must turn it FAILED (the old silent drop left it PENDING).
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_TRENDING, "", false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_PENDING);
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_FAILED);
  CT_CHECK(ccord_store_expr_blob(store, CCORD_EXPR_GIF_TRENDING, "", nullptr) == nullptr);

  // FAILED is retryable: the next successful answer reaches the store and parses.
  ft->fail_next = false;
  (void)snprintf(ft->body, sizeof ft->body,
                 "[{\"url\":\"https://tenor.com/view/retry-gif-1\","
                 "\"gif_src\":\"https://media.discordapp.net/attachments/1/retry.gif\","
                 "\"width\":111,\"height\":222}]");
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_TRENDING, "", false, &err) == CCORD_OK);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_PENDING);
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_TRENDING, "") == CCORD_EXPR_BLOB_READY);
  size_t blob_len = 0;
  const char *blob = ccord_store_expr_blob(store, CCORD_EXPR_GIF_TRENDING, "", &blob_len);
  CT_CHECK(blob && blob_len == strlen(ft->body) && strcmp(blob, ft->body) == 0);
  CT_CHECK(ccord_store_gif_trending_count(store) == 1);
  const ccord_gif_item *g = ccord_store_gif_trending_at(store, 0);
  CT_CHECK(g && strcmp(g->url, "https://tenor.com/view/retry-gif-1") == 0);
  CT_CHECK(g && strcmp(g->src, "https://media.discordapp.net/attachments/1/retry.gif") == 0);
  CT_CHECK(g && g->width == 111 && g->height == 222);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Minimal protobuf/base64 writer so the test can hand the session a FrecencyUserSettings answer with
// maximum-length URLs (the picker's page URL/src buffers are 512 bytes).
static size_t test_pb_varint(uint8_t *p, uint64_t v)
{
  size_t n = 0;
  do {
    p[n] = (uint8_t)((v & 0x7fu) | (v > 0x7fu ? 0x80u : 0u));
    v >>= 7;
    n++;
  } while (v != 0);
  return n;
}

static size_t test_pb_bytes(uint8_t *p, unsigned field, const void *data, size_t len)
{
  size_t n = test_pb_varint(p, ((uint64_t)field << 3u) | 2u);
  n += test_pb_varint(p + n, len);
  memcpy(p + n, data, len);
  return n + len;
}

static size_t test_pb_uint(uint8_t *p, unsigned field, uint64_t v)
{
  size_t n = test_pb_varint(p, (uint64_t)field << 3u);
  return n + test_pb_varint(p + n, v);
}

static size_t test_b64(const uint8_t *in, size_t len, char *out)
{
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0, i = 0;
  for (; i + 2 < len; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16u) | ((uint32_t)in[i + 1] << 8u) | in[i + 2];
    out[o++] = tbl[(v >> 18u) & 63u];
    out[o++] = tbl[(v >> 12u) & 63u];
    out[o++] = tbl[(v >> 6u) & 63u];
    out[o++] = tbl[v & 63u];
  }
  if (i < len) {
    uint32_t v = (uint32_t)in[i] << 16u;
    if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8u;
    out[o++] = tbl[(v >> 18u) & 63u];
    out[o++] = tbl[(v >> 12u) & 63u];
    out[o++] = i + 1 < len ? tbl[(v >> 6u) & 63u] : '=';
    out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

static void test_session_expression_settings_512_url(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_expr_transport *ft = calloc(1, sizeof *ft);
  CT_CHECK(bus && store && ft);
  if (!bus || !store || !ft) return;
  ft->base.vt = &fake_expr_vt;
  ccord_session *s = ccord_session_create(bus, store, &ft->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  char url[CCORD_URL_MAX_BYTES + 1], src[CCORD_URL_MAX_BYTES + 1];
  size_t plen = (size_t)snprintf(url, sizeof url, "https://tenor.com/view/");
  memset(url + plen, 'u', CCORD_URL_MAX_BYTES - plen);
  url[CCORD_URL_MAX_BYTES] = '\0';
  plen = (size_t)snprintf(src, sizeof src, "https://media.discordapp.net/attachments/1/");
  memset(src + plen, 's', CCORD_URL_MAX_BYTES - plen);
  src[CCORD_URL_MAX_BYTES] = '\0';

  uint8_t vg[2048], entry[4096], gifs[8192], raw[8192];
  char b64[12000];
  size_t vn = 0;
  vn += test_pb_bytes(vg + vn, 2, src, CCORD_URL_MAX_BYTES);
  vn += test_pb_uint(vg + vn, 3, 720);
  vn += test_pb_uint(vg + vn, 4, 480);
  vn += test_pb_uint(vg + vn, 5, 1);
  size_t en = 0;
  en += test_pb_bytes(entry + en, 1, url, CCORD_URL_MAX_BYTES);
  en += test_pb_bytes(entry + en, 2, vg, vn);
  size_t gn = 0;
  gn += test_pb_bytes(gifs + gn, 1, entry, en);
  size_t rn = 0;
  rn += test_pb_bytes(raw + rn, 2, gifs, gn);
  char *body = ft->body;
  int hn = snprintf(body, sizeof ft->body, "{\"settings\":\"");
  size_t bn = test_b64(raw, rn, b64);
  CT_CHECK(hn > 0 && (size_t)hn + bn + 3 <= sizeof ft->body);
  memcpy(body + (size_t)hn, b64, bn);
  memcpy(body + (size_t)hn + bn, "\"}", 3);

  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_SETTINGS_PROTO, "", false, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) == 1);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, "") == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_favorite_gif_count(store) == 1);
  const ccord_gif_item *fav = ccord_store_favorite_gif_at(store, 0);
  CT_CHECK(fav && strlen(fav->url) == CCORD_URL_MAX_BYTES && strcmp(fav->url, url) == 0);
  CT_CHECK(fav && strlen(fav->src) == CCORD_URL_MAX_BYTES && strcmp(fav->src, src) == 0);
  CT_CHECK(fav && fav->width == 720 && fav->height == 480);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

static void test_session_expression_catalog_and_favorites(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_transport *tr = ccord_mock_transport_create(nullptr, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);
  if (!s) return;

  // settings-proto/2: FrecencyUserSettings favorite GIFs populate the store with page URL, media src, dimensions.
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, "") == CCORD_EXPR_BLOB_NONE);
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_SETTINGS_PROTO, "", false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_SETTINGS_PROTO, "") == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_favorite_gif_count(store) == 2);
  // Newest first: the seeded ship-it favorite (order 2) leads wumpus (order 1), as Discord draws them.
  const ccord_gif_item *fav0 = ccord_store_favorite_gif_at(store, 0);
  const ccord_gif_item *fav1 = ccord_store_favorite_gif_at(store, 1);
  CT_CHECK(fav0 && strcmp(fav0->url, "https://tenor.com/view/ccord-ship-it-gif-1002") == 0 && fav0->order == 2);
  CT_CHECK(fav0 && strcmp(fav0->src, "https://media.discordapp.net/attachments/2001/gif_ship_it.gif") == 0);
  CT_CHECK(fav0 && fav0->width == 320 && fav0->height == 220);
  CT_CHECK(fav1 && strcmp(fav1->url, "https://tenor.com/view/wumpus-wave-gif-1001") == 0 && fav1->order == 1);
  CT_CHECK(fav1 && strcmp(fav1->src, "https://media.discordapp.net/attachments/2001/gif_wumpus_wave.gif") == 0);
  CT_CHECK(fav1 && fav1->width == 320 && fav1->height == 240);

  // Applying the same proto again neither duplicates nor drops an entry.
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_SETTINGS_PROTO, "", true, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_favorite_gif_count(store) == 2);

  // /gifs/trending carries categories and a first trending page; both are ingested.
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_CATEGORIES, "", false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_CATEGORIES, "") == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_gif_category_count(store) == 8);
  const ccord_gif_category *cat = ccord_store_gif_category_at(store, 0);
  CT_CHECK(cat && strcmp(cat->name, "Applause") == 0);
  CT_CHECK(ccord_store_gif_trending_count(store) == 6);
  const ccord_gif_item *trend = ccord_store_gif_trending_at(store, 0);
  CT_CHECK(trend && strcmp(trend->url, "https://tenor.com/view/wumpus-wave-gif-1001") == 0);

  // /gifs/trending-gifs (bare array) replaces the list; /gifs/search (bare array) fills its own list.
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_TRENDING, "", false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_gif_trending_count(store) == 6);
  CT_CHECK(ccord_session_request_expression_data(s, CCORD_EXPR_GIF_SEARCH, "ship it", false, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_expr_blob_state(store, CCORD_EXPR_GIF_SEARCH, "ship it") == CCORD_EXPR_BLOB_READY);
  CT_CHECK(ccord_store_gif_search_count(store) == 4);
  const ccord_gif_item *hit = ccord_store_gif_search_at(store, 0);
  CT_CHECK(hit && strstr(hit->url, "ship-it") != nullptr && hit->width == 320 && hit->height == 240);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// Store parsing of the GIF route shapes: object-wrapped or bare arrays, and an empty trending answer that must
// not wipe the fallback page already carried by /gifs/trending.
static void test_store_gif_payload_shapes(void)
{
  ccord_error err;
  ccord_store *store = ccord_store_create(nullptr, &err);
  CT_CHECK(store != nullptr);
  if (!store) return;

  const char *wrapped =
    "{\"results\":[{\"url\":\"https://tenor.com/view/wrapped-gif-9\","
    "\"src\":\"https://media.discordapp.net/wrapped.gif\",\"width\":64,\"height\":48}]}";
  CT_CHECK(ccord_store_set_expr_blob(store, CCORD_EXPR_GIF_TRENDING, "", wrapped, strlen(wrapped), true, &err) ==
           CCORD_OK);
  CT_CHECK(ccord_store_gif_trending_count(store) == 1);
  CT_CHECK(strcmp(ccord_store_gif_trending_at(store, 0)->url, "https://tenor.com/view/wrapped-gif-9") == 0);
  CT_CHECK(ccord_store_gif_trending_at(store, 0)->width == 64 && ccord_store_gif_trending_at(store, 0)->height == 48);

  // An empty trending-gifs answer keeps the rows already shown.
  CT_CHECK(ccord_store_set_expr_blob(store, CCORD_EXPR_GIF_TRENDING, "", "[]", 2, true, &err) == CCORD_OK);
  CT_CHECK(ccord_store_gif_trending_count(store) == 1);

  // A bare-array /gifs/trending payload is a valid fallback on a store with no trending-gifs rows yet.
  ccord_store *fallback_store = ccord_store_create(nullptr, &err);
  CT_CHECK(fallback_store != nullptr);
  const char *fallback =
    "[{\"url\":\"https://tenor.com/view/fallback-gif-1\",\"gif_src\":\"https://media.discordapp.net/fallback.gif\","
    "\"width\":32,\"height\":24}]";
  CT_CHECK(ccord_store_set_expr_blob(fallback_store, CCORD_EXPR_GIF_CATEGORIES, "", fallback, strlen(fallback), true,
                                     &err) == CCORD_OK);
  CT_CHECK(ccord_store_gif_trending_count(fallback_store) == 1);
  CT_CHECK(strcmp(ccord_store_gif_trending_at(fallback_store, 0)->url, "https://tenor.com/view/fallback-gif-1") == 0);
  ccord_store_destroy(fallback_store);

  ccord_store_destroy(store);
}

static void test_session_m4c_prune_and_automod(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_transport *tr = ccord_mock_transport_create(nullptr, &err);
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s && ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  // 1. AutoMod CRUD via session + mock transport as Admin on guild 2001.
  ccord_admin_request req;
  CT_CHECK(!ccord_store_automod_loaded(store, 2001));
  CT_CHECK(ccord_admin_list_automod_rules(&req, 2001, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_automod_loaded(store, 2001));
  size_t base_rules = 0;
  const ccord_automod_rule *rules = ccord_store_automod_rules(store, 2001, &base_rules);
  CT_CHECK(rules && base_rules >= 2);

  const char *kw[2] = {"phish.example", "scamlink"};
  ccord_automod_rule draft = {
    .guild_id = 2001,
    .name = "Session Anti-Phish",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD,
    .enabled = true,
    .keywords = kw,
    .keyword_count = 2,
    .action_block = true,
    .custom_message = "Blocked by AutoMod",
    .action_timeout = true,
    .timeout_seconds = 120,
  };
  CT_CHECK(ccord_admin_create_automod_rule(&req, 2001, &draft, "m4c test", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  size_t cnt = 0;
  rules = ccord_store_automod_rules(store, 2001, &cnt);
  CT_CHECK(rules && cnt == base_rules + 1);

  ccord_id created_id = CCORD_ID_NONE;
  for (size_t i = 0; i < cnt; i++) {
    if (strcmp(rules[i].name, "Session Anti-Phish") == 0) created_id = rules[i].id;
  }
  CT_CHECK(created_id != CCORD_ID_NONE);

  draft.id = created_id;
  draft.name = "Session Anti-Phish v2";
  draft.enabled = false;
  CT_CHECK(ccord_admin_edit_automod_rule(&req, 2001, created_id, &draft, "disable", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  rules = ccord_store_automod_rules(store, 2001, &cnt);
  const ccord_automod_rule *edited = nullptr;
  for (size_t i = 0; i < cnt; i++) {
    if (rules[i].id == created_id) edited = &rules[i];
  }
  CT_CHECK(edited && !edited->enabled && strcmp(edited->name, "Session Anti-Phish v2") == 0);

  CT_CHECK(ccord_admin_delete_automod_rule(&req, 2001, created_id, "cleanup", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  rules = ccord_store_automod_rules(store, 2001, &cnt);
  bool still_there = false;
  for (size_t i = 0; i < cnt; i++) {
    if (rules[i].id == created_id) still_there = true;
  }
  CT_CHECK(!still_there && cnt == base_rules);

  // 2. Prune preview + execute via session.
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, nullptr, 0, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  ccord_prune_info pi = {0};
  CT_CHECK(ccord_store_prune_info(store, 2001, &pi) && pi.has_result && !pi.executed && pi.days == 7 && pi.pruned >= 1);
  int32_t preview_cnt = pi.pruned;

  CT_CHECK(ccord_admin_execute_prune(&req, 2001, 7, true, nullptr, 0, "inactive sweep", &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);
  CT_CHECK(ccord_store_prune_info(store, 2001, &pi) && pi.has_result && pi.executed && pi.pruned == preview_cnt);

  // 3. Permission & role-hierarchy prechecks (blocked locally before hitting transport).
  ccord_role roles_cfg[4] = {
    {.id = 2001, .guild_id = 2001, .name = "@everyone", .permissions = CCORD_PERM_VIEW_CHANNEL, .position = 0},
    {.id = 2910, .guild_id = 2001, .name = "Helper", .permissions = 0, .position = 1},
    {.id = 2911, .guild_id = 2001, .name = "ModKick", .permissions = CCORD_PERM_KICK_MEMBERS, .position = 5},
    {.id = 2912, .guild_id = 2001, .name = "Senior", .permissions = CCORD_PERM_MANAGE_GUILD, .position = 10},
  };
  ccord_id self_roles[1] = {2911}; // KICK_MEMBERS at position 5, NO MANAGE_GUILD
  CT_CHECK(ccord_store_set_guild_roles(store, 2001, roles_cfg, 4, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_member_roles(store, 2001, 1001, self_roles, 1, &err) == CCORD_OK);

  size_t adm0 = ccord_mock_admin_count(tr);
  // AutoMod requires MANAGE_GUILD -> all blocked locally.
  CT_CHECK(ccord_admin_list_automod_rules(&req, 2001, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_create_automod_rule(&req, 2001, &draft, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  rules = ccord_store_automod_rules(store, 2001, &cnt);
  CT_CHECK(rules && cnt >= 1);
  CT_CHECK(ccord_admin_edit_automod_rule(&req, 2001, rules[0].id, &rules[0], nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_delete_automod_rule(&req, 2001, rules[0].id, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);

  // Prune include_roles hierarchy: peer role (2911, pos 5) and higher role (2912, pos 10) blocked!
  const ccord_id peer_role[1] = {2911};
  const ccord_id higher_role[1] = {2912};
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, peer_role, 1, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_execute_prune(&req, 2001, 7, true, higher_role, 1, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == adm0);

  // Lower role (2910, pos 1 < 5) IS allowed with KICK_MEMBERS.
  const ccord_id lower_role[1] = {2910};
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, lower_role, 1, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_OK);
  pump_all(s);

  // Without KICK_MEMBERS: prune preview & execute both blocked locally!
  roles_cfg[2].permissions = 0;
  CT_CHECK(ccord_store_set_guild_roles(store, 2001, roles_cfg, 4, &err) == CCORD_OK);
  size_t adm1 = ccord_mock_admin_count(tr);
  CT_CHECK(ccord_admin_preview_prune(&req, 2001, 7, nullptr, 0, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_admin_execute_prune(&req, 2001, 7, true, nullptr, 0, nullptr, &err) == CCORD_OK &&
           ccord_session_admin(s, &req, &err) == CCORD_ERR_PERMISSION);
  CT_CHECK(ccord_mock_admin_count(tr) == adm1);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// M0-5: invite previews are reused for 60 s, identical asks within 15 s are deduplicated, and revoke/accept
// invalidate the cached copy. The proxy for "was the transport called" is the mock's admin-request counter.
static void test_session_m0_invite_preview_cache(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 53, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  pump_all(s);

  ccord_admin_request req;
  bool loading = true;
  size_t calls = ccord_mock_admin_count(tr);

  // The first preview reaches the transport and leaves the view loading.
  g_now = 100000;
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/c23-study", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 1);
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && loading);

  // An identical ask while the first is still in flight is dropped (15 s guard).
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 14999, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 1);
  pump_all(s);
  const ccord_invite_preview *prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9003 && strcmp(prev->code, "c23-study") == 0);

  // Clearing the view does not evict the cache: 59999 ms after the fetch is still a hit.
  ccord_session_clear_invite_preview(s);
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && !loading);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 59999, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 1);
  prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9003 && strcmp(prev->code, "c23-study") == 0);

  // At exactly 60000 ms the entry expires and is fetched again.
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 60000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 2);
  pump_all(s);

  // A different code is a different key, and the first code still hits its own entry afterwards.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/vulkan-lab", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 60001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 3);
  pump_all(s);
  prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9004);
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/c23-study", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 60002, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 3);
  prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 9003);

  // The in-flight guard ends exactly at 15000 ms.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/synthwave", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 70000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 4);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 84999, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 4);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 85000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 5);
  pump_all(s);

  // A failed preview releases the guard so the next ask retries at once.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/no-such-code", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 90000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 6);
  pump_all(s);
  CT_CHECK(ccord_session_invite_preview(s, &loading) == nullptr && !loading);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 90001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 7);   // not blocked by the failed attempt
  pump_all(s);

  // Revoking the previewed code invalidates its entry.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/cCordHQ", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 100000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 8);
  pump_all(s);
  prev = ccord_session_invite_preview(s, &loading);
  CT_CHECK(!loading && prev && prev->guild_id == 2001);
  CT_CHECK(ccord_admin_revoke_invite(&req, 2001, "cCordHQ", nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 100001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 9);
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/cCordHQ", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 100002, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 10);   // cache was invalidated
  pump_all(s);

  // Accepting a previewed code invalidates it too.
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/pixel-cinema", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 110000, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 11);
  pump_all(s);
  CT_CHECK(ccord_admin_accept_invite(&req, "pixel-cinema", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 110001, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 12);
  CT_CHECK(ccord_admin_preview_invite(&req, "https://discord.gg/pixel-cinema", &err) == CCORD_OK);
  CT_CHECK(ccord_session_admin_ex(s, &req, g_now + 110002, &err) == CCORD_OK);
  CT_CHECK(ccord_mock_admin_count(tr) == calls + 13);   // cache was invalidated

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// M0-3: pending local echo, nonce-matched confirmation, failure marking without draft restore, retry with the
// SAME nonce, and per-nonce correlation with two sends in flight. Counts never move for the echo itself.
static void test_session_m0_nonce_echo_and_retry(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_mock_config cfg = {.seed = 7, .now_ms = fake_now};
  ccord_transport *tr = ccord_mock_transport_create(&cfg, &err);
  CT_CHECK(bus && store && tr);
  if (!bus || !store || !tr) return;
  ccord_session *s = ccord_session_create(bus, store, tr, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) > 0);
  ccord_id ch = ccord_store_selected_channel(store);
  CT_CHECK(ch != CCORD_ID_NONE && ccord_store_history_loaded(store, ch));
  size_t before = ccord_store_message_count(store, ch);
  const ccord_channel *c0 = ccord_store_channel(store, ch);
  CT_CHECK(c0 && c0->unread_count == 0 && c0->mention_count == 0);
  ccord_id newest_before_echo = c0 ? c0->last_message_id : CCORD_ID_NONE;

  // 1. The pending echo appears at once: local id, pending state, nonce, own author; counts stay put.
  CT_CHECK(ccord_session_send_message(s, ch, "pending text", 12, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 1);
  const ccord_message *pe = ccord_store_message_at(store, ch, before);
  CT_CHECK(pe && pe->send_state == CCORD_MSG_SEND_PENDING);
  CT_CHECK(pe && (pe->id & (1ULL << 63)) != 0 && pe->author_id == ccord_store_self_id(store));
  CT_CHECK(pe && strcmp(pe->content, "pending text") == 0 && pe->nonce[0] != '\0');
  char nonce1[32] = "";
  ccord_id echo1 = pe ? pe->id : CCORD_ID_NONE;
  if (pe) memcpy(nonce1, pe->nonce, sizeof nonce1);
  const ccord_channel *c1 = ccord_store_channel(store, ch);
  CT_CHECK(c1 && c1->unread_count == 0 && c1->mention_count == 0);
  // The echo sorts after confirmed history (it is the last row) but is never the channel's newest real message.
  CT_CHECK(c1 && c1->last_message_id == newest_before_echo && c1->last_message_id != echo1);
  // Only FAILED echoes are retryable: a pending one is NOT_FOUND.
  CT_CHECK(ccord_session_retry_message(s, ch, echo1, &err) == CCORD_ERR_NOT_FOUND);

  // 2. The nonce-matched MESSAGE_CREATE replaces the echo (no duplicate) and keeps the nonce.
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 1);
  CT_CHECK(ccord_store_find_message(store, ch, echo1) == nullptr);
  const ccord_message *cm = ccord_store_message_at(store, ch, before);
  CT_CHECK(cm && cm->send_state == CCORD_MSG_SEND_CONFIRMED && (cm->id & (1ULL << 63)) == 0);
  CT_CHECK(cm && strcmp(cm->content, "pending text") == 0 && strcmp(cm->nonce, nonce1) == 0);
  ccord_id confirmed_id = cm ? cm->id : CCORD_ID_NONE;
  CT_CHECK(ccord_session_retry_message(s, ch, confirmed_id, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_retry_message(s, ch, (1ULL << 63) | 999999u, &err) == CCORD_ERR_NOT_FOUND);
  CT_CHECK(ccord_session_retry_message(s, 424242, echo1, &err) == CCORD_ERR_NOT_FOUND);

  // 3. Someone else's message counts as unread while reading elsewhere; a refused send fails its echo
  //    without touching those counts and without queueing a draft restore.
  ccord_id guild = ccord_store_selected_guild(store);
  ccord_id other = CCORD_ID_NONE;
  for (size_t i = 0; i < ccord_store_channel_count(store, guild); i++) {
    const ccord_channel *c = ccord_store_channel_at(store, guild, i);
    if (c->kind == CCORD_CHANNEL_TEXT && c->id != ch) { other = c->id; break; }
  }
  CT_CHECK(other != CCORD_ID_NONE);
  ccord_session_set_reading(s, other);
  ccord_id foreign_id = CCORD_ID_NONE;
  CT_CHECK(ccord_mock_post_rich_message(tr, ch, 1002, "foreign", nullptr, &foreign_id, &err) == CCORD_OK);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  const ccord_channel *c2 = ccord_store_channel(store, ch);
  CT_CHECK(c2 && c2->unread_count == 1 && c2->mention_count == 0);

  ccord_mock_set_refuse_writes(tr, true);
  CT_CHECK(ccord_session_send_message(s, ch, "will fail", 9, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 3);
  const ccord_message *fe = ccord_store_message_at(store, ch, before + 2);
  CT_CHECK(fe && fe->send_state == CCORD_MSG_SEND_PENDING && strcmp(fe->content, "will fail") == 0);
  char nonce2[32] = "";
  ccord_id echo2 = fe ? fe->id : CCORD_ID_NONE;
  if (fe) memcpy(nonce2, fe->nonce, sizeof nonce2);
  CT_CHECK(strcmp(nonce2, nonce1) != 0);   // a fresh nonce per send
  const ccord_channel *c3 = ccord_store_channel(store, ch);
  CT_CHECK(c3 && c3->unread_count == 1 && c3->mention_count == 0);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  const ccord_message *fm = ccord_store_find_message(store, ch, echo2);
  CT_CHECK(fm && fm->send_state == CCORD_MSG_SEND_FAILED && strcmp(fm->content, "will fail") == 0);
  CT_CHECK(fm && fm->id == echo2 && strcmp(fm->nonce, nonce2) == 0);
  const ccord_channel *c4 = ccord_store_channel(store, ch);
  CT_CHECK(c4 && c4->unread_count == 1 && c4->mention_count == 0);   // the foreign unread survives the failure
  CT_CHECK(c4 && c4->last_message_id == foreign_id);   // the failed echo is not the newest real message
  ccord_action_failure af;
  CT_CHECK(!ccord_session_pop_failure(s, &af));   // the failed bubble holds the draft; no composer restore

  // Mark As Read acknowledges the newest CONFIRMED message, not the failed echo: the foreign unread clears and the
  // echo row survives untouched, ready for retry.
  CT_CHECK(ccord_store_set_channel_counts(store, ch, 1, 1, &err) == CCORD_OK);   // a mention rides the foreign unread
  size_t acked = 0;
  CT_CHECK(ccord_session_mark_read(s, guild, ch, &acked, &err) == CCORD_OK && acked == 1);
  const ccord_channel *c4r = ccord_store_channel(store, ch);
  CT_CHECK(c4r && c4r->unread_count == 0 && c4r->mention_count == 0);
  CT_CHECK(c4r && c4r->last_read_id == foreign_id);
  CT_CHECK(c4r && c4r->last_message_id == foreign_id);
  fm = ccord_store_find_message(store, ch, echo2);
  CT_CHECK(fm && fm->send_state == CCORD_MSG_SEND_FAILED && strcmp(fm->content, "will fail") == 0);

  // 4. Retry resubmits the SAME nonce, re-pends the echo, and the confirmation replaces it.
  ccord_mock_set_refuse_writes(tr, false);
  CT_CHECK(ccord_session_retry_message(s, ch, echo2, &err) == CCORD_OK);
  const ccord_message *re = ccord_store_find_message(store, ch, echo2);
  CT_CHECK(re && re->send_state == CCORD_MSG_SEND_PENDING && strcmp(re->nonce, nonce2) == 0);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 3);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_find_message(store, ch, echo2) == nullptr);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 3);
  const ccord_message *rc = ccord_store_message_at(store, ch, before + 2);
  CT_CHECK(rc && rc->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(rc->content, "will fail") == 0);
  CT_CHECK(rc && strcmp(rc->nonce, nonce2) == 0);   // the original nonce went out again, end to end
  const ccord_channel *c5 = ccord_store_channel(store, ch);
  CT_CHECK(c5 && c5->unread_count == 0);   // own confirmed message marks the channel read (existing semantics)

  // 5. Two sends in flight: each refusal fails exactly its own echo, and retrying one leaves the other failed.
  ccord_mock_set_refuse_writes(tr, true);
  CT_CHECK(ccord_session_send_message(s, ch, "first", 5, &err) == CCORD_OK);
  CT_CHECK(ccord_session_send_message(s, ch, "second", 6, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 5);
  const ccord_message *e1 = ccord_store_message_at(store, ch, before + 3);
  const ccord_message *e2 = ccord_store_message_at(store, ch, before + 4);
  CT_CHECK(e1 && e2 && strcmp(e1->content, "first") == 0 && strcmp(e2->content, "second") == 0);
  char n_second[32] = "";
  ccord_id id_first = e1 ? e1->id : CCORD_ID_NONE, id_second = e2 ? e2->id : CCORD_ID_NONE;
  if (e2) memcpy(n_second, e2->nonce, sizeof n_second);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  const ccord_message *f1 = ccord_store_find_message(store, ch, id_first);
  const ccord_message *f2 = ccord_store_find_message(store, ch, id_second);
  CT_CHECK(f1 && f1->send_state == CCORD_MSG_SEND_FAILED && strcmp(f1->content, "first") == 0);
  CT_CHECK(f2 && f2->send_state == CCORD_MSG_SEND_FAILED && strcmp(f2->content, "second") == 0);
  CT_CHECK(!ccord_session_pop_failure(s, &af));
  ccord_mock_set_refuse_writes(tr, false);
  CT_CHECK(ccord_session_retry_message(s, ch, id_second, &err) == CCORD_OK);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_store_find_message(store, ch, id_second) == nullptr);
  f1 = ccord_store_find_message(store, ch, id_first);
  CT_CHECK(f1 && f1->send_state == CCORD_MSG_SEND_FAILED);
  CT_CHECK(ccord_store_message_count(store, ch) == before + 5);
  // The confirmed "second" takes its real-id place; the failed "first" echo still sorts last (local id).
  const ccord_message *lastm = ccord_store_message_at(store, ch, before + 3);
  CT_CHECK(lastm && strcmp(lastm->content, "second") == 0 && strcmp(lastm->nonce, n_second) == 0);
  CT_CHECK(lastm && lastm->send_state == CCORD_MSG_SEND_CONFIRMED);
  const ccord_message *tailm = ccord_store_message_at(store, ch, before + 4);
  CT_CHECK(tailm && tailm->id == id_first && tailm->send_state == CCORD_MSG_SEND_FAILED);

  // 6. Slowmode refusals carry the nonce too: in 3315 (5 s slowmode, self is a plain Member) the second send
  // inside the cooldown fails exactly its own echo, with no composer restore.
  CT_CHECK(ccord_session_send_message(s, 3315, "slow one", 8, &err) == CCORD_OK);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  CT_CHECK(ccord_session_send_message(s, 3315, "too fast", 8, &err) == CCORD_OK);
  const ccord_message *se = ccord_store_message_at(store, 3315, ccord_store_message_count(store, 3315) - 1u);
  CT_CHECK(se && se->send_state == CCORD_MSG_SEND_PENDING);
  char slow_nonce[32] = "";
  ccord_id slow_echo = se ? se->id : CCORD_ID_NONE;
  if (se) memcpy(slow_nonce, se->nonce, sizeof slow_nonce);
  g_now += 10;
  CT_CHECK(pump_all(s) > 0);
  const ccord_message *sf = ccord_store_find_message(store, 3315, slow_echo);
  CT_CHECK(sf && sf->send_state == CCORD_MSG_SEND_FAILED && strcmp(sf->nonce, slow_nonce) == 0);
  CT_CHECK(!ccord_session_pop_failure(s, &af));

  // 7. Store-level bookkeeping: an unconfirmed bit-63 echo may sit in history, but it never becomes the channel's
  //    newest message, a Mark As Read target, an unread counted by apply_read, or an unread forgotten by a delete.
  const ccord_id echo_chan = 880001, echo_only_chan = 880002, echo_id = (1ULL << 63) | 7u;
  const ccord_id echo_only_id = (1ULL << 63) | 8u, echo_only_last = (1ULL << 63) | 500u;
  ccord_channel book = {.id = echo_chan, .guild_id = guild, .kind = CCORD_CHANNEL_TEXT, .name = "book", .topic = "",
                        .position = 90};
  ccord_channel book2 = {.id = echo_only_chan, .guild_id = guild, .kind = CCORD_CHANNEL_TEXT, .name = "book2", .topic = "",
                         .position = 91, .last_message_id = echo_only_last};
  CT_CHECK(ccord_store_upsert_channel(store, &book, &err) == CCORD_OK);
  CT_CHECK(ccord_store_upsert_channel(store, &book2, &err) == CCORD_OK);
  ccord_message book_hist[2] = {
    {.id = 8001, .channel_id = echo_chan, .author_id = 1002, .timestamp_ms = 1, .content = "old"},
    {.id = 8002, .channel_id = echo_chan, .author_id = 1002, .timestamp_ms = 2, .content = "new"},
  };
  CT_CHECK(ccord_store_set_history(store, echo_chan, book_hist, 2, &err) == CCORD_OK);
  ccord_message book_echo = {.id = echo_id, .channel_id = echo_chan, .author_id = ccord_store_self_id(store),
                             .timestamp_ms = 3, .content = "pending", .send_state = CCORD_MSG_SEND_PENDING};
  memcpy(book_echo.nonce, "book-nonce", 11);
  CT_CHECK(ccord_store_append_message(store, &book_echo, &err) == CCORD_OK);
  CT_CHECK(ccord_store_message_count(store, echo_chan) == 3);
  const ccord_message *book_tail = ccord_store_message_at(store, echo_chan, 2);
  CT_CHECK(book_tail && book_tail->id == echo_id);   // it renders after confirmed history ...
  const ccord_channel *bc = ccord_store_channel(store, echo_chan);
  CT_CHECK(bc && bc->last_message_id == 8002);       // ... but is never the newest real message
  // A history re-fetch merges around the echo and must not pick the bit-63 local id as newest either.
  CT_CHECK(ccord_store_set_history(store, echo_chan, book_hist, 2, &err) == CCORD_OK);
  bc = ccord_store_channel(store, echo_chan);
  CT_CHECK(bc && bc->last_message_id == 8002);
  CT_CHECK(ccord_store_set_channel_counts(store, echo_chan, 5, 2, &err) == CCORD_OK);
  ccord_read_target bt[4];
  CT_CHECK(ccord_store_read_targets(store, guild, echo_chan, 0, bt, 4) == 1 && bt[0].message_id == 8002);
  CT_CHECK(ccord_store_apply_read(store, echo_chan, 8001, UINT32_MAX, &err) == CCORD_OK);
  bc = ccord_store_channel(store, echo_chan);
  CT_CHECK(bc && bc->unread_count == 1 && bc->mention_count == 1);   // only confirmed 8002 is unread
  CT_CHECK(ccord_store_set_channel_counts(store, echo_chan, 1, 1, &err) == CCORD_OK);
  CT_CHECK(ccord_store_delete_messages_bulk(store, echo_chan, &echo_id, 1, &err) == CCORD_OK);
  bc = ccord_store_channel(store, echo_chan);
  CT_CHECK(bc && bc->unread_count == 1 && bc->mention_count == 1);   // deleting the echo forgets no unread
  CT_CHECK(ccord_store_message_count(store, echo_chan) == 2);

  // A channel whose stored pointer is already a bit-63 echo falls back to the newest loaded confirmed message.
  ccord_message book2_hist[2] = {
    {.id = 8101, .channel_id = echo_only_chan, .author_id = 1002, .timestamp_ms = 4, .content = "older"},
    {.id = 8102, .channel_id = echo_only_chan, .author_id = 1002, .timestamp_ms = 5, .content = "newest"},
  };
  CT_CHECK(ccord_store_set_history(store, echo_only_chan, book2_hist, 2, &err) == CCORD_OK);
  ccord_message book2_echo = {.id = echo_only_id, .channel_id = echo_only_chan, .author_id = ccord_store_self_id(store),
                              .timestamp_ms = 6, .content = "pending2", .send_state = CCORD_MSG_SEND_PENDING};
  memcpy(book2_echo.nonce, "book-nonce2", 12);
  CT_CHECK(ccord_store_append_message(store, &book2_echo, &err) == CCORD_OK);
  CT_CHECK(ccord_store_set_channel_counts(store, echo_only_chan, 1, 0, &err) == CCORD_OK);
  const ccord_channel *b2 = ccord_store_channel(store, echo_only_chan);
  CT_CHECK(b2 && b2->last_message_id == echo_only_last);   // still the stale bit-63 pointer
  CT_CHECK(ccord_store_read_targets(store, guild, echo_only_chan, 0, bt, 4) == 1 && bt[0].message_id == 8102);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

// M0-3 edge paths: hook-less fallback, foreign nonce, synchronous refusal rollback, the legacy failure ring when
// no echo matches, and the local-echo guards on ack/edit/delete/reaction.
static void test_session_m0_nonce_fake_edges(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(bus && store);
  if (!bus || !store) return;
  ccord_user users[2] = {
    {.id = 1, .name = "You", .status_text = "", .presence = CCORD_PRESENCE_ONLINE},
    {.id = 2, .name = "Ann", .status_text = "", .presence = CCORD_PRESENCE_IDLE},
  };
  ccord_guild guilds[1] = {{.id = 10, .name = "HQ"}};
  ccord_channel chans[2] = {{.id = 22, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                             .name = "general", .topic = ""},
                            {.id = 23, .guild_id = 10, .parent_id = 0, .kind = CCORD_CHANNEL_TEXT,
                             .name = "elsewhere", .topic = ""}};
  ccord_snapshot snap = {.self_id = 1, .users = users, .user_count = 2, .guilds = guilds, .guild_count = 1,
                         .channels = chans, .channel_count = 2};
  CT_CHECK(ccord_store_set_snapshot(store, &snap, &err) == CCORD_OK);
  ccord_message hist[1] = {{.id = 100, .channel_id = 22, .author_id = 2, .timestamp_ms = 1, .content = "a"}};
  CT_CHECK(ccord_store_set_history(store, 22, hist, 1, &err) == CCORD_OK);

  // 1. Hook-less transport: the wrapper falls back to send_message; the session keeps the legacy no-echo path.
  fake_transport *f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) { free(f); return; }
  CT_CHECK(ccord_transport_send_message_with_nonce(&f->base, 22, "direct", 6, "n-direct", 8, &err) == CCORD_OK);
  CT_CHECK(f->send_count == 1 && f->nonce_send_count == 0);   // the null hook fell back to the legacy send
  CT_CHECK(strcmp(f->last_send_text, "direct") == 0);
  CT_CHECK(ccord_session_send_message(s, 22, "legacy", 6, &err) == CCORD_OK);
  CT_CHECK(f->send_count == 2 && f->nonce_send_count == 0);
  CT_CHECK(ccord_store_message_count(store, 22) == 1);   // no echo without the hook
  // A legacy SEND failure (no nonce) still restores the draft through the failure ring.
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_SEND, .channel_id = 22,
                                    .status = CCORD_ERR_PERMISSION, .text = "no", .draft = "legacy", .draft_len = 6};
  CT_CHECK(ccord_session_pump(s) == 1);
  ccord_action_failure af;
  CT_CHECK(ccord_session_pop_failure(s, &af));
  CT_CHECK(af.action == CCORD_ACTION_SEND && af.draft && af.draft_len == 6);
  ccord_action_failure_free(&af);
  ccord_session_destroy(s);

  // 2. Nonce hook: a foreign nonce appends beside the pending echo; the matching nonce then confirms it.
  f = calloc(1, sizeof *f);
  CT_CHECK(f != nullptr);
  if (!f) return;
  f->base.vt = &fake_nonce_vt;
  s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) { free(f); return; }
  CT_CHECK(ccord_session_send_message(s, 22, "mine", 4, &err) == CCORD_OK);
  CT_CHECK(f->nonce_send_count == 1 && f->send_count == 0);
  CT_CHECK(ccord_store_message_count(store, 22) == 2);
  const ccord_message *pe = ccord_store_message_at(store, 22, 1);
  CT_CHECK(pe && pe->send_state == CCORD_MSG_SEND_PENDING);
  char my_nonce[32] = "";
  ccord_id my_echo = pe ? pe->id : CCORD_ID_NONE;
  if (pe) memcpy(my_nonce, pe->nonce, sizeof my_nonce);
  CT_CHECK(strcmp(f->last_send_nonce, my_nonce) == 0);   // the transport received the echo's nonce
  ccord_message foreign = {.id = 300, .channel_id = 22, .author_id = 2, .timestamp_ms = 5, .content = "foreign"};
  memcpy(foreign.nonce, "not-mine", 9);
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .channel_id = 22,
                                    .messages = &foreign, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_message_count(store, 22) == 3);   // foreign nonce appended, echo untouched
  CT_CHECK(ccord_store_find_message(store, 22, my_echo) != nullptr);
  CT_CHECK(ccord_store_find_message(store, 22, 300) != nullptr);
  ccord_message confirm = {.id = 400, .channel_id = 22, .author_id = 1, .timestamp_ms = 6, .content = "mine"};
  memcpy(confirm.nonce, my_nonce, sizeof confirm.nonce);
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .channel_id = 22,
                                    .messages = &confirm, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_store_message_count(store, 22) == 3);
  CT_CHECK(ccord_store_find_message(store, 22, my_echo) == nullptr);
  const ccord_message *cm = ccord_store_find_message(store, 22, 400);
  CT_CHECK(cm && cm->send_state == CCORD_MSG_SEND_CONFIRMED && strcmp(cm->nonce, my_nonce) == 0);

  // 3. Synchronous refusal: the echo is rolled back and the error returned (the caller keeps the text). The
  // rollback must not eat an unrelated unread: while reading elsewhere, a foreign message made 22 unread first.
  ccord_session_set_reading(s, 23);
  ccord_message foreign2 = {.id = 500, .channel_id = 22, .author_id = 2, .timestamp_ms = 7, .content = "unread maker"};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_MESSAGE_CREATE, .channel_id = 22,
                                    .messages = &foreign2, .message_count = 1};
  CT_CHECK(ccord_session_pump(s) == 1);
  const ccord_channel *uc = ccord_store_channel(store, 22);
  CT_CHECK(uc && uc->unread_count == 1);
  f->nonce_send_fail = true;
  CT_CHECK(ccord_session_send_message(s, 22, "rolled back", 11, &err) == CCORD_ERR_IO);
  CT_CHECK(ccord_store_message_count(store, 22) == 4);   // no leftover echo
  uc = ccord_store_channel(store, 22);
  CT_CHECK(uc && uc->unread_count == 1);   // the rollback restored the counts the delete disturbed
  CT_CHECK(f->nonce_send_count == 2);
  f->nonce_send_fail = false;

  // 4. A refusal whose nonce matches no echo (history replaced meanwhile) keeps the legacy draft ring.
  CT_CHECK(ccord_session_send_message(s, 22, "orphan", 6, &err) == CCORD_OK);
  pe = ccord_store_message_at(store, 22, ccord_store_message_count(store, 22) - 1u);
  CT_CHECK(pe && pe->send_state == CCORD_MSG_SEND_PENDING);
  char orphan_nonce[32] = "";
  if (pe) memcpy(orphan_nonce, pe->nonce, sizeof orphan_nonce);
  ccord_id orphan_echo = pe ? pe->id : CCORD_ID_NONE;
  CT_CHECK(ccord_store_delete_message(store, 22, orphan_echo, &err) == CCORD_OK);   // e.g. history reloaded without it
  f->ev[f->n] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_SEND, .channel_id = 22,
                                  .status = CCORD_ERR_PERMISSION, .text = "no", .draft = "orphan", .draft_len = 6};
  memcpy(f->ev[f->n].action_nonce, orphan_nonce, sizeof orphan_nonce);
  f->n++;
  CT_CHECK(ccord_session_pump(s) == 1);
  CT_CHECK(ccord_session_pop_failure(s, &af));
  CT_CHECK(af.draft && af.draft_len == 6 && memcmp(af.draft, "orphan", 6) == 0);
  ccord_action_failure_free(&af);

  // 5. Local echoes are not server messages: ack is a no-op, edit/delete/reaction refuse.
  CT_CHECK(ccord_session_send_message(s, 22, "guarded", 7, &err) == CCORD_OK);
  pe = ccord_store_message_at(store, 22, ccord_store_message_count(store, 22) - 1u);
  CT_CHECK(pe && pe->send_state == CCORD_MSG_SEND_PENDING);
  ccord_id gid = pe ? pe->id : CCORD_ID_NONE;
  CT_CHECK(ccord_session_ack_message(s, 22, gid, &err) == CCORD_OK);   // no transport ack (hook is null anyway)
  CT_CHECK(ccord_session_edit_message(s, 22, gid, "x", 1, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_delete_message(s, 22, gid, &err) == CCORD_ERR_INVALID);
  CT_CHECK(ccord_session_toggle_reaction(s, 22, gid, "\xf0\x9f\x91\x8d", 4, &err) == CCORD_ERR_INVALID);

  // 6. A retry refused synchronously stays failed with its draft (the next attempt can try again).
  char gnonce[32] = "";
  memcpy(gnonce, pe->nonce, sizeof gnonce);
  f->ev[f->n] = (ccord_net_event){.type = CCORD_NET_ACTION_FAILED, .action = CCORD_ACTION_SEND, .channel_id = 22,
                                  .status = CCORD_ERR_PERMISSION, .text = "no", .draft = "guarded", .draft_len = 7};
  memcpy(f->ev[f->n].action_nonce, gnonce, sizeof gnonce);
  f->n++;
  CT_CHECK(ccord_session_pump(s) == 1);
  const ccord_message *gf = ccord_store_find_message(store, 22, gid);
  CT_CHECK(gf && gf->send_state == CCORD_MSG_SEND_FAILED);
  f->nonce_send_fail = true;
  CT_CHECK(ccord_session_retry_message(s, 22, gid, &err) == CCORD_ERR_IO);
  gf = ccord_store_find_message(store, 22, gid);
  CT_CHECK(gf && gf->send_state == CCORD_MSG_SEND_FAILED && strcmp(gf->content, "guarded") == 0);
  f->nonce_send_fail = false;

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

typedef struct fake_stream_transport {
  ccord_transport base;
  ccord_net_event ev[32];
  size_t n, i;
  uint32_t watch_count, stop_watch_count;
  ccord_id last_watch_guild, last_watch_channel, last_watch_user;
  bool has_rx_frame;
  uint8_t rx_frame[16];
  size_t rx_frame_len;
  bool rx_frame_key;
  int16_t rx_pcm[16];
  size_t rx_pcm_len;
} fake_stream_transport;

static bool fake_stream_poll(ccord_transport *t, ccord_net_event *out)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  if (f->i >= f->n) return false;
  *out = f->ev[f->i++];
  return true;
}

static ccord_status fake_stream_watch(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_id user_id,
                                      ccord_error *err)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  f->watch_count++;
  f->last_watch_guild = guild_id;
  f->last_watch_channel = channel_id;
  f->last_watch_user = user_id;
  ccord_error_clear(err);
  return CCORD_OK;
}

static ccord_status fake_stream_stop_watching(ccord_transport *t, ccord_id guild_id, ccord_id channel_id,
                                              ccord_id user_id, ccord_error *err)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  (void)guild_id;
  (void)channel_id;
  (void)user_id;
  f->stop_watch_count++;
  f->has_rx_frame = false;
  f->rx_pcm_len = 0;
  ccord_error_clear(err);
  return CCORD_OK;
}

static bool fake_stream_poll_frame(ccord_transport *t, uint8_t *buf, size_t buf_cap, size_t *out_len,
                                   uint32_t *out_ts90k, bool *out_keyframe)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  if (!f->has_rx_frame || buf_cap < f->rx_frame_len) return false;
  memcpy(buf, f->rx_frame, f->rx_frame_len);
  *out_len = f->rx_frame_len;
  if (out_ts90k) *out_ts90k = 9000u;
  if (out_keyframe) *out_keyframe = f->rx_frame_key;
  f->has_rx_frame = false;
  return true;
}

static size_t fake_stream_poll_pcm(ccord_transport *t, int16_t *out_pcm, size_t cap_samples)
{
  fake_stream_transport *f = (fake_stream_transport *)t;
  if (f->rx_pcm_len == 0 || cap_samples == 0) return 0;
  size_t take = f->rx_pcm_len < cap_samples ? f->rx_pcm_len : cap_samples;
  memcpy(out_pcm, f->rx_pcm, take * sizeof *out_pcm);
  if (take < f->rx_pcm_len) {
    memmove(f->rx_pcm, f->rx_pcm + take, (f->rx_pcm_len - take) * sizeof *f->rx_pcm);
  }
  f->rx_pcm_len -= take;
  return take;
}

static ccord_status fake_stream_noop_history(ccord_transport *t, ccord_id channel_id, uint32_t limit, ccord_error *err)
{
  (void)t;
  (void)channel_id;
  (void)limit;
  ccord_error_clear(err);
  return CCORD_OK;
}

static const ccord_transport_vtable fake_stream_vt = {
  .name = "fake-stream",
  .connect = fake_connect,
  .disconnect = fake_disconnect,
  .request_history = fake_stream_noop_history,
  .send_message = fake_send,
  .poll = fake_stream_poll,
  .destroy = fake_destroy,
  .watch_stream = fake_stream_watch,
  .stop_watching = fake_stream_stop_watching,
  .poll_stream_frame = fake_stream_poll_frame,
  .poll_stream_pcm = fake_stream_poll_pcm,
};

static void test_session_switch_concurrent_streams(void)
{
  ccord_error err;
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  fake_stream_transport *f = calloc(1, sizeof *f);
  CT_CHECK(bus && store && f);
  if (!bus || !store || !f) return;
  f->base.vt = &fake_stream_vt;
  ccord_session *s = ccord_session_create(bus, store, &f->base, &err);
  CT_CHECK(s != nullptr);
  if (!s) return;

  ccord_guild g = {.id = 10, .name = "G"};
  ccord_channel chs[2] = {
    {.id = 20, .guild_id = 10, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = ""},
    {.id = 21, .guild_id = 10, .kind = CCORD_CHANNEL_VOICE, .name = "StageVoice", .topic = ""},
  };
  ccord_user users[3] = {
    {.id = 1, .name = "me", .status_text = ""},
    {.id = 2, .name = "alice", .status_text = ""},
    {.id = 3, .name = "bob", .status_text = ""},
  };
  ccord_voice_state voices[3] = {
    {.guild_id = 10, .channel_id = 21, .user_id = 1},
    {.guild_id = 10, .channel_id = 21, .user_id = 2, .self_stream = true},
    {.guild_id = 10, .channel_id = 21, .user_id = 3, .self_stream = true},
  };
  ccord_snapshot snap = {
    .self_id = 1,
    .guilds = &g, .guild_count = 1,
    .channels = chs, .channel_count = 2,
    .users = users, .user_count = 3,
  };
  ccord_stream_signal create_a = {
    .kind = CCORD_STREAM_SIG_CREATE, .stream_key = "guild:10:21:2",
    .guild_id = 10, .channel_id = 21, .user_id = 2, .owner_id = 2, .viewer_count = 3,
  };
  ccord_stream_signal create_b = {
    .kind = CCORD_STREAM_SIG_CREATE, .stream_key = "guild:10:21:3",
    .guild_id = 10, .channel_id = 21, .user_id = 3, .owner_id = 3, .paused = true, .viewer_count = 5,
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STATE, .state = CCORD_CONN_CONNECTED};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_SNAPSHOT, .snapshot = &snap};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATE_UPDATE, .voice_state = &voices[0]};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATE_UPDATE, .voice_state = &voices[1]};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATE_UPDATE, .voice_state = &voices[2]};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &create_a};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &create_b};
  CT_CHECK(ccord_session_start(s, &err) == CCORD_OK);
  CT_CHECK(pump_all(s) == 7);
  CT_CHECK(ccord_store_stream_list_count(store) == 2);
  CT_CHECK(ccord_store_stream_find(store, 21, 2) != nullptr);
  CT_CHECK(ccord_store_stream_find(store, 21, 3) != nullptr);

  // Watch stream A (user 2); re-watching A is an idempotent no-op that preserves queued RX media.
  CT_CHECK(ccord_session_watch_stream(s, 10, 21, 2, &err) == CCORD_OK);
  CT_CHECK(f->watch_count == 1 && f->stop_watch_count == 0 && f->last_watch_user == 2);
  const ccord_watched_stream *ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 2 && !ws->paused);

  memcpy(f->rx_frame, "\x00\x00\x00\x01\x65\xaa", 6);
  f->rx_frame_len = 6;
  f->rx_frame_key = true;
  f->has_rx_frame = true;
  f->rx_pcm[0] = 1234;
  f->rx_pcm[1] = -1234;
  f->rx_pcm_len = 2;

  CT_CHECK(ccord_session_watch_stream(s, 10, 21, 2, &err) == CCORD_OK);
  CT_CHECK(f->watch_count == 1 && f->has_rx_frame && f->rx_pcm_len == 2);

  // Switch directly from stream A (user 2) to stream B (user 3):
  // - drains stale RX media from A
  // - transitions watched target + paused state cleanly to B without calling stop_watching
  // - preserves BOTH A and B in ccord_store even when Gateway echoes viewer-unsubscribe STREAM_DELETE for A
  CT_CHECK(ccord_session_watch_stream(s, 10, 21, 3, &err) == CCORD_OK);
  CT_CHECK(f->watch_count == 2 && f->stop_watch_count == 0 && f->last_watch_user == 3);
  CT_CHECK(!f->has_rx_frame && f->rx_pcm_len == 0);
  ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 3 && ws->paused);
  CT_CHECK(ccord_store_stream_list_count(store) == 2);

  ccord_stream_signal unsub_a = {
    .kind = CCORD_STREAM_SIG_DELETE, .stream_key = "guild:10:21:2",
    .guild_id = 10, .channel_id = 21, .user_id = 2, .owner_id = 2,
    .deleted = true, .reason = "stream_stopped",
  };
  ccord_stream_signal ssu_b = {
    .kind = CCORD_STREAM_SIG_SERVER_UPDATE, .stream_key = "guild:10:21:3",
    .guild_id = 10, .channel_id = 21, .user_id = 3, .owner_id = 3,
    .server_update = true, .token = "REDACTED_TEST_TOKEN", .endpoint = "wss://media.example",
  };
  ccord_stream_signal upd_a = {
    .kind = CCORD_STREAM_SIG_UPDATE, .stream_key = "guild:10:21:2",
    .guild_id = 10, .channel_id = 21, .user_id = 2, .owner_id = 2,
    .paused = true, .viewer_count = 7,
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &unsub_a};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &ssu_b};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &upd_a};
  CT_CHECK(pump_all(s) == 3);
  CT_CHECK(ccord_store_stream_list_count(store) == 2);
  CT_CHECK(ccord_store_stream_find(store, 21, 2) != nullptr);
  CT_CHECK(ccord_store_stream_find(store, 21, 3) != nullptr);
  CT_CHECK(ccord_store_stream_viewer_count(store, 21, 2) == 7);
  ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 3 && ws->paused);

  // Switch back from B to A: both remain in ccord_store and A's updated pause state is reflected.
  CT_CHECK(ccord_session_watch_stream(s, 10, 21, 2, &err) == CCORD_OK);
  CT_CHECK(f->watch_count == 3 && f->stop_watch_count == 0 && f->last_watch_user == 2);
  ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 2 && ws->paused);
  CT_CHECK(ccord_store_stream_list_count(store) == 2);

  // Broadcaster B actually stops streaming (self_stream=false + STREAM_DELETE): B is removed, A stays watched.
  ccord_voice_state b_stopped = {.guild_id = 10, .channel_id = 21, .user_id = 3, .self_stream = false};
  ccord_stream_signal del_b = {
    .kind = CCORD_STREAM_SIG_DELETE, .stream_key = "guild:10:21:3",
    .guild_id = 10, .channel_id = 21, .user_id = 3, .owner_id = 3,
    .deleted = true, .reason = "stream_stopped",
  };
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_VOICE_STATE_UPDATE, .voice_state = &b_stopped};
  f->ev[f->n++] = (ccord_net_event){.type = CCORD_NET_STREAM_SIGNAL, .stream_signal = &del_b};
  CT_CHECK(pump_all(s) == 2);
  CT_CHECK(ccord_store_stream_list_count(store) == 1);
  CT_CHECK(ccord_store_stream_find(store, 21, 2) != nullptr);
  CT_CHECK(ccord_store_stream_find(store, 21, 3) == nullptr);
  ws = ccord_session_watched_stream(s);
  CT_CHECK(ws && ws->active && ws->user_id == 2);

  // Explicit stop_watching tears down the active viewer state.
  CT_CHECK(ccord_session_stop_watching(s, &err) == CCORD_OK);
  CT_CHECK(f->stop_watch_count == 1);
  CT_CHECK(ccord_session_watched_stream(s) == nullptr);

  ccord_session_destroy(s);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);
}

int main(void)
{
  test_session_m0_invite_preview_cache();
  test_session_m4a_guild_settings_safety_vanity_and_ownership();
  test_session_m4b_expressions();
  test_session_expression_data_failure();
  test_session_expression_settings_512_url();
  test_session_expression_catalog_and_favorites();
  test_store_gif_payload_shapes();
  test_session_m4c_prune_and_automod();
  test_session_m0_nonce_echo_and_retry();
  test_session_m0_nonce_fake_edges();
  test_session_switch_concurrent_streams();
  return CT_RESULT("test_session_admin");
}
