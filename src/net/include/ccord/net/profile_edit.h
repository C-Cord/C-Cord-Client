// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Editing the own Discord profile:
//   Global:     PATCH /users/@me (global_name, avatar, banner, avatar_decoration_sku_id, nameplate_sku_id),
//               PATCH /users/@me/profile (bio, pronouns, accent_color, theme_colors, profile_effect_id,
//                                         profile_frame_sku_id),
//               PUT   /users/@me/clan (identity_guild_id / identity_enabled).
//   Per-server: PATCH /guilds/{gid}/members/@me (nick, avatar, avatar_decoration_sku_id, nameplate_sku_id),
//               PATCH /guilds/{gid}/profile/@me (banner, bio, pronouns, accent_color, theme_colors,
//                                                profile_effect_id, profile_frame_sku_id).
// Callers fill a ccord_profile_edit with only the fields they changed; this module validates it, builds the REST
// bodies, sends them sequentially through a ccord_profile_rest and reports what Discord answered. It never touches
// the store: ccord_session_edit_profile applies the answer.
//
// Hard limits, enforced before anything is sent: profile requests never carry username, discriminator, email,
// password, new_password, code or any MFA field (those belong strictly to account_edit); images must be PNG, JPEG,
// GIF or WebP by magic bytes and at most 10 MiB; bio <= 190, pronouns <= 40, display name/nick <= 32 characters.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/buf.h>
#include <ccord/core/error.h>
#include <ccord/model/model.h>

#define CCORD_PROFILE_BIO_MAX 190u             // characters (code points)
#define CCORD_PROFILE_PRONOUNS_MAX 40u
#define CCORD_PROFILE_DISPLAY_NAME_MAX 32u
#define CCORD_PROFILE_IMAGE_MAX_BYTES (10u * 1024u * 1024u)
#define CCORD_PROFILE_ACCENT_MAX 0xFFFFFFu
#define CCORD_PROFILE_MAX_FIELD_ERRORS 8u

typedef enum ccord_profile_field : uint8_t {
  CCORD_PROFILE_FIELD_DISPLAY_NAME = 0,   // global_name (or guild nick)
  CCORD_PROFILE_FIELD_AVATAR,
  CCORD_PROFILE_FIELD_BANNER,
  CCORD_PROFILE_FIELD_BIO,
  CCORD_PROFILE_FIELD_PRONOUNS,
  CCORD_PROFILE_FIELD_ACCENT,             // accent_color
  CCORD_PROFILE_FIELD_THEME_COLORS,       // theme_colors: [primary, accent] or null
  CCORD_PROFILE_FIELD_AVATAR_DECORATION,  // avatar_decoration_sku_id
  CCORD_PROFILE_FIELD_PROFILE_EFFECT,     // profile_effect_id
  CCORD_PROFILE_FIELD_PROFILE_FRAME,      // profile_frame_sku_id
  CCORD_PROFILE_FIELD_NAMEPLATE,          // collectibles.nameplate
  CCORD_PROFILE_FIELD_CLAN_GUILD,         // PUT /users/@me/clan
  CCORD_PROFILE_FIELD_COUNT,
  CCORD_PROFILE_FIELD_UNKNOWN = 0xFF
} ccord_profile_field;

// What to change. Zero-initialise (CCORD_PROFILE_EDIT_INIT), then set `has_*` for each field to send.
typedef struct ccord_profile_edit {
  bool has_display_name;
  const char *display_name;        // NULL or "" clears it (global_name / guild nick null)
  bool has_avatar;
  const uint8_t *avatar;           // raw image file bytes; NULL with avatar_len 0 removes the avatar (null)
  size_t avatar_len;
  bool has_banner;
  const uint8_t *banner;           // same convention as avatar
  size_t banner_len;
  bool has_bio;
  const char *bio;                 // NULL or "" clears it
  bool has_pronouns;
  const char *pronouns;            // NULL or "" clears them
  bool has_accent;
  int32_t accent_color;            // 0..0xFFFFFF; negative clears it (null)
  // Extended profile fields (zero-initialised cleanly by CCORD_PROFILE_EDIT_INIT):
  ccord_id guild_id;               // CCORD_ID_NONE = global main profile; non-zero = per-server profile
  bool has_theme_colors;
  bool clear_theme_colors;         // true -> theme_colors: null
  uint32_t theme_primary;          // 0..0xFFFFFF
  uint32_t theme_accent;           // 0..0xFFFFFF
  bool has_avatar_decoration;
  ccord_id avatar_decoration_sku_id; // 0 clears (null)
  const char *avatar_decoration_asset; // optional CDN asset hash for immediate local preview sync
  bool has_profile_effect;
  const char *profile_effect_id;   // NULL or "" clears (null)
  bool has_profile_frame;
  ccord_id profile_frame_sku_id;   // 0 clears (null)
  bool has_nameplate;
  ccord_id nameplate_sku_id;       // 0 clears (null)
  const char *nameplate_asset;     // optional asset path
  const char *nameplate_palette;   // optional palette name
  bool has_clan_guild;             // global profile only (PUT /users/@me/clan)
  ccord_id clan_guild_id;          // 0 clears (identity_enabled: false)
  const char *clan_tag;            // optional 2-4 char tag for local store sync
  const char *clan_badge;          // optional badge hash for local store sync
} ccord_profile_edit;
#define CCORD_PROFILE_EDIT_INIT {0}

