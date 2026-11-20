// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *const mk_user_names[MOCK_NUM_USERS] = {
  "You", "Ada", "Bjorn", "Chen", "Dara", "Eli", "Fatima", "Gus", "Hana", "Ivo", "Lua Bot", "Pixel Bot",
};
static const char *const user_avatars[MOCK_NUM_USERS] = {
  "https://cdn.discordapp.com/avatars/1001/a0.png", "https://cdn.discordapp.com/avatars/1002/a1.png",
  "https://cdn.discordapp.com/avatars/1003/a2.png", "https://cdn.discordapp.com/avatars/1004/a3.png",
  "https://cdn.discordapp.com/avatars/1005/a4.png", "https://cdn.discordapp.com/avatars/1006/a5.png",
  "https://cdn.discordapp.com/avatars/1007/a6.png", "https://cdn.discordapp.com/avatars/1008/a7.png",
  "https://cdn.discordapp.com/avatars/1009/a8.png", "https://cdn.discordapp.com/avatars/1010/a9.png",
  "https://cdn.discordapp.com/avatars/1011/a10.png", "https://cdn.discordapp.com/avatars/1012/a11.png",
};
static const char *const guild_names[MOCK_NUM_GUILDS] = {"C-Cord HQ", "Lua Lounge", "Pixel Garden"};
static const char *const guild_icons[MOCK_NUM_GUILDS] = {
  "https://cdn.discordapp.com/icons/2001/g0.png",
  "https://cdn.discordapp.com/icons/2002/g1.png",
  "https://cdn.discordapp.com/icons/2003/g2.png",
};
static const char *const words[] = {
  "ship", "build", "coffee", "meeting", "lua", "pixel", "render", "queue", "bug", "patch", "review", "merge",
  "tests", "release", "sprite", "frame", "vector", "cache", "socket", "plugin", "theme", "layout", "scroll",
  "latency", "bench", "docs", "idea", "sketch", "palette", "font", "thread", "window",
};
#define NWORDS (sizeof words / sizeof words[0])

typedef struct channel_spec {
  const char *name;
  bool voice;
} channel_spec;

static const channel_spec text_names[MOCK_NUM_GUILDS][6] = {
  {{"general", false}, {"announcements", false}, {"dev", false}, {"off-topic", false}, {"design-forum", false}},
  {{"general", false}, {"lua-help", false}, {"showcase", false}, {"off-topic", false}, {"random", false}},
  {{"general", false}, {"art", false}, {"critique", false}, {"empty", false}, {"off-topic", false}, {"mod-lounge", false}},
};
static const size_t text_counts[MOCK_NUM_GUILDS] = {5, 5, 6};

static uint64_t splitmix(uint64_t *s)
{
  uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

static uint32_t rnd(mock *m, uint32_t n)
{
  return n ? (uint32_t)(splitmix(&m->rng) % n) : 0;
}

uint64_t mk_now_of(const mock *m)
{
  return m->cfg.now_ms ? m->cfg.now_ms(m->cfg.clock_ud) : ccord_clock_ms();
}

// Frees a message content string the mock owns; public structs hold const char *.
void mk_free_content(const char **p)
{
  union { const char *c; void *v; } u = {.c = *p};
  ccord_free(u.v);
  *p = nullptr;
}

void mk_free_rich(const ccord_message_rich **p)
{
  union { const ccord_message_rich *c; ccord_message_rich *m; } u = {.c = *p};
  ccord_message_rich_free(u.m);
  *p = nullptr;
}

// Keeps an old content string alive until destroy so queued events never point at freed memory.
void mk_retire_content(mock *m, const char *content)
{
  if (!content) return;
  if (m->ngarbage == m->capgarbage) {
    size_t ncap = m->capgarbage ? m->capgarbage * 2 : 16;
    const char **grown = ccord_realloc_array(m->garbage, ncap, sizeof *grown);
    if (!grown) return; // leak is preferable to a dangling queued message
    m->garbage = grown;
    m->capgarbage = ncap;
  }
  m->garbage[m->ngarbage++] = content;
}

// A rich block lives as long as the dataset message that holds it, and past that until destroy for the same reason.
void mk_retire_rich(mock *m, const ccord_message_rich *rich)
{
  if (!rich) return;
  if (m->nrgarbage == m->caprgarbage) {
    size_t ncap = m->caprgarbage ? m->caprgarbage * 2 : 8;
    ccord_message_rich **grown = ccord_realloc_array(m->rgarbage, ncap, sizeof *grown);
    if (!grown) return;   // leak is preferable to a dangling queued message
    m->rgarbage = grown;
    m->caprgarbage = ncap;
  }
  union { const ccord_message_rich *c; ccord_message_rich *m; } u = {.c = rich};
  m->rgarbage[m->nrgarbage++] = u.m;
}

// ---- content generation (deterministic per seed) ----

static void append_words(ccord_buf *b, mock *m, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) {
    if (i) (void)ccord_buf_append_str(b, " ");
    (void)ccord_buf_append_str(b, words[rnd(m, (uint32_t)NWORDS)]);
  }
}

static char *make_content(mock *m, size_t index)
{
  ccord_buf b;
  ccord_buf_init(&b, 0);
  uint32_t r = rnd(m, 100);
  if (r < 40) {
    append_words(&b, m, 3 + rnd(m, 6));
    (void)ccord_buf_append_str(&b, rnd(m, 3) == 0 ? "?" : ".");
  } else if (r < 55) {
    uint32_t lines = 2 + rnd(m, 2);
    for (uint32_t i = 0; i < lines; i++) {
      if (i) (void)ccord_buf_append_str(&b, "\n");
      append_words(&b, m, 3 + rnd(m, 4));
    }
  } else if (r < 65) {
    for (int i = 0; i < 4; i++) (void)ccord_buf_append_str(&b, "supercalifragilistic");
  } else if (r < 75) {
    ccord_buf_appendf(&b, "https://example.com/c/%zu?ref=mock", index);
  } else if (r < 85) {
    append_words(&b, m, 25 + rnd(m, 20));
    (void)ccord_buf_append_str(&b, ".");
  } else if (r < 95) {
    static const char *const shorts[] = {"ok", "lol", "+1", "see #dev", "nice"};
    (void)ccord_buf_append_str(&b, shorts[rnd(m, 5)]);
  } else {
    ccord_buf_appendf(&b, "Reply with the attached file (v%zu).", index);
  }
  size_t n = 0;
  char *out = ccord_buf_take(&b, &n);
  return out;
}

// ---- dataset ----

