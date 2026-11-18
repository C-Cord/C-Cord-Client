// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// --verify-slice status-sync: the account's status goes through settings-proto/1 so other open clients follow it, and a
// status changed by another client is followed here. Runs over the offline mock transport (which keeps the account's
// type-1 bytes, records every settings-proto/1 PATCH and every gateway op 3, and can inject a proto update as from
// another client); see verify_ui_parity.h.
//
// Checks: Do Not Disturb sends a settings-proto/1 PATCH that decodes to dnd with every other byte of the base proto
// unchanged, plus op 3; Invisible and a custom status (set, with a custom emoji, cleared) round-trip through the proto;
// an injected update with idle and custom text sets the store's own presence and custom status and re-sends op 3 with
// idle; a partial update without the status field changes neither store nor presence and sends nothing; a partial
// update with only a status keeps the custom status; a full update without custom status clears it; the next write
// builds on the bytes the other client wrote; and a SESSIONS_REPLACE status updates the displayed status without sending.
#define _GNU_SOURCE   // mkdtemp: -std=c2x hides it
#include "verify_ui_parity.h"

#include "app_rpc.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <ccord/core/alloc.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/mock_transport.h>
#include <ccord/rpc/activity.h>

typedef struct ss {
  const ccord_verify_ui_parity_ctx *c;
  ccord_app_rpc *rpc;   // the RPC pipeline of the self-activity checks (null otherwise)
  uint64_t now;         // its virtual clock, ms
  int checks;
  char fail[200];
} ss;

static bool ss_check(ss *s, bool ok, const char *name)
{
  if (!ok) {
    if (!s->fail[0]) snprintf(s->fail, sizeof s->fail, "%s", name);
    return false;
  }
  s->checks++;
  return true;
}

// Other top-level records the account holds (a length-delimited field 3, a varint field 99), around its status record.
static const uint8_t k_before[] = {0x1a, 0x03, 'a', 'b', 'c'};
static const uint8_t k_after[] = {0x98, 0x06, 0x42};
// field 11: status = StringValue "online" (0a 08 | 0a 06 online), show_current_game = BoolValue true (1a 02 | 08 01).
static const uint8_t k_status_rec[] = {0x5a, 0x0e, 0x0a, 0x08, 0x0a, 0x06, 'o', 'n', 'l', 'i', 'n', 'e', 0x1a, 0x02, 0x08, 0x01};
static const uint8_t k_game_flag[] = {0x1a, 0x02, 0x08, 0x01};

typedef struct buf {
  uint8_t d[256];
  size_t n;
} buf;

static void buf_add(buf *b, const uint8_t *p, size_t n)
{
  if (b->n + n <= sizeof b->d) {
    memcpy(b->d + b->n, p, n);
    b->n += n;
  }
}

static bool has_bytes(const uint8_t *hay, size_t hn, const uint8_t *needle, size_t nn)
{
  for (size_t i = 0; i + nn <= hn; i++) {
    if (memcmp(hay + i, needle, nn) == 0) return true;
  }
  return false;
}

static void frames(ss *s, int n) { s->c->frames(s->c->ud, n); }

static const ccord_user *self_user(ss *s)
{
  return ccord_store_user(s->c->store, ccord_store_self_id(s->c->store));
}

// The PATCH at index `i` is exactly the base's other records, in order, then a status record that decodes to `want`.
static bool patch_keeps_others(ss *s, size_t i, ccord_proto_status *out)
{
  size_t len = 0;
  const uint8_t *p = ccord_mock_status_proto_patch_at(s->c->transport, i, &len);
  buf others = {0};
  buf_add(&others, k_before, sizeof k_before);
  buf_add(&others, k_after, sizeof k_after);
  ccord_error err;
  return p && len > others.n && memcmp(p, others.d, others.n) == 0 &&
         has_bytes(p + others.n, len - others.n, k_game_flag, sizeof k_game_flag) &&
         ccord_user_status_decode(p, len, out, &err) == CCORD_OK && out->present;
}