// One fully built HTTP request. `body` is heap memory owned by the plan.
typedef struct ccord_profile_request {
  char path[96];                   // "/users/@me", "/users/@me/profile", "/users/@me/clan", or guild equivalents
  bool use_put;                    // false = PATCH, true = PUT
  char *body;                      // compact JSON, NUL-terminated
  size_t body_len;
  uint32_t fields;                 // bit (1u << ccord_profile_field) per field this request carries
} ccord_profile_request;

typedef struct ccord_profile_plan {
  ccord_profile_request req[3];    // [0] identity/media, [1] profile, [2] clan (only non-empty requests included)
  size_t count;                    // 0..3
} ccord_profile_plan;

// Validates `edit` and builds the requests. Only changed fields are included; a request with no fields is
// omitted. CCORD_ERR_INVALID: nothing to change, bad UTF-8/control characters, or an image that is not PNG, JPEG,
// GIF or WebP. CCORD_ERR_RANGE: a text field over its limit, accent out of range, or an image over 10 MiB.
// On failure *out is empty and nothing needs freeing.
[[nodiscard]] ccord_status ccord_profile_edit_plan(const ccord_profile_edit *edit, ccord_profile_plan *out,
                                                   ccord_error *err);
void ccord_profile_plan_free(ccord_profile_plan *plan);

// "image/png", "image/jpeg", "image/gif" or "image/webp" from the magic bytes; NULL for anything else.
[[nodiscard]] const char *ccord_profile_image_mime(const uint8_t *data, size_t len);

// CDN URL the client shows for an avatar ("avatars") or banner ("banners") hash; false when it does not fit/hash
// is empty. Same shape the gateway mapper builds, so the store shows the same URL a refetch would give.
[[nodiscard]] bool ccord_profile_cdn_url(const char *kind, ccord_id user_id, const char *hash, char *out, size_t cap);

// ---- what Discord answered ----

typedef struct ccord_profile_field_error {
  ccord_profile_field field;       // CCORD_PROFILE_FIELD_UNKNOWN when Discord named a field we do not send
  char name[32];                   // the JSON field name as Discord wrote it ("banner", "avatar", ...)
  char code[48];                   // Discord's per-field code ("BANNER_PREMIUM_REQUIRED") or ""
  char message[200];               // Discord's per-field message, never empty for a listed error
} ccord_profile_field_error;

// The values Discord confirmed. A field is valid only when its bit is set in `fields`; an empty string / -1 then
// means "cleared".
typedef struct ccord_profile_applied {
  uint32_t fields;                 // bit (1u << ccord_profile_field)
  ccord_id guild_id;               // CCORD_ID_NONE = global; non-zero = per-server profile
  char display_name[CCORD_NAME_MAX_BYTES + 1];
  char avatar_hash[128];
  char banner_hash[128];
  char bio[CCORD_PROFILE_BIO_MAX * 4u + 1u];
  char pronouns[CCORD_PROFILE_PRONOUNS_MAX * 4u + 1u];
  int32_t accent_color;            // -1 = none
  bool theme_colors_set;           // true when theme_primary/theme_accent active; false = cleared
  uint32_t theme_primary;
  uint32_t theme_accent;
  ccord_id avatar_decoration_sku_id;
  char avatar_decoration_url[256];
  char profile_effect_id[64];
  ccord_id profile_frame_sku_id;
  ccord_id nameplate_sku_id;
  char nameplate_asset[128];
  char nameplate_palette[24];
  ccord_id clan_guild_id;
  char clan_tag[16];
  char clan_badge[64];
} ccord_profile_applied;