ccord_status mk_chan_append(mock *m, size_t ci, ccord_message msg, ccord_error *err)
{
  if (m->chn[ci] == m->chcap[ci]) {
    size_t ncap = m->chcap[ci] ? m->chcap[ci] * 2 : 64;
    ccord_message *g = ccord_realloc_array(m->chmsgs[ci], ncap, sizeof *g);
    if (!g) {
      mk_free_content(&msg.content);
      mk_free_rich(&msg.rich);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    m->chmsgs[ci] = g;
    m->chcap[ci] = ncap;
  }
  m->chmsgs[ci][m->chn[ci]++] = msg;
  return CCORD_OK;
}

// The mock's roles. Rank is the member-list rank (0 = top); position is the role hierarchy (higher outranks lower).
typedef struct mock_role_spec {
  const char *name;
  uint32_t color;
  uint8_t rank;
  int32_t position;
  uint64_t permissions;
  bool hoist;
} mock_role_spec;
enum { MOCK_ROLE_ADMINS = 0, MOCK_ROLE_MODERATORS, MOCK_ROLE_MEMBERS };
static const mock_role_spec k_role_specs[3] = {
  {"Admins", 0xF0B232u, 0, 3, CCORD_PERM_ADMINISTRATOR, true},
  {"Moderators", 0x2ECC71u, 1, 2,
   CCORD_PERM_KICK_MEMBERS | CCORD_PERM_BAN_MEMBERS | CCORD_PERM_MANAGE_MESSAGES | CCORD_PERM_MODERATE_MEMBERS |
     CCORD_PERM_MANAGE_NICKNAMES | CCORD_PERM_MENTION_EVERYONE | CCORD_PERM_MANAGE_THREADS | CCORD_PERM_PIN_MESSAGES,
   true},
  {"Members", 0u, 2, 1, 0, false},
};
// What everyone may do before roles and overwrites say otherwise.
#define MOCK_EVERYONE_PERMS                                                                                          \
  (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_SEND_MESSAGES | CCORD_PERM_READ_MESSAGE_HISTORY | CCORD_PERM_ADD_REACTIONS | \
   CCORD_PERM_EMBED_LINKS | CCORD_PERM_ATTACH_FILES | CCORD_PERM_USE_EXTERNAL_EMOJIS | CCORD_PERM_CONNECT |           \
   CCORD_PERM_SPEAK | CCORD_PERM_USE_VAD | CCORD_PERM_CHANGE_NICKNAME | CCORD_PERM_CREATE_PUBLIC_THREADS |           \
   CCORD_PERM_SEND_MESSAGES_IN_THREADS | CCORD_PERM_CREATE_INSTANT_INVITE | CCORD_PERM_STREAM |                    \
   CCORD_PERM_USE_APPLICATION_COMMANDS | CCORD_PERM_USE_EMBEDDED_ACTIVITIES)

static ccord_id mock_role_id(uint32_t guild_index, int kind)
{
  return MOCK_ROLE_BASE + guild_index * 10u + 1u + (ccord_id)kind;
}

// Who holds which role: in C-Cord HQ you are an admin; in Lua Lounge (owned by Ada) a moderator; in Pixel Garden an
// ordinary member. Ada is that server's admin (its owner), Bjorn moderates everywhere else, the rest are members.
static int mock_role_kind(uint32_t g, uint32_t u)
{
  if (u == 0) return g == 0 ? MOCK_ROLE_ADMINS : (g == 1 ? MOCK_ROLE_MODERATORS : MOCK_ROLE_MEMBERS);
  if (u == 1) return g == 1 ? MOCK_ROLE_ADMINS : MOCK_ROLE_MODERATORS;
  if (u == 2) return MOCK_ROLE_MODERATORS;
  return MOCK_ROLE_MEMBERS;
}

// Rebuilds the public view of who holds which role from the rows (kicked members drop out).
void mock_sync_member_roles(mock *m)
{
  size_t n = 0;
  for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
    const mock_mrow *r = &m->mrows[i];
    if (!r->present) continue;
    m->member_roles[n++] = (ccord_member_roles){.guild_id = r->guild_id, .user_id = r->user_id, .role_ids = r->ids,
                                                .count = r->count};
  }
  m->nmember_roles = n;
}

static void build_permissions(mock *m)
{
  size_t nr = 0, nmr = 0;
  m->next_role_id = MOCK_ROLE_BASE + 1000u;
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) {
    ccord_id gid = m->guilds[g].id;
    m->roles[nr++] = (ccord_role){.id = gid, .guild_id = gid, .name = "@everyone", .permissions = MOCK_EVERYONE_PERMS};
    for (int k = MOCK_ROLE_MEMBERS; k >= MOCK_ROLE_ADMINS; k--) {
      const mock_role_spec *rs = &k_role_specs[k];
      m->roles[nr++] = (ccord_role){.id = mock_role_id(g, k), .guild_id = gid, .name = rs->name, .color = rs->color,
                                    .position = rs->position, .permissions = rs->permissions, .hoist = rs->hoist,
                                    .mentionable = k == MOCK_ROLE_MODERATORS};
    }
    for (uint32_t u = 0; u < MOCK_NUM_USERS; u++) {
      mock_mrow *r = &m->mrows[nmr++];
      bool hq_roleless = (g == 0u && (u == 8u || u == 9u));
      *r = (mock_mrow){.guild_id = gid, .user_id = m->users[u].id, .count = hq_roleless ? 0u : 1u, .present = true};
      if (!hq_roleless) r->ids[0] = mock_role_id(g, mock_role_kind(g, u));
    }
  }
  m->nroles = nr;
  mock_sync_member_roles(m);
  // Four channels whose overwrites matter: announcements are read-only for @everyone in C-Cord HQ (Moderators may
  // still post and start threads); #showcase in Lua Lounge is the same; #critique in Pixel Garden is read-only for
  // everyone but admins. Read-only here means what it does on Discord: no sending and no new threads. #mod-lounge in
  // Pixel Garden is a private channel: @everyone may not even view it, Moderators may (and Admins always can), so the
  // plain member the user is there never sees it listed.
  static const uint64_t k_read_only = CCORD_PERM_SEND_MESSAGES | CCORD_PERM_CREATE_PUBLIC_THREADS |
                                      CCORD_PERM_SEND_MESSAGES_IN_THREADS;
  size_t no = 0;
  struct { ccord_id channel, guild; uint32_t guild_index; uint64_t everyone_deny, mods_allow, everyone_allow; } spec[5] = {
    {3112, 2001, 0, k_read_only, k_read_only, 0},
    {3213, 2002, 1, k_read_only, k_read_only, 0},
    {3313, 2003, 2, k_read_only, 0, 0},
    {3316, 2003, 2, CCORD_PERM_VIEW_CHANNEL, CCORD_PERM_VIEW_CHANNEL, 0},
    // The HQ stage lets everyone ask to speak (Discord grants REQUEST_TO_SPEAK on stage channels).
    {3123, 2001, 0, 0, 0, CCORD_PERM_REQUEST_TO_SPEAK},
  };
  for (size_t i = 0; i < 5; i++) {
    size_t cnt = 0;
    m->overwrite_pool[no][cnt++] = (ccord_overwrite){.id = spec[i].guild, .kind = CCORD_OVERWRITE_ROLE,
                                                     .allow = spec[i].everyone_allow, .deny = spec[i].everyone_deny};
    if (spec[i].mods_allow) {
      m->overwrite_pool[no][cnt++] = (ccord_overwrite){.id = mock_role_id(spec[i].guild_index, MOCK_ROLE_MODERATORS),
                                                       .kind = CCORD_OVERWRITE_ROLE, .allow = spec[i].mods_allow};
    }
    m->overwrites[no] = (ccord_channel_overwrites){.channel_id = spec[i].channel, .items = m->overwrite_pool[no],
                                                   .count = cnt};
    no++;
  }
  m->noverwrites = no;
}