static size_t patches(ss *s) { return ccord_mock_status_proto_patch_count(s->c->transport); }
static size_t op3s(ss *s) { return ccord_mock_presence_send_count(s->c->transport); }

// Builds type-1 bytes: the base's other records, then a status record for `st` (what another client would send).
static size_t other_client_proto(const ccord_proto_status *st, bool with_others, uint8_t *out, size_t cap)
{
  buf base = {0};
  if (with_others) {
    buf_add(&base, k_before, sizeof k_before);
    buf_add(&base, k_after, sizeof k_after);
  }
  uint8_t *made = nullptr;
  size_t n = 0;
  ccord_error err;
  if (!st) {
    if (base.n > cap) return 0;
    memcpy(out, base.d, base.n);
    return base.n;
  }
  if (ccord_user_status_patch(base.d, base.n, st, &made, &n, &err) != CCORD_OK || n > cap) {
    ccord_free(made);
    return 0;
  }
  memcpy(out, made, n);
  ccord_free(made);
  return n;
}

static int ss_run(ss *s)
{
  ccord_transport *t = s->c->transport;
  ccord_error err;
  frames(s, 3);

  // Server bytes: other records around an online status that also says show_current_game.
  buf base = {0};
  buf_add(&base, k_before, sizeof k_before);
  buf_add(&base, k_status_rec, sizeof k_status_rec);
  buf_add(&base, k_after, sizeof k_after);
  ccord_mock_set_status_proto_base(t, base.d, base.n);
  if (!ss_check(s, patches(s) == 0, "no patch yet")) return 1;
  size_t op3_start = op3s(s);

  // 1. Do Not Disturb: proto PATCH decoding to dnd, other bytes unchanged, plus op 3.
  if (!ss_check(s, ccord_session_set_presence(s->c->session, CCORD_PRESENCE_DND, nullptr, &err) == CCORD_OK, "set dnd")) return 1;
  frames(s, 3);
  ccord_proto_status ps;
  if (!ss_check(s, patches(s) == 1, "dnd: one proto patch")) return 1;
  if (!ss_check(s, patch_keeps_others(s, 0, &ps), "dnd: other bytes unchanged")) return 1;
  if (!ss_check(s, ps.has_status && strcmp(ps.status, "dnd") == 0 && !ps.has_custom, "dnd: patch decodes to dnd")) return 1;
  const char *body = ccord_mock_status_proto_patch_body(t, 0);
  if (!ss_check(s, body && strncmp(body, "{\"settings\":\"", 13) == 0, "dnd: body is {\"settings\":b64}")) return 1;
  const char *op3_text = nullptr;
  if (!ss_check(s, op3s(s) == op3_start + 1 && ccord_mock_last_presence_send(t, &op3_text) == CCORD_PRESENCE_DND, "dnd: op 3 sent")) return 1;
  if (!ss_check(s, self_user(s) && self_user(s)->presence == CCORD_PRESENCE_DND, "dnd: store shows dnd")) return 1;

  // 2. Invisible round-trips (the next write builds on what the last one wrote).
  if (!ss_check(s, ccord_session_set_presence(s->c->session, CCORD_PRESENCE_OFFLINE, nullptr, &err) == CCORD_OK, "set invisible")) return 1;
  frames(s, 3);
  if (!ss_check(s, patches(s) == 2 && patch_keeps_others(s, 1, &ps) && strcmp(ps.status, "invisible") == 0,
                "invisible: patch decodes to invisible, others unchanged")) return 1;
  if (!ss_check(s, ccord_mock_last_presence_send(t, &op3_text) == CCORD_PRESENCE_OFFLINE, "invisible: op 3 sent")) return 1;

  // 3. Custom status set: text and a unicode emoji; the status stays invisible.
  if (!ss_check(s, ccord_session_set_custom_status(s->c->session, "\xF0\x9F\x99\x82", CCORD_ID_NONE, "Hello", 0, &err) == CCORD_OK,
                "set custom status")) return 1;
  frames(s, 3);
  if (!ss_check(s, patches(s) == 3 && patch_keeps_others(s, 2, &ps), "custom: patch keeps others")) return 1;
  if (!ss_check(s, strcmp(ps.status, "invisible") == 0 && ps.has_custom && strcmp(ps.custom_text, "Hello") == 0 &&
                     strcmp(ps.emoji_name, "\xF0\x9F\x99\x82") == 0 && ps.emoji_id == 0,
                "custom: text and emoji in the proto, status unchanged")) return 1;

  // 3b. A custom emoji (name + id) and an expiry.
  if (!ss_check(s, ccord_session_set_custom_status(s->c->session, "party", 12345, "Party", 4102444800000, &err) == CCORD_OK,
                "set custom emoji status")) return 1;
  frames(s, 3);
  if (!ss_check(s, patches(s) == 4 && patch_keeps_others(s, 3, &ps) && ps.emoji_id == 12345 &&
                     strcmp(ps.emoji_name, "party") == 0 && ps.custom_expires_at_ms == 4102444800000ull,
                "custom emoji: id, name and expiry in the proto")) return 1;

  // 3c. Changing the presence keeps the custom status (store encoding "name:id" round-trips).
  if (!ss_check(s, ccord_session_set_presence(s->c->session, CCORD_PRESENCE_ONLINE, nullptr, &err) == CCORD_OK, "set online")) return 1;
  frames(s, 3);
  if (!ss_check(s, patches(s) == 5 && patch_keeps_others(s, 4, &ps) && strcmp(ps.status, "online") == 0 && ps.has_custom &&
                     strcmp(ps.custom_text, "Party") == 0 && ps.emoji_id == 12345,
                "presence change keeps the custom status in the proto")) return 1;

  // 4. Custom status cleared: field 2 gone, status stays.
  if (!ss_check(s, ccord_session_set_custom_status(s->c->session, "", CCORD_ID_NONE, "", 0, &err) == CCORD_OK, "clear custom status")) return 1;
  frames(s, 3);
  if (!ss_check(s, patches(s) == 6 && patch_keeps_others(s, 5, &ps) && !ps.has_custom && strcmp(ps.status, "online") == 0,
                "clear: custom status removed from the proto")) return 1;

  // 5. Another client sets idle with a custom status: store and op 3 follow.
  uint8_t other[256];
  ccord_proto_status idle = {0};
  idle.has_status = true;
  memcpy(idle.status, "idle", 5);
  idle.has_custom = true;
  memcpy(idle.custom_text, "Out to lunch", 13);
  memcpy(idle.emoji_name, "\xF0\x9F\x8D\x94", 5);
  size_t on = other_client_proto(&idle, true, other, sizeof other);
  if (!ss_check(s, on > 0, "build other-client proto")) return 1;
  size_t op3_before = op3s(s), patches_before = patches(s);
  if (!ss_check(s, ccord_mock_inject_status_proto_update(t, other, on, false, &err) == CCORD_OK, "inject idle update")) return 1;
  frames(s, 4);
  const ccord_user *me = self_user(s);
  if (!ss_check(s, me && me->presence == CCORD_PRESENCE_IDLE, "incoming: store presence is idle")) return 1;
  if (!ss_check(s, me && strcmp(me->custom_status_text, "Out to lunch") == 0 &&
                     strcmp(me->custom_status_emoji, "\xF0\x9F\x8D\x94") == 0,
                "incoming: store custom status follows")) return 1;
  if (!ss_check(s, op3s(s) > op3_before && ccord_mock_last_presence_send(t, &op3_text) == CCORD_PRESENCE_IDLE &&
                     strcmp(op3_text, "Out to lunch") == 0,
                "incoming: op 3 re-sent with idle and the custom text")) return 1;
  if (!ss_check(s, patches(s) == patches_before, "incoming: no proto write back (no echo loop)")) return 1;

  // 6. A partial update without the status field changes nothing.
  uint8_t none[64];
  size_t nn = other_client_proto(nullptr, true, none, sizeof none);
  op3_before = op3s(s);
  if (!ss_check(s, nn > 0 && ccord_mock_inject_status_proto_update(t, none, nn, true, &err) == CCORD_OK, "inject partial without status")) return 1;
  frames(s, 4);
  me = self_user(s);
  if (!ss_check(s, me && me->presence == CCORD_PRESENCE_IDLE && strcmp(me->custom_status_text, "Out to lunch") == 0 &&
                     op3s(s) == op3_before,
                "partial without status: store, presence and op 3 unchanged")) return 1;

  // 7. A partial update with only a status keeps the custom status.
  ccord_proto_status dnd = {0};
  dnd.has_status = true;
  memcpy(dnd.status, "dnd", 4);
  on = other_client_proto(&dnd, false, other, sizeof other);
  if (!ss_check(s, on > 0 && ccord_mock_inject_status_proto_update(t, other, on, true, &err) == CCORD_OK, "inject partial dnd")) return 1;
  frames(s, 4);
  me = self_user(s);
  if (!ss_check(s, me && me->presence == CCORD_PRESENCE_DND && strcmp(me->custom_status_text, "Out to lunch") == 0 &&
                     ccord_mock_last_presence_send(t, &op3_text) == CCORD_PRESENCE_DND && strcmp(op3_text, "Out to lunch") == 0,
                "partial status only: presence follows, custom status kept, op 3 re-sent")) return 1;

  // 8. A full update without a custom status clears it (another client cleared it).
  ccord_proto_status online = {0};
  online.has_status = true;
  memcpy(online.status, "online", 7);
  on = other_client_proto(&online, true, other, sizeof other);
  if (!ss_check(s, on > 0 && ccord_mock_inject_status_proto_update(t, other, on, false, &err) == CCORD_OK, "inject full online")) return 1;
  frames(s, 4);
  me = self_user(s);
  if (!ss_check(s, me && me->presence == CCORD_PRESENCE_ONLINE && me->custom_status_text[0] == '\0' &&
                     me->custom_status_emoji[0] == '\0' && ccord_mock_last_presence_send(t, &op3_text) == CCORD_PRESENCE_ONLINE &&
                     op3_text[0] == '\0',
                "full update without custom: cleared, op 3 re-sent online")) return 1;

  // 9. The next write builds on the bytes the other client wrote (the mock's server bytes are the injected proto).
  if (!ss_check(s, ccord_session_set_presence(s->c->session, CCORD_PRESENCE_IDLE, nullptr, &err) == CCORD_OK, "set idle")) return 1;
  frames(s, 3);
  size_t plen = 0;
  const uint8_t *last = ccord_mock_status_proto_patch_at(t, patches(s) - 1u, &plen);
  buf others = {0};
  buf_add(&others, k_before, sizeof k_before);
  buf_add(&others, k_after, sizeof k_after);
  if (!ss_check(s, last && plen > others.n && memcmp(last, others.d, others.n) == 0 &&
                     ccord_user_status_decode(last, plen, &ps, &err) == CCORD_OK && strcmp(ps.status, "idle") == 0 && !ps.has_custom,
                "next write builds on the other client's bytes")) return 1;

  // 10. SESSIONS_REPLACE: the only way the account learns the status it is shown with. Display only: no op 3, no write.
  size_t op3_pre = op3s(s), patches_pre = patches(s);
  if (!ss_check(s, ccord_mock_inject_sessions_replace(t, CCORD_PRESENCE_DND, &err) == CCORD_OK, "inject sessions replace")) return 1;
  frames(s, 4);
  me = self_user(s);
  if (!ss_check(s, me && me->presence == CCORD_PRESENCE_DND && op3s(s) == op3_pre && patches(s) == patches_pre,
                "sessions replace: displayed status follows, nothing is sent")) return 1;

  // 11. Two quick changes, then the echo of the first arrives late: it must not put the older status back. The echo of
  // the second changes nothing, and another client choosing the same status as our latest write still applies.
  if (!ss_check(s, ccord_session_set_presence(s->c->session, CCORD_PRESENCE_ONLINE, nullptr, &err) == CCORD_OK, "echo: set online")) return 1;
  if (!ss_check(s, ccord_session_set_presence(s->c->session, CCORD_PRESENCE_DND, nullptr, &err) == CCORD_OK, "echo: set dnd")) return 1;
  frames(s, 3);
  size_t patches_e = patches(s), op3_e = op3s(s);
  on = other_client_proto(&online, true, other, sizeof other);   // the echo of the first write (online, no custom)
  if (!ss_check(s, on > 0 && ccord_mock_inject_status_proto_update(t, other, on, false, &err) == CCORD_OK, "echo: inject first echo")) return 1;
  frames(s, 4);
  me = self_user(s);
  if (!ss_check(s, me && me->presence == CCORD_PRESENCE_DND && op3s(s) == op3_e && patches(s) == patches_e,
                "echo of an older own write is ignored (store, op 3 unchanged)")) return 1;
  on = other_client_proto(&dnd, true, other, sizeof other);      // the echo of the second write
  if (!ss_check(s, on > 0 && ccord_mock_inject_status_proto_update(t, other, on, false, &err) == CCORD_OK, "echo: inject second echo")) return 1;
  frames(s, 4);
  me = self_user(s);
  if (!ss_check(s, me && me->presence == CCORD_PRESENCE_DND && op3s(s) == op3_e, "echo of the latest write changes nothing")) return 1;
  on = other_client_proto(&idle, true, other, sizeof other);     // a real change from another client afterwards
  if (!ss_check(s, on > 0 && ccord_mock_inject_status_proto_update(t, other, on, false, &err) == CCORD_OK, "echo: inject idle")) return 1;
  frames(s, 4);
  me = self_user(s);
  if (!ss_check(s, me && me->presence == CCORD_PRESENCE_IDLE && op3s(s) > op3_e, "a later change from another client still applies")) return 1;
  return 0;
}

