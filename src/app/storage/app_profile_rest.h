// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// App wiring for own-profile edits: creates the ccord_profile_rest backend (src/net/profile_edit.h) and attaches
// it to the session (ccord_session_set_profile_rest), which owns it, pumps it and destroys it. Without this call
// ccord_session_edit_profile answers CCORD_ERR_UNSUPPORTED.
//
// The live backend carries its own copy of the token, so it must follow the account: the app calls
// ccord_app_profile_rest_attach_live whenever the live transport is (re)built (it replaces the old backend, which
// is destroyed with the old token) and ccord_app_profile_rest_detach on sign-out.
#include <stdbool.h>

#include <ccord/core/error.h>
#include <ccord/model/model.h>
#include <ccord/session/session.h>

// --mock: the in-process stand-in for Discord's two routes. `self_id` is only echoed in its answers (the store's
// own user is what the session updates); pass CCORD_ID_NONE when the session has not signed in yet.
// Returns false and fills `err` on failure; the session then has no backend.
[[nodiscard]] bool ccord_app_profile_rest_attach_mock(ccord_session *s, ccord_id self_id, ccord_error *err);

// Live account: loads the token exactly like the live transport does (CCORD_DISCORD_TOKEN, else `token_file`,
// else the standalone token file), then builds an authenticated REST queue against `api_base` (null = Discord)
// and attaches it. Any previous backend is detached first, so a failure leaves none rather than the old account's.
[[nodiscard]] bool ccord_app_profile_rest_attach_live(ccord_session *s, const char *token_file, const char *api_base,
                                                      ccord_error *err);

// Destroys the session's profile backend (pending edits answer CCORD_ERR_IO). Null-safe. Use on sign-out and at
// exit before ccord_session_destroy.
void ccord_app_profile_rest_detach(ccord_session *s);