void mock_audit_push_change(ccord_audit_entry *e, const char *key, const char *old_v, const char *new_v)
{
  if (!e || !key || key[0] == '\0' || e->change_count >= CCORD_AUDIT_MAX_CHANGES) return;
  ccord_audit_change *ch = &e->changes[e->change_count++];
  (void)snprintf(ch->key, sizeof ch->key, "%s", key);
  (void)snprintf(ch->old_val, sizeof ch->old_val, "%s", old_v ? old_v : "");
  (void)snprintf(ch->new_val, sizeof ch->new_val, "%s", new_v ? new_v : "");
}

void mock_audit_fill_changes(ccord_audit_entry *e, const char *detail)
{
  if (!e || !detail || detail[0] == '\0' || e->change_count > 0) return;
  const char *colon = strstr(detail, ": ");
  if (!colon || colon == detail) return;
  char key[48];
  size_t klen = (size_t)(colon - detail);
  if (klen >= sizeof key) klen = sizeof key - 1u;
  memcpy(key, detail, klen);
  key[klen] = '\0';
  const char *rest = colon + 2;
  const char *arrow = strstr(rest, " -> ");
  if (arrow) {
    char old_v[96];
    size_t olen = (size_t)(arrow - rest);
    if (olen >= sizeof old_v) olen = sizeof old_v - 1u;
    memcpy(old_v, rest, olen);
    old_v[olen] = '\0';
    mock_audit_push_change(e, key, old_v, arrow + 4);
  } else {
    mock_audit_push_change(e, key, "", rest);
  }
}

// A little history so the Bans, Invites and Audit Log pages of C-Cord HQ are not empty on first open.
static void mock_seed_admin(mock *m)
{
  int64_t now = (int64_t)mk_now_of(m);
  const int64_t day = 86400000;
  ccord_id hq = m->guilds[0].id;
  m->bans[m->nbans++] = (mock_ban){hq, {.user_id = 7001, .name = "spam_account", .reason = "Posting scam links"}};
  m->bans[m->nbans++] = (mock_ban){hq, {.user_id = 7002, .name = "troll_99", .reason = ""}};
  m->invites[m->ninvites++] = (mock_invite){hq, {.code = "cCordHQ", .channel_id = 3111, .channel_name = "general",
                                                  .inviter_id = m->users[1].id, .inviter_name = mk_user_names[1],
                                                  .uses = 14, .created_ms = now - 9 * day}};
  m->invites[m->ninvites++] = (mock_invite){hq, {.code = "weekend", .channel_id = 3111, .channel_name = "general",
                                                  .inviter_id = m->self_id ? m->self_id : m->users[0].id,
                                                  .inviter_name = mk_user_names[0], .uses = 3, .max_uses = 10,
                                                  .max_age_s = 604800, .created_ms = now - 2 * day,
                                                  .expires_ms = now + 5 * day}};
  m->next_invite = 1;
  struct { ccord_id actor; uint16_t action; ccord_id target; const char *tname, *reason, *detail; int64_t ago; } seed[] = {
    {m->users[1].id, 31, mock_role_id(0, MOCK_ROLE_MODERATORS), "Moderators", "", "color: 0 -> 3066993", 3 * day},
    {m->users[2].id, 20, 7003, "raid_bot", "Spam in #general", "", 2 * day},
    {m->users[1].id, 10, 3115, "design-forum", "", "name: design-forum", day},
  };
  for (size_t i = 0; i < sizeof seed / sizeof seed[0]; i++) {
    m->last_audit_id = (((ccord_id)(now - seed[i].ago) - MOCK_DISCORD_EPOCH_MS) << 22) | (ccord_id)(i + 1);
    ccord_id actor = seed[i].actor;
    m->audit[m->naudit++] = (mock_audit){hq, {.id = m->last_audit_id, .time_ms = now - seed[i].ago, .user_id = actor,
                                              .user_name = actor == m->users[1].id ? mk_user_names[1] : mk_user_names[2],
                                              .target_id = seed[i].target, .target_name = seed[i].tname,
                                              .action = seed[i].action, .reason = seed[i].reason,
                                              .detail = seed[i].detail}};
    mock_audit_fill_changes(&m->audit[m->naudit - 1].e, seed[i].detail);
  }
  static const char *const k_seed_kw[] = {"free nitro", "*discord-gift*"};
  static const char *const k_seed_allow[] = {"discord.gg"};
  m->automod[m->nautomod++] = (ccord_automod_rule){
    .id = 91001u,
    .guild_id = hq,
    .creator_id = m->users[1].id,
    .name = "Block Scam Links",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_KEYWORD,
    .enabled = true,
    .keywords = k_seed_kw,
    .keyword_count = 2u,
    .allow_list = k_seed_allow,
    .allow_count = 1u,
    .action_block = true,
    .custom_message = "Scam links are blocked in C-Cord HQ.",
    .action_alert = true,
    .alert_channel_id = 3113u,
    .exempt_role_count = 1u,
    .exempt_role_ids = {mock_role_id(0, MOCK_ROLE_MODERATORS)},
  };
  m->automod[m->nautomod++] = (ccord_automod_rule){
    .id = 91002u,
    .guild_id = hq,
    .creator_id = m->users[1].id,
    .name = "Anti-Mention Raid",
    .event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND,
    .trigger_type = CCORD_AUTOMOD_TRIGGER_MENTION_SPAM,
    .enabled = true,
    .mention_total_limit = 5u,
    .mention_raid_protection = true,
    .action_block = true,
    .custom_message = "",
    .action_timeout = true,
    .timeout_seconds = 300u,
  };
  m->webhooks[m->nwebhooks++] = (ccord_webhook){
    .id = 92501u,
    .type = CCORD_WEBHOOK_INCOMING,
    .guild_id = hq,
    .channel_id = 3113u,
    .creator_id = m->users[1].id,
    .creator_name = mk_user_names[1],
    .name = "CI Build Hook",
    .avatar = "",
    .token = "mock_wh_tok_92501",
    .source_guild_name = "",
    .source_channel_name = "",
  };
  m->app_perms[m->napp_perms++] = (ccord_app_cmd_perms){
    .id = m->users[10].id,
    .application_id = m->users[10].id,
    .guild_id = hq,
    .rule_count = 2u,
    .rules = {
      {.id = mock_role_id(0, MOCK_ROLE_MODERATORS), .type = CCORD_APP_PERM_ROLE, .permission = true},
      {.id = hq - 1u, .type = CCORD_APP_PERM_CHANNEL, .permission = true},
    },
  };
  m->app_perms[m->napp_perms++] = (ccord_app_cmd_perms){
    .id = 88001u,
    .application_id = m->users[11].id,
    .guild_id = hq,
    .rule_count = 2u,
    .rules = {
      {.id = 3114u, .type = CCORD_APP_PERM_CHANNEL, .permission = true},
      {.id = m->users[5].id, .type = CCORD_APP_PERM_USER, .permission = false},
    },
  };
  m->welcome[0].guild_id = hq;
  m->welcome[0].present = true;
  m->welcome[0].chs[0] = (ccord_welcome_channel){
    .channel_id = 3111u, .description = "Introduce yourself and chat", .emoji_name = "#"
  };
  m->welcome[0].chs[1] = (ccord_welcome_channel){
    .channel_id = 3113u, .description = "Compiler, protocol & UI patches", .emoji_name = "*"
  };
  m->welcome[0].ws = (ccord_welcome_screen){
    .enabled = true,
    .description = "Welcome to C-Cord HQ — native C23 Discord client development.",
    .channels = m->welcome[0].chs,
    .channel_count = 2u,
  };
  static const ccord_onboarding_option k_seed_ob_opts[] = {
    {.id = 95101u, .title = "Protocol & Net", .description = "Gateway, REST governor & voice", .emoji_name = "*",
     .channel_count = 1u, .channel_ids = {3113u}},
    {.id = 95102u, .title = "Design & UI", .description = "Clay layout, themes & widgets", .emoji_name = "#",
     .channel_count = 1u, .channel_ids = {3115u}},
  };
  static const ccord_onboarding_prompt k_seed_ob_prompts[] = {
    {.id = 95001u, .type = CCORD_ONBOARDING_MULTIPLE_CHOICE, .title = "What areas interest you?",
     .single_select = false, .required = true, .in_onboarding = true,
     .options = k_seed_ob_opts, .option_count = 2u},
  };
  m->onboarding[0].guild_id = hq;
  m->onboarding[0].present = true;
  m->onboarding[0].ob = (ccord_onboarding){
    .guild_id = hq,
    .enabled = true,
    .mode = CCORD_ONBOARDING_DEFAULT,
    .default_channel_count = 2u,
    .default_channel_ids = {3111u, 3112u},
    .prompts = k_seed_ob_prompts,
    .prompt_count = 1u,
  };
}