typedef struct ccord_profile_edit_result {
  ccord_status status;             // CCORD_OK when every request was accepted; else the first failure
  long http_status;                // status of the failing request (200/204 on success); 0 when none was answered
  uint32_t requests_sent;          // 0..2
  ccord_profile_applied applied;   // fields Discord accepted (a later request can fail after an earlier one landed)
  char message[200];               // Discord's top-level message ("Invalid Form Body"), or a transport description
  int32_t code;                    // Discord's error code (0 when none), e.g. 50035, 60003
  double retry_after_s;            // 429 only: the retry_after Discord reported (0 otherwise)
  bool rate_limited;               // HTTP 429
  bool captcha_required;           // Discord asked for a captcha (captcha_key in the body)
  bool password_required;          // Discord wants the account password (code 50018 or a "password" field error)
  bool mfa_required;               // Discord wants a 2FA code (code 60003)
  size_t field_error_count;
  ccord_profile_field_error field_errors[CCORD_PROFILE_MAX_FIELD_ERRORS];
} ccord_profile_edit_result;

// Convention: the callback runs exactly once per accepted ccord_profile_edit_start, from ccord_profile_rest_pump
// (never from inside _start), also with status != CCORD_OK when the rest object is destroyed first. `res` is valid
// only during the call.
typedef void (*ccord_profile_edit_cb)(void *ud, const ccord_profile_edit_result *res);

// Callback for GET /users/@me/collectibles-purchases (+ /user-profile-effects enrichment).
// `items` is valid only during the callback (caller copies what it keeps).
typedef void (*ccord_collectibles_cb)(void *ud, ccord_status status, long http_status,
                                      const ccord_owned_collectible *items, size_t count, const char *message);

// ---- sending ----

// Where the requests go. Both backends answer asynchronously and call back from ccord_profile_rest_pump.
typedef struct ccord_profile_rest ccord_profile_rest;

// Authenticated REST queue (request governor, 429 retry, X-Super-Properties, Authorization) against `base_url`
// (NULL = https://discord.com/api/v9). The token is copied and wiped on destroy. CCORD_ERR_UNSUPPORTED (NULL)
// when the build has no live transport.
[[nodiscard]] ccord_profile_rest *ccord_profile_rest_create_live(const char *base_url, const char *token,
                                                                 ccord_error *err);
// In-process stand-in for Discord's two routes, for --mock: it applies the same JSON bodies and answers with the
// objects Discord would, on the next pump.
[[nodiscard]] ccord_profile_rest *ccord_profile_rest_create_mock(ccord_id self_id, ccord_error *err);
// Mock only: the next request is answered with this status and body instead of being applied (consumed once).
void ccord_profile_rest_mock_fail_next(ccord_profile_rest *r, long http_status, const char *json_body);
// Mock only: how many requests it has received, and the last path/body (valid until the next request).
size_t ccord_profile_rest_mock_count(const ccord_profile_rest *r);
const char *ccord_profile_rest_mock_last_path(const ccord_profile_rest *r);
const char *ccord_profile_rest_mock_last_body(const ccord_profile_rest *r);

void ccord_profile_rest_destroy(ccord_profile_rest *r);   // pending edits call back with CCORD_ERR_IO
void ccord_profile_rest_pump(ccord_profile_rest *r, uint64_t now_ms);
bool ccord_profile_rest_pending(const ccord_profile_rest *r);

// Validates, then queues the first request. Returns CCORD_OK once queued (the callback follows); on any error
// nothing was sent and the callback will not run. A second request (/users/@me/profile) is sent only after the
// first one succeeded.
[[nodiscard]] ccord_status ccord_profile_edit_start(ccord_profile_rest *r, const ccord_profile_edit *edit,
                                                    ccord_profile_edit_cb cb, void *ud, ccord_error *err);

// Fetches the authenticated user's owned Discord Collectibles (Avatar Decorations, Profile Effects, Nameplates)
// from GET /users/@me/collectibles-purchases?variants_return_style=1 and enriches Profile Effects via
// GET /user-profile-effects. Runs asynchronously and calls `cb` from ccord_profile_rest_pump.
[[nodiscard]] ccord_status ccord_profile_rest_fetch_collectibles(ccord_profile_rest *r, ccord_collectibles_cb cb,
                                                                 void *ud, ccord_error *err);

// Parsing helper exposed for tests: fills `res` from an HTTP answer (status + body) of the request carrying
// `fields`. Returns true when the answer was a success.
bool ccord_profile_edit_parse_answer(uint32_t fields, long http_status, const char *body, size_t len,
                                     ccord_profile_edit_result *res);
