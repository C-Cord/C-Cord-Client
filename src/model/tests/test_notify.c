// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/model/notify.h>

#include "ccord_test.h"

#define MIN_MS (60LL * 1000)
#define HOUR_MS (60LL * MIN_MS)
#define NOW 1800000000000LL

static ccord_notify_scope row(bool muted, int64_t until, ccord_notify_level level)
{
  return (ccord_notify_scope){.guild_id = 10, .channel_id = 0, .level = level, .muted = muted, .mute_until_ms = until};
}

static void test_mute_expiry(void)
{
  ccord_notify_scope forever = row(true, 0, CCORD_NOTIFY_ALL);
  ccord_notify_scope timed = row(true, NOW + HOUR_MS, CCORD_NOTIFY_ALL);
  ccord_notify_scope over = row(true, NOW - 1, CCORD_NOTIFY_ALL);
  ccord_notify_scope off = row(false, NOW + HOUR_MS, CCORD_NOTIFY_ALL);   // an end time without the flag mutes nothing
  CT_CHECK(ccord_notify_scope_muted(&forever, NOW));
  CT_CHECK(ccord_notify_scope_muted(&forever, NOW + 1000 * HOUR_MS));
  CT_CHECK(ccord_notify_scope_muted(&timed, NOW));
  CT_CHECK(ccord_notify_scope_muted(&timed, NOW + HOUR_MS - 1));
  CT_CHECK(!ccord_notify_scope_muted(&timed, NOW + HOUR_MS));       // it ends at its end time
  CT_CHECK(!ccord_notify_scope_muted(&over, NOW));
  CT_CHECK(!ccord_notify_scope_muted(&off, NOW));
  CT_CHECK(ccord_notify_scope_muted(&timed, 0));                    // no clock: believe the mute
  CT_CHECK(!ccord_notify_scope_muted(nullptr, NOW));
}

static void test_resolve_mute(void)
{
  ccord_notify_scope g = row(false, 0, CCORD_NOTIFY_ALL), cat = row(false, 0, CCORD_NOTIFY_INHERIT),
                     ch = row(false, 0, CCORD_NOTIFY_INHERIT);
  CT_CHECK(!ccord_notify_resolve(&g, &cat, &ch, NOW).muted);
  CT_CHECK(!ccord_notify_resolve(nullptr, nullptr, nullptr, NOW).muted);
  g.muted = true;                                                    // a muted server mutes every channel
  CT_CHECK(ccord_notify_resolve(&g, &cat, &ch, NOW).muted);
  CT_CHECK(ccord_notify_resolve(&g, nullptr, nullptr, NOW).muted);
  g.muted = false;
  cat.muted = true;                                                  // so does a muted category
  CT_CHECK(ccord_notify_resolve(&g, &cat, &ch, NOW).muted);
  cat.muted = false;
  ch.muted = true;                                                   // and the channel itself
  CT_CHECK(ccord_notify_resolve(&g, &cat, &ch, NOW).muted);
  CT_CHECK(ccord_notify_resolve(nullptr, nullptr, &ch, NOW).muted);
  ch.mute_until_ms = NOW + 5 * MIN_MS;
  CT_CHECK(ccord_notify_resolve(&g, &cat, &ch, NOW).muted);
  CT_CHECK(!ccord_notify_resolve(&g, &cat, &ch, NOW + 5 * MIN_MS).muted);   // a finished mute no longer counts
}

static void test_resolve_level(void)
{
  ccord_notify_scope g = row(false, 0, CCORD_NOTIFY_MENTIONS), cat = row(false, 0, CCORD_NOTIFY_INHERIT),
                     ch = row(false, 0, CCORD_NOTIFY_INHERIT);
  CT_CHECK(ccord_notify_resolve(&g, &cat, &ch, NOW).level == CCORD_NOTIFY_MENTIONS);         // from the server
  cat.level = CCORD_NOTIFY_NOTHING;
  CT_CHECK(ccord_notify_resolve(&g, &cat, &ch, NOW).level == CCORD_NOTIFY_NOTHING);          // the category wins
  ch.level = CCORD_NOTIFY_ALL;
  CT_CHECK(ccord_notify_resolve(&g, &cat, &ch, NOW).level == CCORD_NOTIFY_ALL);              // the channel wins over both
  CT_CHECK(ccord_notify_resolve(nullptr, nullptr, nullptr, NOW).level == CCORD_NOTIFY_ALL);  // nothing known: all messages
  CT_CHECK(!ccord_notify_resolve(nullptr, nullptr, nullptr, NOW).level_set);                 // ...but nobody chose it
  CT_CHECK(ccord_notify_resolve(&g, &cat, &ch, NOW).level_set);
  g.level = CCORD_NOTIFY_INHERIT;
  ccord_notify_scope plain = row(false, 0, CCORD_NOTIFY_INHERIT);
  CT_CHECK(ccord_notify_resolve(&g, &plain, &plain, NOW).level == CCORD_NOTIFY_ALL);         // nobody sets one
  CT_CHECK(!ccord_notify_resolve(&g, &plain, &plain, NOW).level_set);
  ccord_notify_scope junk = row(false, 0, (ccord_notify_level)9);
  CT_CHECK(ccord_notify_resolve(&junk, nullptr, nullptr, NOW).level == CCORD_NOTIFY_ALL);    // an unknown level is ignored
  CT_CHECK(!ccord_notify_resolve(&junk, nullptr, nullptr, NOW).level_set);
  g = row(false, 0, CCORD_NOTIFY_ALL);
  g.suppress_everyone = true;
  CT_CHECK(ccord_notify_resolve(&g, nullptr, &ch, NOW).suppress_everyone);
  CT_CHECK(!ccord_notify_resolve(&g, nullptr, &ch, NOW).suppress_roles);
  g.suppress_everyone = false;
  g.suppress_roles = true;
  CT_CHECK(ccord_notify_resolve(&g, nullptr, &ch, NOW).suppress_roles);
}