ccord_status mk_build_dataset(mock *m, ccord_error *err)
{
  // users
  static const char *const user_bios[MOCK_NUM_USERS] = {
    "Building native C23 Discord experiences.",
    "Systems programmer & protocol tinkerer.",
    "Voice engine maintainer.",
    "UI/UX & theme designer.",
    "Shader & rendering enthusiast.",
    "Audio DSP & Opus hacker.",
    "Security & transport hygiene.",
    "Linux desktop integration.",
    "Plugin runtime author.",
    "Benchmarking & profiling.",
    "Automated Lua helper bot.",
    "Pixel art & sprite bot.",
  };
  static const char *const user_pronouns[MOCK_NUM_USERS] = {
    "they/them", "she/her", "he/him", "he/they", "she/they", "he/him",
    "she/her", "they/them", "she/her", "he/him", "bot", "bot",
  };
  static const char *const user_handles[MOCK_NUM_USERS] = {
    "you", "ada", "bjorn", "chen", "dara", "eli",
    "fatima", "gus", "hana", "ivo", "lua_bot", "pixel_bot",
  };
  static const char *const user_banners[MOCK_NUM_USERS] = {
    "https://cdn.discordapp.com/banners/1001/b0.png", "https://cdn.discordapp.com/banners/1002/b1.png",
    "https://cdn.discordapp.com/banners/1003/b2.png", "https://cdn.discordapp.com/banners/1004/b3.png",
    "https://cdn.discordapp.com/banners/1005/b4.png", "https://cdn.discordapp.com/banners/1006/b5.png",
    "https://cdn.discordapp.com/banners/1007/b6.png", "https://cdn.discordapp.com/banners/1008/b7.png",
    "https://cdn.discordapp.com/banners/1009/b8.png", "https://cdn.discordapp.com/banners/1010/b9.png",
    "https://cdn.discordapp.com/banners/1011/b10.png", "https://cdn.discordapp.com/banners/1012/b11.png",
  };
  static const uint32_t user_accents[MOCK_NUM_USERS] = {
    0x5865F2u, 0x57F287u, 0xFEE75Cu, 0xEB459Eu, 0xED4245u, 0x3BA55Cu,
    0xFAA61Au, 0x9B59B6u, 0xE91E63u, 0x1ABC9Cu, 0x3498DBu, 0xE67E22u,
  };
  ccord_arena_init(&m->automod_arena, 4096u);
  m->next_automod_id = MOCK_AUTOMOD_ID_BASE;
  m->next_webhook_id = MOCK_WEBHOOK_ID_BASE;
  m->next_onboarding_id = MOCK_ONBOARDING_ID_BASE;
  for (uint32_t i = 0; i < MOCK_NUM_USERS; i++) {
    bool bot = i >= 10;
    m->users[i] = (ccord_user){
      .id = 1001 + i, .name = mk_user_names[i], .handle = user_handles[i],
      .status_text = (i % 3 == 0) ? "Listening to lo-fi" : "",
      .avatar_url = user_avatars[i],
      .banner_url = user_banners[i],
      .accent_color = user_accents[i],
      .bio = user_bios[i],
      .pronouns = user_pronouns[i],
      .presence = bot ? CCORD_PRESENCE_ONLINE : (ccord_presence)(1 + rnd(m, 3)), .is_bot = bot,
      .avatar_palette = (uint8_t)(i % 6),
    };
    if (i == 1) {
      (void)snprintf(m->users[i].clan_tag, sizeof m->users[i].clan_tag, "CCRD");
      m->users[i].clan_guild_id = m->guilds[0].id;
      (void)snprintf(m->users[i].clan_badge, sizeof m->users[i].clan_badge, "ccrd_badge");
    } else if (i == 2) {
      (void)snprintf(m->users[i].clan_tag, sizeof m->users[i].clan_tag, "LUA");
      m->users[i].clan_guild_id = m->guilds[0].id;
      (void)snprintf(m->users[i].clan_badge, sizeof m->users[i].clan_badge, "ccrd_badge");
    }
    if (i % 3 == 0) {
      m->users[i].has_activity = true;
      m->users[i].activity_type = CCORD_ACTIVITY_LISTENING;
      (void)snprintf(m->users[i].activity_name, sizeof m->users[i].activity_name, "lo-fi");
      (void)snprintf(m->users[i].activity_details, sizeof m->users[i].activity_details, "Beats to code C23 to");
      (void)snprintf(m->users[i].activity_state, sizeof m->users[i].activity_state, "ChilledCow");
      m->users[i].activity_start_ms = m->cfg.base_time_ms - 185000ll;
    }
  }
  m->users[0].presence = CCORD_PRESENCE_ONLINE;
  m->users[8].presence = CCORD_PRESENCE_OFFLINE;
  m->users[9].presence = CCORD_PRESENCE_OFFLINE;
  m->self_id = m->users[0].id;
  m->notes[0] = (ccord_user_note){.user_id = m->users[1].id, .note = "Core maintainer"};
  m->nnotes = 1;
  m->relationships[0] = (ccord_relationship){.user_id = m->users[1].id, .kind = CCORD_RELATIONSHIP_FRIEND};
  m->relationships[1] = (ccord_relationship){.user_id = m->users[2].id, .kind = CCORD_RELATIONSHIP_FRIEND};
  m->relationships[2] = (ccord_relationship){.user_id = m->users[3].id, .kind = CCORD_RELATIONSHIP_PENDING_INCOMING};
  m->relationships[3] = (ccord_relationship){.user_id = m->users[4].id, .kind = CCORD_RELATIONSHIP_PENDING_OUTGOING};
  m->relationships[4] = (ccord_relationship){.user_id = m->users[5].id, .kind = CCORD_RELATIONSHIP_BLOCKED};
  m->nrelationships = 5;
  // M6 account state: Discord default user settings, two devices (one current) and three linked accounts
  // (one revoked), matching
  m->user_settings = (ccord_user_settings){
    .default_guilds_restricted = false,
    .explicit_content_filter = 0,
    .friend_source_all = true,
    .friend_source_mutual_friends = false,
    .friend_source_mutual_guilds = false,
    .allow_access_to_age_restricted_servers = false,
    .developer_mode = false,
    .gif_auto_play = true,
    .animate_emoji = true,
    .enable_tts_command = false,
    .stream_notifications_enabled = false,
    .message_display_compact = false,
    .animate_stickers = 0,
    .locale = "en-US",
  };
  m->auth_sessions[0] = (ccord_auth_session){
    .id_hash = "5f2a9c1d7e3b4a6f8c0d1e2f3a4b5c6d7e8f9012",
    .os = "Linux", .browser = "Firefox 145", .device = "C-Cord Desktop", .location = "Berlin, Germany",
    .approx_last_used_ms = m->cfg.base_time_ms - 60000ll, .current = true,
  };
  m->auth_sessions[1] = (ccord_auth_session){
    .id_hash = "08b1e4a29d6c4f37a5e0b9c8d7f61234567890ab",
    .os = "Windows", .browser = "Chrome 142", .device = "Work Laptop", .location = "Berlin, Germany",
    .approx_last_used_ms = m->cfg.base_time_ms - 86400000ll, .current = false,
  };
  m->nauth_sessions = 2;
  m->connections[0] = (ccord_connection){
    .id = "76561198000000001", .name = "alex", .type = "steam",
    .revoked = false, .verified = true, .friend_sync = true, .show_activity = true,
    .two_way_link = false, .visibility = 1,
  };
  m->connections[1] = (ccord_connection){
    .id = "alex-gh", .name = "alex", .type = "github",
    .revoked = false, .verified = true, .friend_sync = false, .show_activity = true,
    .two_way_link = true, .visibility = 1,
  };
  m->connections[2] = (ccord_connection){
    .id = "2533274900000001", .name = "Alex", .type = "xbox",
    .revoked = true, .verified = false, .friend_sync = false, .show_activity = false,
    .two_way_link = false, .visibility = 0,
  };
  m->nconnections = 3;
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) {
    m->guilds[g] = (ccord_guild){.id = 2001 + g, .name = guild_names[g], .icon_url = guild_icons[g],
                                 .afk_timeout = 300u, .has_guild_settings = true};
  }
  m->nguilds = MOCK_NUM_GUILDS;
  m->guilds[0].system_channel_id = 3111;
  m->guilds[0].verification_level = 1u;
  m->guilds[0].explicit_content_filter = 1u;
  m->guilds[0].premium_tier = 3u;
  m->guilds[0].premium_subscription_count = 14u;
  m->guilds[0].feature_community = true;
  m->guilds[0].rules_channel_id = 3112u;
  m->guilds[0].public_updates_channel_id = 3113u;
  m->guilds[0].feature_vanity_url = true;
  m->guilds[0].feature_banner = true;
  (void)snprintf(m->guilds[0].vanity_url_code, sizeof m->guilds[0].vanity_url_code, "ccord");
  m->guilds[1].owner_id = m->users[1].id;   // "Lua Lounge" belongs to someone else (Ada); the others have no known owner

  // channels (display order: category, its text channels, category, its voice channels, its stage)
  size_t cap = 8; // extra capacity for seeded forum thread channels
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) cap += 2 + text_counts[g] + 3;
  m->channels = ccord_alloc_array(cap, sizeof *m->channels);
  m->chmsgs = ccord_calloc(cap, sizeof *m->chmsgs);
  m->chn = ccord_calloc(cap, sizeof *m->chn);
  m->chcap = ccord_calloc(cap, sizeof *m->chcap);
  m->members = ccord_alloc_array(MOCK_NUM_GUILDS * MOCK_NUM_USERS, sizeof *m->members);
  if (!m->channels || !m->chmsgs || !m->chn || !m->chcap || !m->members) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  size_t n = 0;
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) {
    ccord_id gid = m->guilds[g].id;
    ccord_id cat_text = 3000 + (g + 1) * 100 + 10;
    ccord_id cat_voice = 3000 + (g + 1) * 100 + 20;
    m->channels[n++] = (ccord_channel){.id = cat_text, .guild_id = gid, .parent_id = 0,
                                       .kind = CCORD_CHANNEL_CATEGORY, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_CATEGORY,
                                       .name = "TEXT CHANNELS", .topic = ""};
    for (size_t j = 0; j < text_counts[g]; j++) {
      bool is_forum = (g == 0 && j == 4);
      bool is_announcement = (g == 0 && j == 1);
      uint32_t slowmode_s = ((g == 0 && j == 3) || (g == 2 && j == 4)) ? 5u : 0u; // #off-topic in C-Cord HQ & Pixel Garden
      m->channels[n++] = (ccord_channel){.id = cat_text + 1 + j, .guild_id = gid, .parent_id = cat_text,
                                         .kind = CCORD_CHANNEL_TEXT,
                                         .discord_type = is_forum ? CCORD_DISCORD_CHANNEL_GUILD_FORUM
                                                                  : (is_announcement ? CCORD_DISCORD_CHANNEL_GUILD_ANNOUNCEMENT : 0),
                                         .name = text_names[g][j].name,
                                         .topic = is_forum ? "UI/UX proposals, architecture RFCs, and bug reports — tag your posts!"
                                                           : ((j == 0) ? "Welcome" : ""),
                                         .rate_limit_per_user = slowmode_s,
                                         .default_auto_archive_duration = 1440u};
    }
    m->channels[n++] = (ccord_channel){.id = cat_voice, .guild_id = gid, .parent_id = 0,
                                       .kind = CCORD_CHANNEL_CATEGORY, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_CATEGORY,
                                       .name = "VOICE CHANNELS", .topic = ""};
    static const char *const voice[] = {"General", "Gaming"};
    for (size_t j = 0; j < 2; j++) {
      m->channels[n++] = (ccord_channel){.id = cat_voice + 1 + j, .guild_id = gid, .parent_id = cat_voice,
                                         .kind = CCORD_CHANNEL_VOICE, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_VOICE,
                                         .name = voice[j], .topic = "", .bitrate = 64000u, .user_limit = 0u};
    }
    m->channels[n++] = (ccord_channel){.id = cat_voice + 3, .guild_id = gid, .parent_id = cat_voice,
                                       .kind = CCORD_CHANNEL_STAGE, .discord_type = CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE,
                                       .name = "Stage", .topic = "", .bitrate = 64000u, .user_limit = 0u};
  }
  size_t snap_nchannels = n;
  m->nchannels = n;

  // members: every user in every guild, each holding one role (see mock_role_kind for who gets which)
  size_t nm = 0;
  int64_t join_ref = (int64_t)mk_now_of(m) >= (int64_t)MOCK_DISCORD_EPOCH_MS ? (int64_t)mk_now_of(m) : m->cfg.base_time_ms;
  for (uint32_t g = 0; g < MOCK_NUM_GUILDS; g++) {
    for (uint32_t u = 0; u < MOCK_NUM_USERS; u++) {
      const mock_role_spec *rs = &k_role_specs[mock_role_kind(g, u)];
      int64_t days_ago = (u == 8u) ? 35ll : ((u == 9u) ? 10ll : (int64_t)(42u - u * 3u - g));
      m->members[nm++] = (ccord_member){.guild_id = m->guilds[g].id, .user_id = m->users[u].id,
                                        .role_name = rs->name, .role_color = rs->color, .role_rank = rs->rank,
                                        .joined_at_ms = join_ref - days_ago * 86400000ll};
    }
  }
  m->nmembers = nm;
  build_permissions(m);
  mock_seed_admin(m);

  // One seeded voice participant (with active stream) so the UI always has a populated voice channel,
  // plus a stage speaker and a suppressed audience member with a pending request-to-speak on the HQ stage.
  m->voices[0] = (ccord_voice_state){.guild_id = m->guilds[0].id, .channel_id = 3121, .user_id = m->users[1].id,
                                     .self_mute = false, .self_deaf = false, .self_video = false,
                                     .self_stream = true};
  m->voices[1] = (ccord_voice_state){.guild_id = m->guilds[0].id, .channel_id = 3123, .user_id = m->users[2].id,
                                     .suppress = false};
  m->voices[2] = (ccord_voice_state){.guild_id = m->guilds[0].id, .channel_id = 3123, .user_id = m->users[3].id,
                                     .suppress = true,
                                     .request_to_speak_timestamp = m->cfg.base_time_ms - 60000ll};
  m->nvoices = 3;

  // A live stage instance per stage channel; they ride STAGE_INSTANCE_UPDATE events after the snapshot
  // (the snapshot itself has no stage rows).
  m->next_stage_id = 96001u;
  m->stages[0] = (ccord_stage_instance){.id = m->next_stage_id++, .channel_id = 3123, .guild_id = 2001,
                                        .topic = "Community Town Hall", .privacy_level = 2,
                                        .guild_scheduled_event_id = MOCK_EVENT_ID_BASE};
  m->stages[1] = (ccord_stage_instance){.id = m->next_stage_id++, .channel_id = 3223, .guild_id = 2002,
                                        .topic = "Lua Lounge Live", .privacy_level = 2};
  m->stages[2] = (ccord_stage_instance){.id = m->next_stage_id++, .channel_id = 3323, .guild_id = 2003,
                                        .topic = "Pixel Garden Stage", .privacy_level = 2};
  m->nstages = 3;

  // Guild scheduled events (REST rows, not snapshot data): one stage event and one external event in C-Cord HQ,
  // one voice event in Lua Lounge. They give EVENT_LIST / RSVP / MODIFY / DELETE something to work on.
  int64_t ev_base = m->cfg.base_time_ms ? m->cfg.base_time_ms : MOCK_DEFAULT_BASE_MS;
  m->next_event_id = MOCK_EVENT_ID_BASE;
  m->events[0] = (ccord_scheduled_event){
    .id = m->next_event_id++, .guild_id = 2001, .channel_id = 3123, .creator_id = m->users[1].id,
    .name = "Community Town Hall", .description = "Monthly community Q&A on the HQ stage.",
    .scheduled_start_ms = ev_base + 86400000ll, .privacy_level = 2,
    .status = CCORD_SCHEDULED_EVENT_SCHEDULED, .entity_type = CCORD_SCHEDULED_EVENT_STAGE_INSTANCE,
    .user_count = 12,
  };
  m->events[1] = (ccord_scheduled_event){
    .id = m->next_event_id++, .guild_id = 2001, .creator_id = m->users[2].id,
    .name = "Pixel Art Meetup", .description = "Bring your latest sprites.", .location = "Online",
    .scheduled_start_ms = ev_base + 2 * 86400000ll, .scheduled_end_ms = ev_base + 2 * 86400000ll + 7200000ll,
    .privacy_level = 2, .status = CCORD_SCHEDULED_EVENT_SCHEDULED, .entity_type = CCORD_SCHEDULED_EVENT_EXTERNAL,
    .user_count = 5,
  };
  m->events[2] = (ccord_scheduled_event){
    .id = m->next_event_id++, .guild_id = 2002, .channel_id = 3221, .creator_id = m->users[1].id,
    .name = "Lua Lounge Listening Party", .scheduled_start_ms = ev_base + 3 * 86400000ll,
    .privacy_level = 2, .status = CCORD_SCHEDULED_EVENT_SCHEDULED, .entity_type = CCORD_SCHEDULED_EVENT_VOICE,
    .user_count = 3,
  };
  m->nevents = 3;

  // messages: text channels only; server 1 #general is the stress channel
  uint64_t ids = MOCK_ID_BASE;
  for (size_t ci = 0; ci < m->nchannels; ci++) {
    const ccord_channel *ch = &m->channels[ci];
    if (ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_forum(ch->discord_type)) continue;
    size_t count;
    if (ch->id == MOCK_GENERAL_ID) {
      count = m->cfg.big_channel_messages;
    } else if (strcmp(ch->name, "empty") == 0) {
      count = 0;
    } else {
      count = 20 + rnd(m, 181);
    }
    ccord_id prev_author = m->users[0].id;
    for (size_t i = 0; i < count; i++) {
      if (rnd(m, 100) >= 35) prev_author = m->users[rnd(m, 10)].id; // bursts keep the previous author
      char *text = make_content(m, i);
      if (!text) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      ccord_message msg = {
        .id = ++ids, .channel_id = ch->id, .author_id = prev_author,
        .timestamp_ms = m->cfg.base_time_ms - (int64_t)(count - 1 - i) * MOCK_MSG_STEP_MS,
        .content = text,
      };
      ccord_status ast = mk_chan_append(m, ci, msg, err);
      if (ast != CCORD_OK) return ast;
    }
  }

  // Seed forum metadata & 5 rich forum posts for #design-forum (id=3115) in C-Cord HQ (guild=2001).
  m->forum_metas[0] = (ccord_forum_meta){
    .forum_id = 3115,
    .default_sort = CCORD_FORUM_SORT_ACTIVITY,
    .tag_count = 5,
    .tags = {
      {.id = 4001, .name = "RFC", .emoji_name = "\xf0\x9f\x93\x90", .moderated = false},
      {.id = 4002, .name = "Bug", .emoji_name = "\xf0\x9f\x90\x9b", .moderated = false},
      {.id = 4003, .name = "UI/UX", .emoji_name = "\xf0\x9f\x8e\xa8", .moderated = false},
      {.id = 4004, .name = "Performance", .emoji_name = "\xe2\x9a\xa1", .moderated = false},
      {.id = 4005, .name = "Resolved", .emoji_name = "\xe2\x9c\x85", .moderated = true},
    },
  };
  m->nforum_metas = 1u;

  m->capforum_posts = 16;
  m->forum_posts = ccord_calloc(m->capforum_posts, sizeof *m->forum_posts);
  if (!m->forum_posts) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  typedef struct seed_forum_post {
    ccord_id thread_id;
    ccord_id owner_id;
    const char *title;
    bool pinned;
    ccord_id tags[2];
    uint8_t tag_count;
    uint32_t reactions;
    const char *emoji;
    bool self_reacted;
    int64_t created_offset_ms;
    int64_t active_offset_ms;
    ccord_id msg_authors[4];
    const char *msgs[4];
    size_t msg_count;
  } seed_forum_post;

  static const seed_forum_post k_seed_posts[5] = {
    {
      .thread_id = 3151, .owner_id = 1002,
      .title = "Welcome & Forum Guidelines \xe2\x80\x94 Read Before Posting",
      .pinned = true, .tags = {4001, 4003}, .tag_count = 2,
      .reactions = 14, .emoji = "\xe2\x9d\xa4\xef\xb8\x8f", .self_reacted = true,
      .created_offset_ms = 7200000ll, .active_offset_ms = 600000ll,
      .msg_authors = {1002, 1003, 1004},
      .msgs = {
        "Welcome to #design-forum! Use tags to categorize RFCs, UI polish ideas, and bug reports.",
        "Pinned! Please include reproduction steps or screenshots when tagging with Bug or UI/UX.",
        "Love the new card layout and tag pills \xe2\x80\x94 super easy to scan active discussions.",
      },
      .msg_count = 3,
    },
    {
      .thread_id = 3152, .owner_id = 1004,
      .title = "RFC: Native C23 Forum Channel View & Tag Filtering",
      .pinned = false, .tags = {4001, 4003}, .tag_count = 2,
      .reactions = 9, .emoji = "\xf0\x9f\x94\xa5", .self_reacted = true,
      .created_offset_ms = 5400000ll, .active_offset_ms = 120000ll,
      .msg_authors = {1004, 1005, 1001, 1002},
      .msgs = {
        "Proposal for rendering Discord guild_forum (type 15) channels with card list, tag filter pills, and inline composer.",
        "Can we support multi-tag AND filtering and instant client-side title search?",
        "Yes! Clicking any post card opens the underlying thread with a header breadcrumb back to the forum.",
        "Ship it! Let's keep unopened forum threads hidden from the channel sidebar to avoid clutter.",
      },
      .msg_count = 4,
    },
    {
      .thread_id = 3153, .owner_id = 1005,
      .title = "Vulkan & SDL_Renderer glyph atlas caching under 4K HiDPI",
      .pinned = false, .tags = {4004, 4003}, .tag_count = 2,
      .reactions = 7, .emoji = "\xe2\x9a\xa1", .self_reacted = false,
      .created_offset_ms = 10800000ll, .active_offset_ms = 300000ll,
      .msg_authors = {1005, 1010, 1005},
      .msgs = {
        "Measured 0.18ms frame times after pre-warming the shelf packer with common emoji and UI icons.",
        "Benchmarks look solid on both X11 and Wayland fractional scaling.",
        "Updated the atlas eviction policy so color Twemoji glyphs never thrash during fast scrolling.",
      },
      .msg_count = 3,
    },
    {
      .thread_id = 3154, .owner_id = 1003,
      .title = "Fixed: Voice gateway UDP IP discovery timeout on dual-stack NAT",
      .pinned = false, .tags = {4002, 4005}, .tag_count = 2,
      .reactions = 11, .emoji = "\xe2\x9c\x85", .self_reacted = true,
      .created_offset_ms = 18000000ll, .active_offset_ms = 900000ll,
      .msg_authors = {1003, 1006, 1003},
      .msgs = {
        "Deferred Opcode 1 SELECT_PROTOCOL until the 74-byte 0x0002 UDP discovery response arrives.",
        "Confirmed this fixes the silent audio drop when switching between voice channels rapidly.",
        "Marked as Resolved \xe2\x80\x94 regression test added to test_live_transport.",
      },
      .msg_count = 3,
    },
    {
      .thread_id = 3155, .owner_id = 1009,
      .title = "Lua 5.5 sandboxed plugin hooks for custom slash commands",
      .pinned = false, .tags = {4001, 0}, .tag_count = 1,
      .reactions = 5, .emoji = "\xe2\x9d\xa4\xef\xb8\x8f", .self_reacted = false,
      .created_offset_ms = 25200000ll, .active_offset_ms = 1800000ll,
      .msg_authors = {1009, 1011},
      .msgs = {
        "Designing a zero-allocation IPC bridge so Lua plugins can register custom slash commands safely.",
        "Beep boop! Ready to test custom Lua slash commands in the sandbox.",
      },
      .msg_count = 2,
    },
  };

  for (size_t p = 0; p < 5; p++) {
    const seed_forum_post *sp = &k_seed_posts[p];
    size_t tci = m->nchannels++;
    m->channels[tci] = (ccord_channel){
      .id = sp->thread_id,
      .guild_id = 2001,
      .parent_id = 3115,
      .kind = CCORD_CHANNEL_TEXT,
      .discord_type = CCORD_DISCORD_CHANNEL_PUBLIC_THREAD,
      .name = sp->title,
      .topic = "",
    };
    ccord_id first_mid = CCORD_ID_NONE;
    ccord_id last_mid = CCORD_ID_NONE;
    for (size_t k = 0; k < sp->msg_count; k++) {
      char *copy = ccord_strndup(sp->msgs[k], strlen(sp->msgs[k]));
      if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      int64_t ts = m->cfg.base_time_ms - sp->created_offset_ms + (int64_t)k * 60000ll;
      if (k + 1 == sp->msg_count) ts = m->cfg.base_time_ms - sp->active_offset_ms;
      ccord_message msg = {
        .id = ++ids,
        .channel_id = sp->thread_id,
        .author_id = sp->msg_authors[k],
        .timestamp_ms = ts,
        .content = copy,
        .reaction_count = (k == 0) ? sp->reactions : 0,
        .self_reacted = (k == 0) ? sp->self_reacted : false,
      };
      ccord_status ast = mk_chan_append(m, tci, msg, err);
      if (ast != CCORD_OK) return ast;
      if (k == 0) first_mid = msg.id;
      last_mid = msg.id;
    }
    ccord_forum_post fp = {
      .thread_id = sp->thread_id,
      .forum_id = 3115,
      .guild_id = 2001,
      .owner_id = sp->owner_id,
      .title = sp->title,
      .applied_tag_count = sp->tag_count,
      .message_count = (uint32_t)(sp->msg_count > 0 ? sp->msg_count - 1 : 0),
      .create_ts_ms = m->cfg.base_time_ms - sp->created_offset_ms,
      .last_activity_ms = m->cfg.base_time_ms - sp->active_offset_ms,
      .last_message_id = last_mid,
      .pinned = sp->pinned,
      .first_message_id = first_mid,
      .first_author_id = sp->owner_id,
      .first_content = sp->msgs[0],
      .thumbnail_url = "",
      .reaction_emoji = sp->emoji,
      .reaction_count = sp->reactions,
    };
    for (uint8_t t = 0; t < sp->tag_count; t++) fp.applied_tags[t] = sp->tags[t];
    m->forum_posts[m->nforum_posts++] = fp;
  }
  m->next_thread_id = 3156;
  m->next_msg_id = ids + 1;

  m->snap = (ccord_snapshot){
    .self_id = m->self_id, .users = m->users, .user_count = MOCK_NUM_USERS,
    .guilds = m->guilds, .guild_count = m->nguilds, .channels = m->channels, .channel_count = snap_nchannels,
    .members = m->members, .member_count = m->nmembers,
    .voice_states = m->voices, .voice_state_count = m->nvoices,
    .relationships = m->relationships, .relationship_count = m->nrelationships,
    .notes = m->notes, .note_count = m->nnotes,
  };
  return CCORD_OK;
}