// ---- self activity ("Playing X" must not outlive the game) ----

static bool ss_fresh_ready(ss *s)
{
  ccord_transport *t = s->c->transport;
  ccord_mock_link_stats a, b;
  ccord_mock_link_stats_get(t, &a);
  ccord_mock_set_session_ttl(t, 1);
  ccord_mock_drop_gateway(t, false);
  b = a;
  for (int i = 0; i < 600 && b.identifies == a.identifies; i++) {
    frames(s, 1);
    ccord_mock_link_stats_get(t, &b);
  }
  frames(s, 4);
  return b.identifies > a.identifies;
}

static bool ss_no_game_in_op3(ss *s)
{
  size_t n = 0;
  const char *j = ccord_mock_rich_activities(s->c->transport, &n);
  return !j || n == 0;
}

static bool ss_self_playing(ss *s, const char *name)
{
  const ccord_user *me = self_user(s);
  return me && me->has_activity && strcmp(me->activity_name, name) == 0;
}

static void ss_step(ss *s, int n)
{
  for (int i = 0; i < n; i++) {
    s->now += 100;
    ccord_app_rpc_poll(s->rpc, s->now);
    frames(s, 1);
    struct timespec ts = {.tv_nsec = 1000000L};
    nanosleep(&ts, nullptr);
  }
}