static void test_should_notify(void)
{
  ccord_notify_effective all = {.level = CCORD_NOTIFY_ALL};
  ccord_notify_effective men = {.level = CCORD_NOTIFY_MENTIONS};
  ccord_notify_effective none = {.level = CCORD_NOTIFY_NOTHING};
  ccord_notify_effective muted = {.muted = true, .level = CCORD_NOTIFY_ALL};
  CT_CHECK(ccord_notify_should_notify(&all, false, false, false));
  CT_CHECK(ccord_notify_should_notify(&all, true, false, false));
  CT_CHECK(!ccord_notify_should_notify(&men, false, false, false));          // plain message, mentions only
  CT_CHECK(ccord_notify_should_notify(&men, true, false, false));
  CT_CHECK(ccord_notify_should_notify(&men, false, true, false));            // @everyone counts as a mention...
  CT_CHECK(ccord_notify_should_notify(&men, false, false, true));            // ...and so does one of your roles
  CT_CHECK(!ccord_notify_should_notify(&none, true, true, true));
  CT_CHECK(!ccord_notify_should_notify(&muted, true, true, true));
  CT_CHECK(!ccord_notify_should_notify(nullptr, true, true, true));
  men.suppress_everyone = true;                                              // suppressing drops only the broad mentions
  CT_CHECK(!ccord_notify_should_notify(&men, false, true, false));
  CT_CHECK(ccord_notify_should_notify(&men, true, true, false));
  CT_CHECK(ccord_notify_should_notify(&men, false, false, true));
  men.suppress_roles = true;
  CT_CHECK(!ccord_notify_should_notify(&men, false, false, true));
  CT_CHECK(ccord_notify_should_notify(&men, true, false, true));
  all.suppress_everyone = true;                                              // "All Messages" still notifies for a message
  CT_CHECK(ccord_notify_should_notify(&all, false, true, false));
}

static void test_labels_and_options(void)
{
  CT_CHECK(strcmp(ccord_notify_level_label(CCORD_NOTIFY_ALL), "All Messages") == 0);
  CT_CHECK(strcmp(ccord_notify_level_label(CCORD_NOTIFY_MENTIONS), "Only @mentions") == 0);
  CT_CHECK(strcmp(ccord_notify_level_label(CCORD_NOTIFY_NOTHING), "Nothing") == 0);
  CT_CHECK(strcmp(ccord_notify_level_label(CCORD_NOTIFY_INHERIT), "Server Default") == 0);
  size_t n = 0;
  const ccord_mute_option *o = ccord_notify_mute_options(&n);
  CT_CHECK(o && n == 6);
  CT_CHECK(o[0].duration_ms == 15 * MIN_MS && o[1].duration_ms == HOUR_MS && o[2].duration_ms == 3 * HOUR_MS &&
           o[3].duration_ms == 8 * HOUR_MS && o[4].duration_ms == 24 * HOUR_MS);
  CT_CHECK(o[5].duration_ms == 0 && strstr(o[5].label, "turn it back on") != nullptr);
  for (size_t i = 0; i < n; i++) CT_CHECK(o[i].label && o[i].label[0]);
  CT_CHECK(ccord_notify_mute_options(nullptr) == o);
}

static void test_remaining_text(void)
{
  char t[64];
  ccord_notify_scope s = row(true, NOW + 12 * MIN_MS, CCORD_NOTIFY_ALL);
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && strcmp(t, "12 minutes left") == 0);
  s.mute_until_ms = NOW + 61 * 1000;
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && strcmp(t, "2 minutes left") == 0);   // rounds up
  s.mute_until_ms = NOW + 1;
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && strcmp(t, "1 minute left") == 0);
  s.mute_until_ms = NOW + HOUR_MS;
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && strcmp(t, "1 hour left") == 0);
  s.mute_until_ms = NOW + 3 * HOUR_MS - MIN_MS;
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && strcmp(t, "3 hours left") == 0);
  s.mute_until_ms = NOW + 24 * HOUR_MS;
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && strcmp(t, "1 day left") == 0);
  s.mute_until_ms = NOW + 49 * HOUR_MS;
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && strcmp(t, "3 days left") == 0);
  s.mute_until_ms = 0;
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && strcmp(t, "until you turn it back on") == 0);
  s.muted = false;
  CT_CHECK(!ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t) && t[0] == '\0');
  s = row(true, NOW - 5, CCORD_NOTIFY_ALL);
  CT_CHECK(!ccord_notify_mute_remaining_text(&s, NOW, t, sizeof t));                                   // already over
  s = row(true, NOW + HOUR_MS, CCORD_NOTIFY_ALL);
  char tiny[8];
  CT_CHECK(ccord_notify_mute_remaining_text(&s, NOW, tiny, sizeof tiny) && strlen(tiny) < sizeof tiny);   // truncates, terminates
  CT_CHECK(!ccord_notify_mute_remaining_text(&s, NOW, nullptr, 0));
}

int main(void)
{
  test_mute_expiry();
  test_resolve_mute();
  test_resolve_level();
  test_should_notify();
  test_labels_and_options();
  test_remaining_text();
  return CT_RESULT("test_notify");
}