bool mock_str_eq_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false;
  }
  return *a == '\0' && *b == '\0';
}

// Membership screening: joining this invite materializes a guild whose self member is pending until the canned rules
// form below is accepted (GET /guilds/{g}/member-verification, PUT /guilds/{g}/requests/@me).

ccord_status mock_materialize_guild(mock *m, ccord_id gid_hint, const char *gname, bool self_owner,
                                           bool self_pending, ccord_admin_op admin_op, ccord_id *out_guild_id,
                                           ccord_id *out_channel_id, ccord_error *err)
{
  if (m->nguilds >= MOCK_GUILDS_MAX || m->nroles + 2u > MOCK_ROLES_MAX) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "mock: server limit reached");
  }
  if (m->next_admin_id == 0) m->next_admin_id = 8000;
  ccord_id gid = gid_hint != CCORD_ID_NONE ? gid_hint : m->next_admin_id++;
  while (gid_hint == CCORD_ID_NONE && mock_guild_known(m, gid)) gid = m->next_admin_id++;
  const char *owned_name = mk_admin_owned(m, gname && gname[0] ? gname : "New Server");
  if (!owned_name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");

  ccord_guild g = {
    .id = gid,
    .name = owned_name,
    .icon_url = "",
    .owner_id = self_owner ? m->self_id : 0,
    .self_permissions = self_owner ? UINT64_MAX : MOCK_EVERYONE_PERMS,
    .afk_timeout = 300u,
    .has_guild_settings = true,
  };
  m->guilds[m->nguilds++] = g;
  m->snap.guild_count = m->nguilds;

  ccord_role r_ev = {.id = gid, .guild_id = gid, .name = "@everyone", .permissions = MOCK_EVERYONE_PERMS};
  m->roles[m->nroles++] = r_ev;
  ccord_id admin_role_id = CCORD_ID_NONE;
  if (self_owner) {
    admin_role_id = m->next_role_id++;
    m->roles[m->nroles++] = (ccord_role){
      .id = admin_role_id,
      .guild_id = gid,
      .name = "Admins",
      .color = 0xF0B232u,
      .position = 1,
      .permissions = CCORD_PERM_ADMINISTRATOR,
      .hoist = true,
    };
  }
  for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
    if (!m->mrows[i].present) {
      m->mrows[i] = (mock_mrow){.guild_id = gid, .user_id = m->self_id, .present = true};
      if (admin_role_id != CCORD_ID_NONE) {
        m->mrows[i].ids[0] = admin_role_id;
        m->mrows[i].count = 1;
      }
      break;
    }
  }
  mock_sync_member_roles(m);

  ccord_member *nm = ccord_realloc_array(m->members, m->nmembers + 1u, sizeof *nm);
  if (!nm) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  m->members = nm;
  ccord_member self_m = {
    .guild_id = gid,
    .user_id = m->self_id,
    .role_name = self_owner ? "Admins" : "Members",
    .role_color = self_owner ? 0xF0B232u : 0u,
    .role_rank = self_owner ? 0u : 2u,
    .pending = self_pending,   // membership screening: the guild holds this member until the rules form is accepted
  };
  m->members[m->nmembers++] = self_m;
  m->snap.members = m->members;
  m->snap.member_count = m->nmembers;

  ccord_channel cat = {
    .id = m->next_admin_id++,
    .guild_id = gid,
    .parent_id = 0,
    .kind = CCORD_CHANNEL_CATEGORY,
    .name = "TEXT CHANNELS",
    .topic = "",
    .position = 0,
    .discord_type = 4u,
  };
  size_t cci = mk_admin_add_channel(m, &cat, err);
  if (cci == SIZE_MAX) return CCORD_ERR_NOMEM;
  ccord_channel gen = {
    .id = m->next_admin_id++,
    .guild_id = gid,
    .parent_id = cat.id,
    .kind = CCORD_CHANNEL_TEXT,
    .name = "general",
    .topic = "Welcome!",
    .position = 1,
    .discord_type = 0u,
  };
  size_t gci = mk_admin_add_channel(m, &gen, err);
  if (gci == SIZE_MAX) return CCORD_ERR_NOMEM;

  if (out_guild_id) *out_guild_id = gid;
  if (out_channel_id) *out_channel_id = gen.id;

  mock_item it = mk_make_item(CCORD_NET_GUILD_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
  it.guild = ccord_alloc_array(1, sizeof *it.guild);
  it.chan = ccord_alloc_array(2, sizeof *it.chan);
  it.member = ccord_alloc_array(1, sizeof *it.member);
  size_t rcnt = 0, rfirst = mock_guild_roles(m, gid, &rcnt);
  it.roles_own = ccord_alloc_array(rcnt ? rcnt : 1u, sizeof *it.roles_own);
  if (!it.guild || !it.chan || !it.member || !it.roles_own) {
    mk_item_release(&it);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }
  *it.guild = g;
  it.chan[0] = m->channels[cci];
  it.chan[1] = m->channels[gci];
  *it.member = self_m;
  if (rcnt) memcpy(it.roles_own, &m->roles[rfirst], rcnt * sizeof *it.roles_own);

  it.ev.guild = it.guild;
  it.ev.guild_id = gid;
  it.ev.admin_op = admin_op;
  it.ev.guild_channels = it.chan;
  it.ev.guild_channel_count = 2u;
  it.ev.guild_members = it.member;
  it.ev.guild_member_count = 1u;
  it.ev.users = m->users;
  it.ev.user_count = MOCK_NUM_USERS;
  it.ev.roles = it.roles_own;
  it.ev.role_count = rcnt;
  for (size_t i = 0; i < m->nmember_roles; i++) {
    if (m->member_roles[i].guild_id == gid && m->member_roles[i].user_id == m->self_id) {
      it.ev.member_roles = &m->member_roles[i];
      it.ev.member_role_count = 1u;
      break;
    }
  }
  return mk_push_item(m, &it, err);
}


