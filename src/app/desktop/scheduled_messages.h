// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Scheduled messages (client-side): the user queues text for a channel and C-Cord sends it through the session's
// ordinary text send once it is due, while C-Cord is running. The queue lives on disk per account
// (<data dir>/accounts/<user id>/scheduled_messages.json, mode 0600, atomic replace) and is loaded once the signed-in
// user's id is known. An entry more than CCORD_SCHEDULED_MISSED_MS overdue when the queue is loaded is marked
// missed: it is never auto-sent, the user picks Send now or Delete. A failed send keeps the entry with its error and
// retries with backoff. A send the session accepted is final here: a later refusal shows as a failed message in the
// chat, which the user can retry from there.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>
#include <ccord/session/session.h>
#include <ccord/ui/ui.h>

#define CCORD_SCHEDULED_MIN_LEAD_MS (60 * 1000)          // a message must be scheduled at least this far ahead
#define CCORD_SCHEDULED_MISSED_MS (5 * 60 * 1000)        // overdue by more than this at load => missed

// Once at startup, after the session and store exist. `store` supplies the signed-in user id (it names the account's queue
// directory). Loads the queue when the user id is already known (else tick does).
void ccord_scheduled_messages_init(ccord_session *session, ccord_store *store);
// Once per main-loop iteration. now_unix_ms = wall-clock time (ms since the Unix epoch). Sends at most one due message
// per call through session.
void ccord_scheduled_messages_tick(ccord_session *session, int64_t now_unix_ms);
// Milliseconds until the next non-missed scheduled message (or retry) becomes due; UINT32_MAX when none pending or offline.
uint32_t ccord_scheduled_messages_next_due_ms(int64_t now_unix_ms);
// True when at least one scheduled message is currently queued (fast O(1) check before reading the wall clock).
bool ccord_scheduled_messages_has_entries(void);
// Once at shutdown, before the session is destroyed. Writes nothing new (every change is saved when made).
void ccord_scheduled_messages_shutdown(void);

// Clock for ccord_scheduled_messages_now_ms (tests inject a fake one); nullptr restores the wall clock.
void ccord_scheduled_messages_set_clock(int64_t (*now_unix_ms)(void *ud), void *ud);
int64_t ccord_scheduled_messages_now_ms(void);

// Queues `utf8` for channel_id at due_unix_ms. Refused (CCORD_ERR_INVALID / RANGE / NOT_FOUND) for blank or invalid or
// over-long text, a due time less than CCORD_SCHEDULED_MIN_LEAD_MS after now_unix_ms, a full queue (CCORD_UI_SCHEDULED_MAX)
// or before the signed-in user is known. out_id (optional) gets the entry id.
[[nodiscard]] ccord_status ccord_scheduled_messages_add(ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                                                        int64_t now_unix_ms, uint64_t *out_id, ccord_error *err);
// Same, with files: `paths` are npaths absolute file paths (<= CCORD_RICH_MAX_ATTACHMENTS) sent with the message when
// due; spoilers (optional) flags each as a spoiler. Text may be blank when there is at least one file. The files are read at
// send time: a missing, unreadable or over-size (25 MB) one marks the entry failed with the file named; nothing is sent.
[[nodiscard]] ccord_status ccord_scheduled_messages_add_files(ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                                                              int64_t now_unix_ms, const char *const *paths, const bool *spoilers,
                                                              size_t npaths, uint64_t *out_id, ccord_error *err);
// This account's entries, soonest first. Returns the number written (<= cap).
size_t ccord_scheduled_messages_list(ccord_ui_scheduled_item *out, size_t cap);
// Removes an entry without sending it. False when the id is unknown.
bool ccord_scheduled_messages_cancel(uint64_t id);
// Sends an entry now (also a missed one); removed on success. On failure the entry stays with its error.
[[nodiscard]] ccord_status ccord_scheduled_messages_send_now(uint64_t id, ccord_error *err);
// Path of the queue file in use ("" before the account is known). For tests.
const char *ccord_scheduled_messages_path(void);