static int ss_client_connect(const char *path)
{
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  size_t plen = strlen(path);
  if (plen >= sizeof addr.sun_path) {
    close(fd);
    return -1;
  }
  memcpy(addr.sun_path, path, plen);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool ss_send_frame(int fd, uint32_t op, const char *json)
{
  char frame[2048];
  size_t len = strlen(json);
  if (ccord_rpc_frame_encode(frame, sizeof frame, op, json, len) != CCORD_OK) return false;
  size_t off = 0;
  while (off < 8u + len) {
    ssize_t n = send(fd, frame + off, 8u + len - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

// The RPC client case: a client sets "Playing Tipsy" and then goes away without clearing it (the process dies or the
// socket closes). Nothing echoes the own presence back here, as Discord may not.
static int ss_rpc_client_vanishes(ss *s)
{
  ccord_transport *t = s->c->transport;
  ccord_error err = {0};
  char dir[512];
  const char *tmp = getenv("TMPDIR");
  (void)snprintf(dir, sizeof dir, "%s/ccord_ssy_XXXXXX", (tmp && tmp[0]) ? tmp : ".");
  if (!mkdtemp(dir)) return ss_check(s, false, "rpc: scratch dir") ? 0 : 1;
  ccord_app_rpc_config cfg = {.session = s->c->session, .dir = dir, .coalesce_ms = 0};
  s->rpc = ccord_app_rpc_create(&cfg, &err);
  int fd = -1, rc = 1;
  char path[600];
  if (!ss_check(s, s->rpc != nullptr, "rpc: server starts")) goto out;
  (void)snprintf(path, sizeof path, "%s", ccord_app_rpc_path(s->rpc));
  s->now = 10000;
  fd = ss_client_connect(path);
  if (!ss_check(s, fd >= 0 && ss_send_frame(fd, 0, "{\"v\":1,\"client_id\":\"1234567890123456789\"}"), "rpc: client connects")) goto out;
  ss_step(s, 5);
  if (!ss_check(s,
                ss_send_frame(fd, 1,
                              "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"n\",\"args\":{\"pid\":4242,\"activity\":"
                              "{\"type\":0,\"name\":\"Tipsy\",\"details\":\"In a match\"}}}"),
                "rpc: client sets Playing Tipsy"))
    goto out;
  for (int i = 0; i < 40 && ss_no_game_in_op3(s); i++) ss_step(s, 1);
  if (!ss_check(s, !ss_no_game_in_op3(s), "rpc: the activity reaches the gateway presence")) goto out;
  // Discord reflects it back to the account: the own user shows Playing Tipsy.
  if (!ss_check(s, ccord_mock_set_self_activity(t, "Tipsy", true, &err) == CCORD_OK, "rpc: echo own activity")) goto out;
  ss_step(s, 4);
  if (!ss_check(s, ss_self_playing(s, "Tipsy"), "rpc: store self shows Playing Tipsy")) goto out;
  // The client dies: no CLEAR, the socket just closes. Discord does not echo the clear back to the account.
  ccord_mock_set_own_presence_echo(t, false);
  size_t op3_before = op3s(s);
  close(fd);
  fd = -1;
  for (int i = 0; i < 40 && !ss_no_game_in_op3(s); i++) ss_step(s, 1);
  ss_step(s, 4);
  if (!ss_check(s, ss_no_game_in_op3(s), "rpc: after the client vanished the next op 3 carries no game activity")) goto out;
  if (!ss_check(s, op3s(s) > op3_before, "rpc: a presence update went out for the clear")) goto out;
  if (!ss_check(s, self_user(s) && !self_user(s)->has_activity && self_user(s)->activity_name[0] == '\0',
                "rpc: store self no longer shows Playing Tipsy"))
    goto out;
  rc = 0;
out:
  ccord_mock_set_own_presence_echo(t, true);
  if (fd >= 0) close(fd);
  ccord_app_rpc_destroy(s->rpc);
  s->rpc = nullptr;
  (void)rmdir(dir);
  return rc;
}

static int ss_self_activity(ss *s)
{
  ccord_transport *t = s->c->transport;
  ccord_error err = {0};
  ccord_presence pres = self_user(s) ? self_user(s)->presence : CCORD_PRESENCE_ONLINE;

  // A. Another session stops the game: SESSIONS_REPLACE lists no activity.
  if (!ss_check(s, ccord_mock_set_self_activity(t, "Tipsy", true, &err) == CCORD_OK, "activity: echo own activity")) return 1;
  frames(s, 4);
  if (!ss_check(s, ss_self_playing(s, "Tipsy"), "activity: store self shows Playing Tipsy")) return 1;
  size_t op3_pre = op3s(s);
  if (!ss_check(s, ccord_mock_inject_sessions_replace_activity(t, pres, nullptr, &err) == CCORD_OK, "activity: inject sessions replace without activity")) return 1;
  frames(s, 4);
  if (!ss_check(s, self_user(s) && !self_user(s)->has_activity && self_user(s)->activity_name[0] == '\0',
                "activity: sessions replace without activities clears the own Playing line"))
    return 1;
  if (!ss_check(s, self_user(s)->presence == pres && op3s(s) == op3_pre, "activity: presence kept, nothing sent")) return 1;
  // ... and another session starting one shows it.
  if (!ss_check(s, ccord_mock_inject_sessions_replace_activity(t, pres, "Tipsy", &err) == CCORD_OK, "activity: inject sessions replace with activity")) return 1;
  frames(s, 4);
  if (!ss_check(s, ss_self_playing(s, "Tipsy") && self_user(s)->activity_type == CCORD_ACTIVITY_PLAYING,
                "activity: sessions replace with an activity shows it"))
    return 1;

  // B. A fresh READY no longer lists it (it ended while this client was away): the snapshot rebuilds the users, so the old
  // line must not come back (guard; the H1 'upsert keeps the old activity' path does not run for a READY).
  if (!ss_check(s, ccord_mock_set_self_activity(t, nullptr, false, &err) == CCORD_OK, "activity: server drops the activity")) return 1;
  if (!ss_check(s, ss_fresh_ready(s), "activity: fresh READY delivered")) return 1;
  if (!ss_check(s, self_user(s) && !self_user(s)->has_activity && self_user(s)->activity_name[0] == '\0',
                "activity: a READY without the activity leaves no own Playing line"))
    return 1;

  // C. Cleared while the app is still on the cache (stale), then READY: the clear goes out, never the old activity.
  static const char tipsy[] = "[{\"name\":\"Tipsy\",\"type\":0}]";
  if (!ss_check(s, ccord_session_set_rich_presence(s->c->session, tipsy, &err) == CCORD_OK, "stale: set activity")) return 1;
  frames(s, 3);
  if (!ss_check(s, !ss_no_game_in_op3(s), "stale: the activity is published")) return 1;
  ccord_session_set_stale(s->c->session, true);
  size_t rich_ops = op3s(s);
  if (!ss_check(s, ccord_session_set_rich_presence(s->c->session, nullptr, &err) != CCORD_OK, "stale: clear is refused while stale")) return 1;
  if (!ss_check(s, !ss_no_game_in_op3(s) && op3s(s) == rich_ops, "stale: nothing reached the transport while stale")) return 1;
  if (!ss_check(s, ccord_mock_set_self_activity(t, nullptr, false, &err) == CCORD_OK, "stale: server drops the activity")) return 1;
  if (!ss_check(s, ss_fresh_ready(s), "stale: fresh READY delivered")) return 1;
  if (!ss_check(s, ss_no_game_in_op3(s), "stale: after READY the op 3 carries no game activity")) return 1;
  if (!ss_check(s, op3s(s) > rich_ops, "stale: the clear went out as a presence update")) return 1;
  if (!ss_check(s, op3s(s) == rich_ops + 1u, "stale: READY did not re-push the withdrawn activity first (one op 3, not two)")) return 1;
  if (!ss_check(s, self_user(s) && !self_user(s)->has_activity, "stale: store self shows no Playing line")) return 1;

  // D. The RPC client goes away without clearing.
  return ss_rpc_client_vanishes(s);
}

int ccord_verify_status_sync_slice(const ccord_verify_ui_parity_ctx *ctx)
{
  ss s = {.c = ctx};
  if (!ctx || !ctx->session || !ctx->store || !ctx->transport || !ctx->frames) {
    printf("status-sync: FAIL missing context\n");
    fflush(stdout);
    return 1;
  }
  if (ss_run(&s) != 0 || ss_self_activity(&s) != 0) {
    printf("status-sync: FAIL %s\n", s.fail[0] ? s.fail : "check");
    fflush(stdout);
    return 1;
  }
  printf("status-sync: PASS checks=%d\n", s.checks);
  fflush(stdout);
  return 0;
}
