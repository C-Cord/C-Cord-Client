// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "server_settings_internal.h"

const char *const ss_k_page_names[SS_PAGE_COUNT] = {
  "overview", "roles", "invites", "bans", "audit", "safety", "danger",
  "emoji", "stickers", "soundboard", "members", "automod", "integrations", "community"
};
const char *const ss_k_page_labels[SS_PAGE_COUNT] = {
  "Overview", "Roles", "Invites", "Bans", "Audit Log", "Safety Setup", "Danger Zone",
  "Emoji", "Stickers", "Soundboard", "Members", "AutoMod", "Integrations", "Community"
};
const ss_page ss_k_sidebar_order[SS_PAGE_COUNT] = {
  SS_OVERVIEW, SS_ROLES, SS_INTEGRATIONS, SS_COMMUNITY,
  SS_EMOJI, SS_STICKERS, SS_SOUNDBOARD,
  SS_SAFETY, SS_AUTOMOD, SS_INVITES, SS_BANS, SS_MEMBERS, SS_AUDIT, SS_DANGER
};
const char *const ss_k_tab_names[TAB_COUNT] = {"display", "perms", "members"};
const char *const ss_k_tab_labels[TAB_COUNT] = {"Display", "Permissions", "Manage Members"};
// What a page needs: Discord shows Invites, Safety Setup, AutoMod, and Community to anyone who can manage the server;
// Integrations is visible to webhook or server managers; Danger Zone is owner-only; Emoji / Stickers / Soundboard are
// visible to members with Create or Manage Expressions; Members is visible to managers and moderators.
static const uint64_t k_page_need[SS_PAGE_COUNT] = {
  CCORD_PERM_MANAGE_GUILD,    // SS_OVERVIEW
  CCORD_PERM_MANAGE_ROLES,    // SS_ROLES
  CCORD_PERM_MANAGE_GUILD,    // SS_INVITES
  CCORD_PERM_BAN_MEMBERS,     // SS_BANS
  CCORD_PERM_VIEW_AUDIT_LOG,  // SS_AUDIT
  CCORD_PERM_MANAGE_GUILD,    // SS_SAFETY
  0,                          // SS_DANGER (checked via guild->owner_id == self_id)
  0,                          // SS_EMOJI (checked via ccord_perm_can_create_expression)
  0,                          // SS_STICKERS (checked via ccord_perm_can_create_expression)
  0,                          // SS_SOUNDBOARD (checked via ccord_perm_can_create_expression)
  0,                          // SS_MEMBERS (checked in ss_page_allowed)
  CCORD_PERM_MANAGE_GUILD,    // SS_AUTOMOD
  0,                          // SS_INTEGRATIONS (checked in ss_page_allowed)
  CCORD_PERM_MANAGE_GUILD     // SS_COMMUNITY
};
// Discord's own role colour presets (bright row, then the dark row).
const uint32_t ss_k_colors[20] = {0x1ABC9C, 0x2ECC71, 0x3498DB, 0x9B59B6, 0xE91E63, 0xF1C40F, 0xE67E22, 0xE74C3C, 0x95A5A6,
                                    0x607D8B, 0x11806A, 0x1F8B4C, 0x206694, 0x71368A, 0xAD1457, 0xC27C0E, 0xA84300, 0x992D22,
                                    0x979C9F, 0x546E7A};
const uint32_t ss_k_inv_ages[7] = {1800, 3600, 21600, 43200, 86400, 604800, 0};
const char *const ss_k_inv_age_labels[7] = {"30 minutes", "1 hour", "6 hours", "12 hours", "1 day", "7 days", "Never"};
const uint32_t ss_k_inv_uses[7] = {0, 1, 5, 10, 25, 50, 100};
const char *const ss_k_inv_use_labels[7] = {"No limit", "1 use", "5 uses", "10 uses", "25 uses", "50 uses", "100 uses"};
const uint32_t ss_k_afk_timeouts[5] = {60, 300, 900, 1800, 3600};
const char *const ss_k_afk_timeout_labels[5] = {"1 minute", "5 minutes", "15 minutes", "30 minutes", "1 hour"};

// ---------------------------------------------------------------- state

static struct ui_server_settings *ss_get(const ccord_ui *ui) { return ui->ss; }

void ss_free_ov_images(struct ui_server_settings *ss)
{
  if (!ss) return;
  ccord_free(ss->ov_icon_uri);
  ss->ov_icon_uri = nullptr;
  ss->ov_icon_mode = SS_IMG_UNCHANGED;
  ccord_free(ss->ov_banner_uri);
  ss->ov_banner_uri = nullptr;
  ss->ov_banner_mode = SS_IMG_UNCHANGED;
}

static void ss_free_expr_uploads(struct ui_server_settings *ss)
{
  if (!ss) return;
  ccord_free(ss->emoji_upload_uri);
  ss->emoji_upload_uri = nullptr;
  ss->emoji_upload_anim = false;
  ss->emoji_upload_file[0] = '\0';
  ccord_free(ss->sticker_upload_bytes);
  ss->sticker_upload_bytes = nullptr;
  ss->sticker_upload_len = 0;
  ss->sticker_upload_mime[0] = '\0';
  ss->sticker_upload_fmt = 0;
  ss->sticker_upload_file[0] = '\0';
  ccord_free(ss->sound_upload_uri);
  ss->sound_upload_uri = nullptr;
  ss->sound_upload_file[0] = '\0';
}

static struct ui_server_settings *ss_ensure(ccord_ui *ui)
{
  if (ui->ss) return ui->ss;
  struct ui_server_settings *ss = ccord_calloc(1, sizeof *ss);
  if (!ss) return nullptr;
  ccord_text_edit_init(&ss->name_edit, SS_TITLE_MAX);
  ccord_text_edit_init(&ss->role_name_edit, SS_TITLE_MAX);
  ccord_text_edit_init(&ss->color_edit, 7);
  ccord_text_edit_init(&ss->ban_search, 64);
  ccord_text_edit_init(&ss->vanity_edit, CCORD_ADMIN_INVITE_CODE_MAX);
  ccord_text_edit_init(&ss->emoji_search, 64);
  ccord_text_edit_init(&ss->emoji_new_name, 32);
  ccord_text_edit_init(&ss->sticker_search, 64);
  ccord_text_edit_init(&ss->sticker_new_name, 30);
  ccord_text_edit_init(&ss->sticker_new_tags, 200);
  ccord_text_edit_init(&ss->sticker_new_desc, 100);
  ccord_text_edit_init(&ss->sound_search, 64);
  ccord_text_edit_init(&ss->sound_new_name, 32);
  ccord_text_edit_init(&ss->sound_new_emoji, 32);
  ccord_text_edit_init(&ss->expr_edit_name, 32);
  ccord_text_edit_init(&ss->expr_edit_tags, 200);
  ccord_text_edit_init(&ss->expr_edit_desc, 100);
  ccord_text_edit_init(&ss->expr_edit_emoji, 32);
  ccord_text_edit_init(&ss->member_search, 64);
  ccord_text_edit_init(&ss->prune_reason, 120);
  ccord_text_edit_init(&ss->am_name, 100);
  ccord_text_edit_init(&ss->am_keywords, SS_AM_KEYWORD_CSV_MAX);
  ccord_text_edit_init(&ss->am_regex, SS_AM_REGEX_CSV_MAX);
  ccord_text_edit_init(&ss->am_allow, SS_AM_ALLOW_CSV_MAX);
  ccord_text_edit_init(&ss->am_custom_msg, CCORD_AUTOMOD_CUSTOM_MSG_MAX_BYTES);
  ccord_text_edit_init(&ss->wh_new_name, CCORD_WEBHOOK_NAME_MAX_BYTES);
  ccord_text_edit_init(&ss->wh_edit_name, CCORD_WEBHOOK_NAME_MAX_BYTES);
  ccord_text_edit_init(&ss->ws_desc, CCORD_WELCOME_DESC_MAX_BYTES);
  ccord_text_edit_init(&ss->ws_add_desc, CCORD_WELCOME_CH_DESC_MAX_BYTES);
  ccord_text_edit_init(&ss->ws_add_emoji, CCORD_EMOJI_MAX_BYTES);
  ccord_text_edit_init(&ss->ob_prompt_title, CCORD_ONBOARDING_TITLE_MAX_BYTES);
  ccord_text_edit_init(&ss->ob_opt_title, CCORD_ONBOARDING_TITLE_MAX_BYTES);
  ccord_text_edit_init(&ss->ob_opt_desc, CCORD_ONBOARDING_DESC_MAX_BYTES);
  ss->sound_new_vol_pct = 100u;
  ss->prune_days = 7u;
  ss->comm_wiz_safety = true;
  ss->hover = -1;
  ui->ss = ss;
  return ss;
}

void ui_server_settings_destroy(ccord_ui *ui)
{
  if (!ui || !ui->ss) return;
  ss_free_ov_images(ui->ss);
  ss_free_expr_uploads(ui->ss);
  ccord_text_edit_free(&ui->ss->name_edit);
  ccord_text_edit_free(&ui->ss->role_name_edit);
  ccord_text_edit_free(&ui->ss->color_edit);
  ccord_text_edit_free(&ui->ss->ban_search);
  ccord_text_edit_free(&ui->ss->vanity_edit);
  ccord_text_edit_free(&ui->ss->emoji_search);
  ccord_text_edit_free(&ui->ss->emoji_new_name);
  ccord_text_edit_free(&ui->ss->sticker_search);
  ccord_text_edit_free(&ui->ss->sticker_new_name);
  ccord_text_edit_free(&ui->ss->sticker_new_tags);
  ccord_text_edit_free(&ui->ss->sticker_new_desc);
  ccord_text_edit_free(&ui->ss->sound_search);
  ccord_text_edit_free(&ui->ss->sound_new_name);
  ccord_text_edit_free(&ui->ss->sound_new_emoji);
  ccord_text_edit_free(&ui->ss->expr_edit_name);
  ccord_text_edit_free(&ui->ss->expr_edit_tags);
  ccord_text_edit_free(&ui->ss->expr_edit_desc);
  ccord_text_edit_free(&ui->ss->expr_edit_emoji);
  ccord_text_edit_free(&ui->ss->member_search);
  ccord_text_edit_free(&ui->ss->prune_reason);
  ccord_text_edit_free(&ui->ss->am_name);
  ccord_text_edit_free(&ui->ss->am_keywords);
  ccord_text_edit_free(&ui->ss->am_regex);
  ccord_text_edit_free(&ui->ss->am_allow);
  ccord_text_edit_free(&ui->ss->am_custom_msg);
  ccord_text_edit_free(&ui->ss->wh_new_name);
  ccord_text_edit_free(&ui->ss->wh_edit_name);
  ccord_text_edit_free(&ui->ss->ws_desc);
  ccord_text_edit_free(&ui->ss->ws_add_desc);
  ccord_text_edit_free(&ui->ss->ws_add_emoji);
  ccord_text_edit_free(&ui->ss->ob_prompt_title);
  ccord_text_edit_free(&ui->ss->ob_opt_title);
  ccord_text_edit_free(&ui->ss->ob_opt_desc);
  ccord_free(ui->ss->mem_cache_idx);
  ccord_free(ui->ss->mem_role_w);
  ccord_free(ui->ss);
  ui->ss = nullptr;
}

bool ui_server_settings_is_open(const ccord_ui *ui)
{
  return ui && ui->ss && ui->ss->open;
}

// Ticks one column's wheel glide (scroll.h) by fdt_ms. Returns true while it still moves.
static bool ss_glide_tick(ccord_ui *ui, ccord_scroller *g, float *offset, float fdt_ms, bool reduced)
{
  if (g->pending == 0.0f) return false;
  ui->dirty = true;
  return ccord_scroller_tick(g, offset, fdt_ms, CCORD_SCROLL_UNBOUNDED, reduced);
}

bool ui_server_settings_scroll_tick(ccord_ui *ui, float fdt_ms, bool reduced)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss) return false;
  if (!ss->open) {   // shut: drop any glide so it does not carry on when reopened
    ccord_scroller_cancel(&ss->scroll_glide);
    ccord_scroller_cancel(&ss->list_glide);
    ccord_scroller_cancel(&ss->modview_glide);
    return false;
  }
  bool a = ss_glide_tick(ui, &ss->scroll_glide, &ss->scroll, fdt_ms, reduced);
  bool b = ss_glide_tick(ui, &ss->list_glide, &ss->list_scroll, fdt_ms, reduced);
  bool c = ss_glide_tick(ui, &ss->modview_glide, &ss->modview_scroll, fdt_ms, reduced);
  return a || b || c;
}

void ss_set_edit(ccord_text_edit *e, const char *s)
{
  ccord_error err;
  ccord_text_edit_clear(e);
  if (s && s[0] != '\0') (void)ccord_text_edit_insert(e, s, strlen(s), &err);
}

ccord_text_edit *ss_edit_for(struct ui_server_settings *ss, ss_field f)
{
  switch (f) {
    case SSF_NAME: return &ss->name_edit;
    case SSF_ROLE_NAME: return &ss->role_name_edit;
    case SSF_ROLE_COLOR: return &ss->color_edit;
    case SSF_BAN_SEARCH: return &ss->ban_search;
    case SSF_VANITY: return &ss->vanity_edit;
    case SSF_EMOJI_SEARCH: return &ss->emoji_search;
    case SSF_EMOJI_NEW_NAME: return &ss->emoji_new_name;
    case SSF_STICKER_SEARCH: return &ss->sticker_search;
    case SSF_STICKER_NEW_NAME: return &ss->sticker_new_name;
    case SSF_STICKER_NEW_TAGS: return &ss->sticker_new_tags;
    case SSF_STICKER_NEW_DESC: return &ss->sticker_new_desc;
    case SSF_SOUND_SEARCH: return &ss->sound_search;
    case SSF_SOUND_NEW_NAME: return &ss->sound_new_name;
    case SSF_SOUND_NEW_EMOJI: return &ss->sound_new_emoji;
    case SSF_EXPR_EDIT_NAME: return &ss->expr_edit_name;
    case SSF_EXPR_EDIT_TAGS: return &ss->expr_edit_tags;
    case SSF_EXPR_EDIT_DESC: return &ss->expr_edit_desc;
    case SSF_EXPR_EDIT_EMOJI: return &ss->expr_edit_emoji;
    case SSF_MEMBER_SEARCH: return &ss->member_search;
    case SSF_PRUNE_REASON: return &ss->prune_reason;
    case SSF_AM_NAME: return &ss->am_name;
    case SSF_AM_KEYWORDS: return &ss->am_keywords;
    case SSF_AM_REGEX: return &ss->am_regex;
    case SSF_AM_ALLOW: return &ss->am_allow;
    case SSF_AM_CUSTOM_MSG: return &ss->am_custom_msg;
    case SSF_WH_NEW_NAME: return &ss->wh_new_name;
    case SSF_WH_EDIT_NAME: return &ss->wh_edit_name;
    case SSF_WS_DESC: return &ss->ws_desc;
    case SSF_WS_ADD_DESC: return &ss->ws_add_desc;
    case SSF_WS_ADD_EMOJI: return &ss->ws_add_emoji;
    case SSF_OB_PROMPT_TITLE: return &ss->ob_prompt_title;
    case SSF_OB_OPT_TITLE: return &ss->ob_opt_title;
    case SSF_OB_OPT_DESC: return &ss->ob_opt_desc;
    case SSF_NONE: break;
  }
  return nullptr;
}

// ---------------------------------------------------------------- permissions and gating

uint64_t ss_self_perms(const ccord_ui *ui, ccord_id guild)
{
  return ccord_store_permissions(ui->d.store, guild, CCORD_ID_NONE, ccord_store_self_id(ui->d.store), ui->now_unix_ms);
}

bool ss_is_owner(const ccord_ui *ui, ccord_id guild)
{
  if (!ui || !ui->d.store) return false;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, guild);
  ccord_id self = ccord_store_self_id(ui->d.store);
  return gd && self != CCORD_ID_NONE && gd->owner_id != CCORD_ID_NONE && gd->owner_id == self;
}

bool ss_page_allowed(const ccord_ui *ui, ccord_id guild, ss_page p)
{
  if (p >= SS_PAGE_COUNT) return false;
  if (p == SS_DANGER) return ss_is_owner(ui, guild);
  if (p == SS_EMOJI || p == SS_STICKERS || p == SS_SOUNDBOARD) {
    return ccord_perm_can_create_expression(ss_self_perms(ui, guild));
  }
  if (p == SS_INTEGRATIONS) {
    uint64_t perms = ss_self_perms(ui, guild);
    return ccord_perm_has(perms, CCORD_PERM_MANAGE_WEBHOOKS) ||
           ccord_perm_has(perms, CCORD_PERM_MANAGE_GUILD);
  }
  if (p == SS_MEMBERS) {
    uint64_t perms = ss_self_perms(ui, guild);
    return ccord_perm_has(perms, CCORD_PERM_MANAGE_GUILD) ||
           ccord_perm_has(perms, CCORD_PERM_MANAGE_ROLES) ||
           ccord_perm_has(perms, CCORD_PERM_KICK_MEMBERS) ||
           ccord_perm_has(perms, CCORD_PERM_BAN_MEMBERS) ||
           ccord_perm_has(perms, CCORD_PERM_MODERATE_MEMBERS);
  }
  return ccord_perm_has(ss_self_perms(ui, guild), k_page_need[p]);
}

bool ui_server_settings_available(const ccord_ui *ui, ccord_id guild)
{
  if (!ui || !ui->d.store || guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID) return false;
  if (!ccord_store_guild(ui->d.store, guild)) return false;
  for (size_t i = 0; i < SS_PAGE_COUNT; i++) {
    if (ss_page_allowed(ui, guild, ss_k_sidebar_order[i])) return true;
  }
  return false;
}

bool ui_server_settings_modview_available(const ccord_ui *ui, ccord_id guild)
{
  if (!ui || !ui->d.store || guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID) return false;
  if (!ccord_store_guild(ui->d.store, guild)) return false;
  return ss_page_allowed(ui, guild, SS_MEMBERS);
}

// Policy as everywhere: only take something away on exact data. Otherwise Discord decides.
ccord_perm_verdict ss_role_verdict(const ccord_ui *ui, ccord_id guild, ccord_id role, bool assign)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, guild, self)) return CCORD_PERM_ALLOWED;
  return ccord_store_can_edit_role(ui->d.store, guild, self, role, assign);
}

bool ss_can_prune_role(const ccord_ui *ui, ccord_id guild, const ccord_role *ro)
{
  if (!ro || ro->id == guild || ro->managed) return false;
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, guild, self)) return true;
  const ccord_guild *g = ccord_store_guild(ui->d.store, guild);
  if (g && g->owner_id == self) return true;
  size_t nroles = 0, nactor = 0;
  const ccord_role *roles = ccord_store_roles(ui->d.store, guild, &nroles);
  const ccord_id *actor_roles = nullptr;
  if (!roles || nroles == 0 || !ccord_store_member_roles(ui->d.store, guild, self, &actor_roles, &nactor)) return true;
  const ccord_role *actor_top = ccord_perm_top_role(guild, roles, nroles, actor_roles, nactor);
  return ccord_perm_outranks(actor_top, ro);
}

ccord_perm_verdict ss_grant_verdict(const ccord_ui *ui, ccord_id guild, uint64_t from, uint64_t to)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, guild, self)) return CCORD_PERM_ALLOWED;
  const ccord_guild *g = ccord_store_guild(ui->d.store, guild);
  return ccord_perm_can_grant(guild, g ? g->owner_id : CCORD_ID_NONE, self, ss_self_perms(ui, guild), from, to);
}

// ---------------------------------------------------------------- working copies

static void role_copy_from(ss_role_copy *c, const ccord_role *role)
{
  (void)snprintf(c->name, sizeof c->name, "%s", role->name ? role->name : "");
  c->color = role->color;
  c->hoist = role->hoist;
  c->mention = role->mentionable;
  c->perms = role->permissions;
}

static bool role_copy_equal(const ss_role_copy *a, const ss_role_copy *b)
{
  return strcmp(a->name, b->name) == 0 && a->color == b->color && a->hoist == b->hoist && a->mention == b->mention &&
         a->perms == b->perms;
}

uint32_t ss_norm_afk_timeout(uint32_t s)
{
  for (size_t i = 0; i < AFK_TIMEOUT_COUNT; i++) {
    if (ss_k_afk_timeouts[i] == s) return s;
  }
  return 300u;
}

static void overview_copy_from(ss_overview_copy *c, const ccord_guild *g)
{
  (void)snprintf(c->name, sizeof c->name, "%s", g->name ? g->name : "");
  c->system_channel_id = g->system_channel_id;
  c->system_channel_flags = g->system_channel_flags & CCORD_SYSCHAN_KNOWN_MASK;
  c->default_notifications = g->default_message_notifications > 1u ? 0u : g->default_message_notifications;
  c->afk_channel_id = g->afk_channel_id;
  c->afk_timeout = ss_norm_afk_timeout(g->afk_timeout);
}

static bool overview_copy_equal(const ss_overview_copy *a, const ss_overview_copy *b)
{
  return strcmp(a->name, b->name) == 0 && a->system_channel_id == b->system_channel_id &&
         a->system_channel_flags == b->system_channel_flags && a->default_notifications == b->default_notifications &&
         a->afk_channel_id == b->afk_channel_id && a->afk_timeout == b->afk_timeout;
}

void ss_overview_work(const struct ui_server_settings *ss, ss_overview_copy *out)
{
  *out = ss->ov_work_cfg;
  (void)snprintf(out->name, sizeof out->name, "%s", ccord_text_edit_cstr(&ss->name_edit));
}

static void safety_copy_from(ss_safety_copy *c, const ccord_guild *g)
{
  c->verification_level = g->verification_level > 4u ? 0u : g->verification_level;
  c->explicit_filter = g->explicit_content_filter > 2u ? 0u : g->explicit_content_filter;
}

static bool safety_copy_equal(const ss_safety_copy *a, const ss_safety_copy *b)
{
  return a->verification_level == b->verification_level && a->explicit_filter == b->explicit_filter;
}

static void welcome_copy_from(ss_welcome_copy *out, const ccord_welcome_screen *ws)
{
  *out = (ss_welcome_copy){0};
  if (!ws) return;
  out->enabled = ws->enabled;
  (void)snprintf(out->description, sizeof out->description, "%s", ws->description ? ws->description : "");
  size_t n = ws->channel_count < CCORD_WELCOME_MAX_CHANNELS ? ws->channel_count : CCORD_WELCOME_MAX_CHANNELS;
  for (size_t i = 0; ws->channels && i < n; i++) {
    ss_welcome_chan_copy *dst = &out->channels[out->channel_count++];
    dst->channel_id = ws->channels[i].channel_id;
    dst->emoji_id = ws->channels[i].emoji_id;
    (void)snprintf(dst->description, sizeof dst->description, "%s",
                   ws->channels[i].description ? ws->channels[i].description : "");
    (void)snprintf(dst->emoji_name, sizeof dst->emoji_name, "%s",
                   ws->channels[i].emoji_name ? ws->channels[i].emoji_name : "");
  }
}

static bool welcome_copy_equal(const ss_welcome_copy *a, const ss_welcome_copy *b)
{
  if (a->enabled != b->enabled || strcmp(a->description, b->description) != 0 ||
      a->channel_count != b->channel_count) {
    return false;
  }
  for (size_t i = 0; i < a->channel_count; i++) {
    if (a->channels[i].channel_id != b->channels[i].channel_id ||
        a->channels[i].emoji_id != b->channels[i].emoji_id ||
        strcmp(a->channels[i].description, b->channels[i].description) != 0 ||
        strcmp(a->channels[i].emoji_name, b->channels[i].emoji_name) != 0) {
      return false;
    }
  }
  return true;
}

static void onboarding_copy_from(ss_onboarding_copy *out, const ccord_onboarding *ob)
{
  *out = (ss_onboarding_copy){0};
  if (!ob) return;
  out->enabled = ob->enabled;
  out->mode = ob->mode > 1u ? 0u : ob->mode;
  size_t nd = ob->default_channel_count < CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS
                ? ob->default_channel_count
                : CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS;
  out->default_channel_count = nd;
  if (nd > 0) memcpy(out->default_channel_ids, ob->default_channel_ids, nd * sizeof(ccord_id));
  size_t np = ob->prompt_count < CCORD_ONBOARDING_MAX_PROMPTS ? ob->prompt_count : CCORD_ONBOARDING_MAX_PROMPTS;
  for (size_t pi = 0; ob->prompts && pi < np; pi++) {
    const ccord_onboarding_prompt *sp = &ob->prompts[pi];
    ss_ob_prompt_copy *dp = &out->prompts[out->prompt_count++];
    dp->id = sp->id;
    dp->type = sp->type > 1u ? 0u : sp->type;
    dp->single_select = sp->single_select;
    dp->required = sp->required;
    dp->in_onboarding = sp->in_onboarding;
    (void)snprintf(dp->title, sizeof dp->title, "%s", sp->title ? sp->title : "");
    size_t no = sp->option_count < CCORD_ONBOARDING_MAX_OPTIONS ? sp->option_count : CCORD_ONBOARDING_MAX_OPTIONS;
    for (size_t oi = 0; sp->options && oi < no; oi++) {
      const ccord_onboarding_option *so = &sp->options[oi];
      ss_ob_opt_copy *dst = &dp->options[dp->option_count++];
      dst->id = so->id;
      dst->emoji_id = so->emoji_id;
      (void)snprintf(dst->title, sizeof dst->title, "%s", so->title ? so->title : "");
      (void)snprintf(dst->description, sizeof dst->description, "%s", so->description ? so->description : "");
      (void)snprintf(dst->emoji_name, sizeof dst->emoji_name, "%s", so->emoji_name ? so->emoji_name : "");
      size_t nc = so->channel_count < CCORD_ONBOARDING_MAX_OPT_CHANNELS ? so->channel_count : CCORD_ONBOARDING_MAX_OPT_CHANNELS;
      dst->channel_count = nc;
      if (nc > 0) memcpy(dst->channel_ids, so->channel_ids, nc * sizeof(ccord_id));
      size_t nr = so->role_count < CCORD_ONBOARDING_MAX_OPT_ROLES ? so->role_count : CCORD_ONBOARDING_MAX_OPT_ROLES;
      dst->role_count = nr;
      if (nr > 0) memcpy(dst->role_ids, so->role_ids, nr * sizeof(ccord_id));
    }
  }
}

static bool onboarding_copy_equal(const ss_onboarding_copy *a, const ss_onboarding_copy *b)
{
  if (a->enabled != b->enabled || a->mode != b->mode ||
      a->default_channel_count != b->default_channel_count || a->prompt_count != b->prompt_count) {
    return false;
  }
  for (size_t i = 0; i < a->default_channel_count; i++) {
    if (a->default_channel_ids[i] != b->default_channel_ids[i]) return false;
  }
  for (size_t pi = 0; pi < a->prompt_count; pi++) {
    const ss_ob_prompt_copy *pa = &a->prompts[pi], *pb = &b->prompts[pi];
    if (pa->id != pb->id || pa->type != pb->type || pa->single_select != pb->single_select ||
        pa->required != pb->required || pa->in_onboarding != pb->in_onboarding ||
        strcmp(pa->title, pb->title) != 0 || pa->option_count != pb->option_count) {
      return false;
    }
    for (size_t oi = 0; oi < pa->option_count; oi++) {
      const ss_ob_opt_copy *oa = &pa->options[oi], *ob = &pb->options[oi];
      if (oa->id != ob->id || oa->emoji_id != ob->emoji_id ||
          strcmp(oa->title, ob->title) != 0 || strcmp(oa->description, ob->description) != 0 ||
          strcmp(oa->emoji_name, ob->emoji_name) != 0 ||
          oa->channel_count != ob->channel_count || oa->role_count != ob->role_count) {
        return false;
      }
      for (size_t ci = 0; ci < oa->channel_count; ci++) {
        if (oa->channel_ids[ci] != ob->channel_ids[ci]) return false;
      }
      for (size_t ri = 0; ri < oa->role_count; ri++) {
        if (oa->role_ids[ri] != ob->role_ids[ri]) return false;
      }
    }
  }
  return true;
}

static void ss_format_color(uint32_t color, char *out, size_t cap)
{
  if (color == 0) {
    out[0] = '\0';
    return;
  }
  (void)snprintf(out, cap, "#%06X", color & 0xFFFFFFu);
}

// What the user has typed into the role-name box, as part of the working copy.
void ss_work_name(const struct ui_server_settings *ss, ss_role_copy *c)
{
  (void)snprintf(c->name, sizeof c->name, "%s", ccord_text_edit_cstr(&ss->role_name_edit));
}

bool ss_role_dirty(const struct ui_server_settings *ss)
{
  if (ss->work_role == CCORD_ID_NONE || ss->work_role != ss->role_sel) return false;
  ss_role_copy w = ss->work;
  ss_work_name(ss, &w);
  return !role_copy_equal(&w, &ss->base);
}

static bool ss_overview_dirty(const struct ui_server_settings *ss)
{
  if (!ss->ov_loaded) return false;
  ss_overview_copy w;
  ss_overview_work(ss, &w);
  return !overview_copy_equal(&w, &ss->ov_base_cfg) || ss->ov_icon_mode != SS_IMG_UNCHANGED ||
         ss->ov_banner_mode != SS_IMG_UNCHANGED;
}

static bool ss_safety_dirty(const struct ui_server_settings *ss)
{
  if (!ss->safety_loaded) return false;
  return !safety_copy_equal(&ss->safety_work, &ss->safety_base);
}

bool ss_dirty(const struct ui_server_settings *ss)
{
  if (ss->page == SS_OVERVIEW) return ss_overview_dirty(ss);
  if (ss->page == SS_ROLES) return ss_role_dirty(ss);
  if (ss->page == SS_SAFETY) return ss_safety_dirty(ss);
  return false;
}

void ss_load_role(struct ui_server_settings *ss, const ccord_role *role)
{
  role_copy_from(&ss->base, role);
  ss->work = ss->base;
  ss->work_role = role->id;
  ss_set_edit(&ss->role_name_edit, ss->base.name);
  char hex[8];
  ss_format_color(ss->work.color, hex, sizeof hex);
  ss_set_edit(&ss->color_edit, hex);
}

// Keeps the working copies in step with the store: a role that vanished is dropped, a new selection is loaded, and a
// change Discord made (our own save landing, or someone else's edit) either replaces a clean copy or becomes the new
// baseline of a dirty one.
void ss_sync(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  const ccord_guild *g = ccord_store_guild(st, ss->guild_id);
  if (!g) {
    ui_server_settings_close(ui);
    return;
  }
  // Overview working copy (name, system/AFK channels, default notifications, icon/banner).
  ss_overview_copy ov_now;
  overview_copy_from(&ov_now, g);
  const char *g_icon = g->icon_url ? g->icon_url : "";
  const char *g_banner = g->banner_url;
  if (!ss->ov_loaded) {
    ss->ov_base_cfg = ov_now;
    ss->ov_work_cfg = ov_now;
    (void)snprintf(ss->ov_base_icon, sizeof ss->ov_base_icon, "%s", g_icon);
    (void)snprintf(ss->ov_base_banner, sizeof ss->ov_base_banner, "%s", g_banner);
    ss_free_ov_images(ss);
    ss_set_edit(&ss->name_edit, ov_now.name);
    ss->ov_loaded = true;
  } else {
    if (strcmp(g_icon, ss->ov_base_icon) != 0) {
      (void)snprintf(ss->ov_base_icon, sizeof ss->ov_base_icon, "%s", g_icon);
      ccord_free(ss->ov_icon_uri);
      ss->ov_icon_uri = nullptr;
      ss->ov_icon_mode = SS_IMG_UNCHANGED;
    }
    if (strcmp(g_banner, ss->ov_base_banner) != 0) {
      (void)snprintf(ss->ov_base_banner, sizeof ss->ov_base_banner, "%s", g_banner);
      ccord_free(ss->ov_banner_uri);
      ss->ov_banner_uri = nullptr;
      ss->ov_banner_mode = SS_IMG_UNCHANGED;
    }
    if (!overview_copy_equal(&ov_now, &ss->ov_base_cfg)) {
      if (!ss_overview_dirty(ss)) {
        ss->ov_base_cfg = ov_now;
        ss->ov_work_cfg = ov_now;
        ss_set_edit(&ss->name_edit, ov_now.name);
      } else {
        if (strcmp(ccord_text_edit_cstr(&ss->name_edit), ss->ov_base_cfg.name) == 0) {
          ss_set_edit(&ss->name_edit, ov_now.name);
        }
        if (ss->ov_work_cfg.system_channel_id == ss->ov_base_cfg.system_channel_id) {
          ss->ov_work_cfg.system_channel_id = ov_now.system_channel_id;
        }
        uint32_t touched = ss->ov_work_cfg.system_channel_flags ^ ss->ov_base_cfg.system_channel_flags;
        ss->ov_work_cfg.system_channel_flags =
          (ov_now.system_channel_flags & ~touched) | (ss->ov_work_cfg.system_channel_flags & touched);
        if (ss->ov_work_cfg.default_notifications == ss->ov_base_cfg.default_notifications) {
          ss->ov_work_cfg.default_notifications = ov_now.default_notifications;
        }
        if (ss->ov_work_cfg.afk_channel_id == ss->ov_base_cfg.afk_channel_id) {
          ss->ov_work_cfg.afk_channel_id = ov_now.afk_channel_id;
        }
        if (ss->ov_work_cfg.afk_timeout == ss->ov_base_cfg.afk_timeout) {
          ss->ov_work_cfg.afk_timeout = ov_now.afk_timeout;
        }
        ss->ov_base_cfg = ov_now;
      }
    }
  }

  // Safety Setup working copy (verification level + explicit media content filter).
  ss_safety_copy sf_now;
  safety_copy_from(&sf_now, g);
  if (!ss->safety_loaded) {
    ss->safety_base = sf_now;
    ss->safety_work = sf_now;
    ss->safety_loaded = true;
  } else if (!safety_copy_equal(&sf_now, &ss->safety_base)) {
    if (!ss_safety_dirty(ss)) {
      ss->safety_base = sf_now;
      ss->safety_work = sf_now;
    } else {
      if (ss->safety_work.verification_level == ss->safety_base.verification_level) {
        ss->safety_work.verification_level = sf_now.verification_level;
      }
      if (ss->safety_work.explicit_filter == ss->safety_base.explicit_filter) {
        ss->safety_work.explicit_filter = sf_now.explicit_filter;
      }
      ss->safety_base = sf_now;
    }
  }

  // Vanity URL box follows the store unless the user is typing in it.
  if (strcmp(g->vanity_url_code, ss->vanity_base) != 0) {
    (void)snprintf(ss->vanity_base, sizeof ss->vanity_base, "%s", g->vanity_url_code);
    if (ss->focus != SSF_VANITY) ss_set_edit(&ss->vanity_edit, g->vanity_url_code);
  }

  // Community channels, Welcome Screen, and Server Onboarding working copies.
  if (!ss->comm_loaded) {
    ss->comm_base_rules = ss->comm_work_rules = g->rules_channel_id;
    ss->comm_base_updates = ss->comm_work_updates = g->public_updates_channel_id;
    ss->comm_loaded = true;
  } else if (g->rules_channel_id != ss->comm_base_rules || g->public_updates_channel_id != ss->comm_base_updates) {
    if (ss->comm_work_rules == ss->comm_base_rules) ss->comm_work_rules = g->rules_channel_id;
    if (ss->comm_work_updates == ss->comm_base_updates) ss->comm_work_updates = g->public_updates_channel_id;
    ss->comm_base_rules = g->rules_channel_id;
    ss->comm_base_updates = g->public_updates_channel_id;
  }

  if (ccord_store_welcome_screen_loaded(st, ss->guild_id)) {
    ss_welcome_copy ws_now;
    welcome_copy_from(&ws_now, ccord_store_welcome_screen(st, ss->guild_id));
    if (!ss->ws_loaded || !welcome_copy_equal(&ws_now, &ss->ws_base)) {
      ss->ws_base = ws_now;
      ss->ws_work = ws_now;
      ss_set_edit(&ss->ws_desc, ws_now.description);
      ss->ws_loaded = true;
    }
  } else {
    ss->ws_loaded = false;
  }

  if (ccord_store_onboarding_loaded(st, ss->guild_id)) {
    ss_onboarding_copy ob_now;
    onboarding_copy_from(&ob_now, ccord_store_onboarding(st, ss->guild_id));
    if (!ss->ob_loaded || !onboarding_copy_equal(&ob_now, &ss->ob_base)) {
      ss->ob_base = ob_now;
      ss->ob_work = ob_now;
      ss->ob_editing_prompt = false;
      ss->ob_loaded = true;
    }
  } else {
    ss->ob_loaded = false;
  }

  if (ss->wh_edit_id != CCORD_ID_NONE && ccord_store_webhooks_loaded(st, ss->guild_id)) {
    size_t nw = 0;
    const ccord_webhook *whs = ccord_store_webhooks(st, ss->guild_id, &nw);
    bool found_wh = false;
    for (size_t i = 0; whs && i < nw; i++) {
      if (whs[i].id == ss->wh_edit_id) { found_wh = true; break; }
    }
    if (!found_wh) ss->wh_edit_id = CCORD_ID_NONE;
  }

  // A role we asked Discord to create shows up as the newest one: select it.
  if (ss->await_role) {
    size_t n = 0;
    const ccord_role *roles = ccord_store_roles(st, ss->guild_id, &n);
    for (size_t i = 0; roles && i < n; i++) {
      if (roles[i].id > ss->await_above) {
        ss->role_sel = roles[i].id;
        ss->tab = TAB_DISPLAY;
        ss->await_role = false;
        break;
      }
    }
    if (ss->await_role && ui->now_ms >= ss->await_until_ms) ss->await_role = false;
  }
  const ccord_role *role = ss->role_sel != CCORD_ID_NONE ? ccord_store_role(st, ss->guild_id, ss->role_sel) : nullptr;
  if (!role) {   // deleted (or never chosen): fall back to @everyone, which always exists
    ss->role_sel = ss->guild_id;
    role = ccord_store_role(st, ss->guild_id, ss->role_sel);
    ss->work_role = CCORD_ID_NONE;
    ss->scroll = 0.0f;
    ccord_scroller_cancel(&ss->scroll_glide);
  }
  if (!role) return;
  if (ss->work_role != role->id) {
    ss_load_role(ss, role);
    return;
  }
  ss_role_copy now;
  role_copy_from(&now, role);
  if (!role_copy_equal(&now, &ss->base)) {
    bool clean = !ss_role_dirty(ss);
    if (clean) {
      ss_load_role(ss, role);
    } else {   // keep what the user changed; whatever they have not touched follows Discord, so Save never reverts it
      if (ss->work.color == ss->base.color) ss->work.color = now.color;
      if (ss->work.hoist == ss->base.hoist) ss->work.hoist = now.hoist;
      if (ss->work.mention == ss->base.mention) ss->work.mention = now.mention;
      uint64_t touched = ss->work.perms ^ ss->base.perms;   // the switches the user flipped keep their value
      ss->work.perms = (now.perms & ~touched) | (ss->work.perms & touched);
      if (strcmp(ccord_text_edit_cstr(&ss->role_name_edit), ss->base.name) == 0) ss_set_edit(&ss->role_name_edit, now.name);
      ss->base = now;
    }
  }
  if (ss->focus != SSF_ROLE_COLOR) {
    char hex[8];
    ss_format_color(ss->work.color, hex, sizeof hex);
    if (strcmp(hex, ccord_text_edit_cstr(&ss->color_edit)) != 0) ss_set_edit(&ss->color_edit, hex);
  }
  if (ss->member_role_filter != CCORD_ID_NONE && !ccord_store_role(st, ss->guild_id, ss->member_role_filter)) {
    ss->member_role_filter = CCORD_ID_NONE;
  }
  if (ss->modview_uid != CCORD_ID_NONE && !ccord_store_find_member(st, ss->guild_id, ss->modview_uid)) {
    ss->modview_uid = CCORD_ID_NONE;
  }
  size_t wri = 0;
  for (size_t i = 0; i < ss->prune_role_count; i++) {
    if (ccord_store_role(st, ss->guild_id, ss->prune_roles[i])) ss->prune_roles[wri++] = ss->prune_roles[i];
  }
  ss->prune_role_count = wri;
  if (ss->am_editing && ss->am_edit_id != CCORD_ID_NONE && ccord_store_automod_loaded(st, ss->guild_id)) {
    size_t nr = 0;
    const ccord_automod_rule *rules = ccord_store_automod_rules(st, ss->guild_id, &nr);
    bool exists = false;
    for (size_t i = 0; rules && i < nr; i++) {
      if (rules[i].id == ss->am_edit_id) { exists = true; break; }
    }
    if (!exists) {
      ss->am_editing = false;
      ss->am_edit_id = CCORD_ID_NONE;
    }
  }
}

// ---------------------------------------------------------------- requests

void ss_request(ccord_ui *ui, ss_page p, bool force)
{
  struct ui_server_settings *ss = ui->ss;
  if (p != SS_INVITES && p != SS_BANS && p != SS_AUDIT &&
      p != SS_EMOJI && p != SS_STICKERS && p != SS_SOUNDBOARD && p != SS_AUTOMOD &&
      p != SS_INTEGRATIONS && p != SS_COMMUNITY) {
    return;
  }
  if (ss->requested[p]) {
    uint64_t age = ui->now_ms >= ss->requested_ms[p] ? ui->now_ms - ss->requested_ms[p] : 0;
    if (force ? age < SS_REFRESH_GAP_MS : age < SS_LIST_FRESH_MS) return;
  }
  ccord_admin_request req;
  ccord_error err;
  if (p == SS_INTEGRATIONS) {
    bool can_wh = ccord_perm_has(ss_self_perms(ui, ss->guild_id), CCORD_PERM_MANAGE_WEBHOOKS);
    if (can_wh) {
      if (ccord_admin_list_webhooks(&req, ss->guild_id, &err) != CCORD_OK) {
        ui_notice(ui, "Couldn't ask for webhooks: %s", err.message);
        return;
      }
      ui_admin_submit(ui, &req);
    }
    if (ccord_admin_list_app_cmd_perms(&req, ss->guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't ask for command permissions: %s", err.message);
      return;
    }
    ss->requested[p] = true;
    ss->failed[p] = false;
    ss->requested_ms[p] = ui->now_ms;
    ui_admin_submit(ui, &req);
    return;
  }
  if (p == SS_COMMUNITY) {
    const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
    if (!gd || !gd->feature_community) return;
    if (ccord_admin_get_welcome_screen(&req, ss->guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't ask for welcome screen: %s", err.message);
      return;
    }
    ui_admin_submit(ui, &req);
    if (ccord_admin_get_onboarding(&req, ss->guild_id, &err) != CCORD_OK) {
      ui_notice(ui, "Couldn't ask for server onboarding: %s", err.message);
      return;
    }
    ss->requested[p] = true;
    ss->failed[p] = false;
    ss->requested_ms[p] = ui->now_ms;
    ui_admin_submit(ui, &req);
    return;
  }
  ccord_status st = CCORD_ERR_INVALID;
  if (p == SS_INVITES) st = ccord_admin_list_invites(&req, ss->guild_id, &err);
  else if (p == SS_BANS) st = ccord_admin_list_bans(&req, ss->guild_id, &err);
  else if (p == SS_AUDIT) st = ccord_admin_list_audit_log(&req, ss->guild_id, ss->audit_user_id, ss->audit_action, &err);
  else if (p == SS_EMOJI) st = ccord_admin_list_emojis(&req, ss->guild_id, &err);
  else if (p == SS_STICKERS) st = ccord_admin_list_stickers(&req, ss->guild_id, &err);
  else if (p == SS_SOUNDBOARD) st = ccord_admin_list_soundboard(&req, ss->guild_id, &err);
  else st = ccord_admin_list_automod_rules(&req, ss->guild_id, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't ask for the list: %s", err.message);
    return;
  }
  ss->requested[p] = true;
  ss->failed[p] = false;
  ss->requested_ms[p] = ui->now_ms;
  if (p == SS_AUDIT) ss->audit_actors_valid = false;
  ui_admin_submit(ui, &req);
}

void ui_server_settings_on_failure(ccord_ui *ui, ccord_admin_op op)
{
  if (!ui_server_settings_is_open(ui)) return;
  struct ui_server_settings *ss = ui->ss;
  if (op == CCORD_ADMIN_LIST_INVITES) ss->failed[SS_INVITES] = true;
  else if (op == CCORD_ADMIN_LIST_BANS) ss->failed[SS_BANS] = true;
  else if (op == CCORD_ADMIN_LIST_AUDIT_LOG) ss->failed[SS_AUDIT] = true;
  else if (op == CCORD_ADMIN_LIST_EMOJIS) ss->failed[SS_EMOJI] = true;
  else if (op == CCORD_ADMIN_LIST_STICKERS) ss->failed[SS_STICKERS] = true;
  else if (op == CCORD_ADMIN_LIST_SOUNDBOARD) ss->failed[SS_SOUNDBOARD] = true;
  else if (op == CCORD_ADMIN_LIST_AUTOMOD_RULES) ss->failed[SS_AUTOMOD] = true;
  else if (op == CCORD_ADMIN_LIST_WEBHOOKS || op == CCORD_ADMIN_LIST_APP_CMD_PERMS) ss->failed[SS_INTEGRATIONS] = true;
  else if (op == CCORD_ADMIN_GET_WELCOME_SCREEN || op == CCORD_ADMIN_GET_ONBOARDING) ss->failed[SS_COMMUNITY] = true;
  ui->dirty = true;
}

// ---------------------------------------------------------------- open / close / navigation

void ss_flash(ccord_ui *ui, const char *why)
{
  ui->ss->flash_until_ms = ui->now_ms + SS_FLASH_MS;
  ui_notice(ui, "%s", why);
  ui->dirty = true;
}

void ss_go_page(ccord_ui *ui, ss_page p)
{
  struct ui_server_settings *ss = ui->ss;
  if (p == ss->page) return;
  if (ss_dirty(ss)) {
    ss_flash(ui, "You have unsaved changes. Save or reset them first.");
    return;
  }
  ss->page = p;
  ss->scroll = 0.0f;
  ccord_scroller_cancel(&ss->scroll_glide);
  ss->focus = SSF_NONE;
  ss->edit_expr_id = CCORD_ID_NONE;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->wh_edit_id = CCORD_ID_NONE;
  ss->wh_del_confirm_id = CCORD_ID_NONE;
  ss->ob_editing_prompt = false;
  ss->prune_confirm = false;
  ss_request(ui, p, false);
  ui->dirty = true;
}

// Offset of the page scroll and how far it can travel (verify-slice seam).
void ui_server_settings_scroll_debug(const ccord_ui *ui, float *scroll, float *room)
{
  const struct ui_server_settings *ss = ui ? ui->ss : nullptr;
  if (scroll) *scroll = ss ? ss->scroll : 0.0f;
  if (room) *room = ss ? fmaxf(0.0f, ss->content_h - ss->view_h) : 0.0f;
}

void ui_server_settings_open(ccord_ui *ui, ccord_id guild)
{
  if (!ui || !ui_server_settings_available(ui, guild)) return;
  if (ui->ss && ui->ss->open && ui->ss->guild_id != guild) ui_server_settings_close(ui);
  struct ui_server_settings *ss = ss_ensure(ui);
  if (!ss) return;
  ss->open = true;
  ss->guild_id = guild;
  ui->mobile_server_settings_detail = false;
  ui->left_drawer_open = false;
  ui->right_drawer_open = false;
  ss->focus = SSF_NONE;
  ss->scroll = ss->list_scroll = 0.0f;
  ccord_scroller_cancel(&ss->scroll_glide);
  ccord_scroller_cancel(&ss->list_glide);
  ss->tab = TAB_DISPLAY;
  ss->role_sel = guild;
  ss->work_role = CCORD_ID_NONE;
  ss->ov_loaded = false;
  ss_free_ov_images(ss);
  ss_free_expr_uploads(ss);
  ss->sound_new_vol_pct = 100u;
  ss->edit_expr_page = SS_OVERVIEW;
  ss->edit_expr_id = CCORD_ID_NONE;
  ss->edit_sound_vol_pct = 100u;
  ss->safety_loaded = false;
  ss->vanity_base[0] = '\0';
  ccord_text_edit_clear(&ss->name_edit);   // never carry another server's name over as a pending edit
  ccord_text_edit_clear(&ss->role_name_edit);
  ccord_text_edit_clear(&ss->color_edit);
  ccord_text_edit_clear(&ss->vanity_edit);
  ccord_text_edit_clear(&ss->emoji_search);
  ccord_text_edit_clear(&ss->emoji_new_name);
  ccord_text_edit_clear(&ss->sticker_search);
  ccord_text_edit_clear(&ss->sticker_new_name);
  ccord_text_edit_clear(&ss->sticker_new_tags);
  ccord_text_edit_clear(&ss->sticker_new_desc);
  ccord_text_edit_clear(&ss->sound_search);
  ccord_text_edit_clear(&ss->sound_new_name);
  ccord_text_edit_clear(&ss->sound_new_emoji);
  ccord_text_edit_clear(&ss->expr_edit_name);
  ccord_text_edit_clear(&ss->expr_edit_tags);
  ccord_text_edit_clear(&ss->expr_edit_desc);
  ccord_text_edit_clear(&ss->expr_edit_emoji);
  ccord_text_edit_clear(&ss->member_search);
  ccord_text_edit_clear(&ss->prune_reason);
  ccord_text_edit_clear(&ss->am_name);
  ccord_text_edit_clear(&ss->am_keywords);
  ccord_text_edit_clear(&ss->am_regex);
  ccord_text_edit_clear(&ss->am_allow);
  ccord_text_edit_clear(&ss->am_custom_msg);
  ccord_text_edit_clear(&ss->wh_new_name);
  ccord_text_edit_clear(&ss->wh_edit_name);
  ccord_text_edit_clear(&ss->ws_desc);
  ccord_text_edit_clear(&ss->ws_add_desc);
  ccord_text_edit_clear(&ss->ws_add_emoji);
  ccord_text_edit_clear(&ss->ob_prompt_title);
  ccord_text_edit_clear(&ss->ob_opt_title);
  ccord_text_edit_clear(&ss->ob_opt_desc);
  const ccord_guild *gd = ccord_store_guild(ui->d.store, guild);
  if (gd && gd->vanity_url_code[0] != '\0') {
    (void)snprintf(ss->vanity_base, sizeof ss->vanity_base, "%s", gd->vanity_url_code);
    ss_set_edit(&ss->vanity_edit, gd->vanity_url_code);
  }
  ss->await_role = false;
  ss->flash_until_ms = 0;
  ss->inv_age_i = 4;      // one day, Discord's default
  ss->inv_uses_i = 0;
  ss->inv_channel = CCORD_ID_NONE;
  ss->audit_filter = CCORD_AUDIT_CAT_ALL;
  ss->audit_user_id = CCORD_ID_NONE;
  ss->audit_action = 0;
  ss->audit_expanded_id = CCORD_ID_NONE;
  ss->audit_actor_count = 0;
  ss->audit_actors_valid = false;
  ss->mem_cache_valid = false;
  ss->mem_role_w_count = 0;
  ss->transfer_user_id = CCORD_ID_NONE;
  ss->member_role_filter = CCORD_ID_NONE;
  ss->member_role_picker_uid = CCORD_ID_NONE;
  ss->modview_uid = CCORD_ID_NONE;
  ss->modview_scroll = 0.0f;
  ss->modview_content_h = 0.0f;
  ccord_scroller_cancel(&ss->modview_glide);
  ss->modview_rect = (ccord_rect){0};
  ss->prune_open = false;
  ss->prune_confirm = false;
  ss->prune_dirty = false;
  ss->prune_days = 7u;
  ss->prune_role_count = 0u;
  ss->am_editing = false;
  ss->am_edit_id = CCORD_ID_NONE;
  ss->am_del_confirm_id = CCORD_ID_NONE;
  ss->wh_new_chan = CCORD_ID_NONE;
  ss->wh_edit_id = CCORD_ID_NONE;
  ss->wh_edit_chan = CCORD_ID_NONE;
  ss->wh_del_confirm_id = CCORD_ID_NONE;
  ss->follow_src_chan = CCORD_ID_NONE;
  ss->follow_dst_chan = CCORD_ID_NONE;
  ss->comm_wiz_rules = CCORD_ID_NONE;
  ss->comm_wiz_updates = CCORD_ID_NONE;
  ss->comm_wiz_safety = true;
  ss->comm_loaded = false;
  ss->ws_loaded = false;
  ss->ws_base = (ss_welcome_copy){0};
  ss->ws_work = (ss_welcome_copy){0};
  ss->ws_add_chan = CCORD_ID_NONE;
  ss->ob_loaded = false;
  ss->ob_base = (ss_onboarding_copy){0};
  ss->ob_work = (ss_onboarding_copy){0};
  ss->ob_editing_prompt = false;
  ss->ob_edit_prompt_idx = 0;
  ss->ob_new_opt_chan_count = 0;
  ss->ob_new_opt_role_count = 0;
  ccord_text_edit_clear(&ss->ban_search);
  for (size_t i = 0; i < SS_PAGE_COUNT; i++) ss->requested[i] = ss->failed[i] = false;
  ss->page = SS_OVERVIEW;
  for (size_t i = 0; i < SS_PAGE_COUNT; i++) {   // the first page the user may see (in sidebar order)
    if (ss_page_allowed(ui, guild, ss_k_sidebar_order[i])) {
      ss->page = ss_k_sidebar_order[i];
      break;
    }
  }
  ui_menu_close(ui);
  ui->member_popover_open = false;
  ss_sync(ui);
  if (!ui_server_settings_is_open(ui)) return;
  ss_request(ui, ui->ss->page, false);
  ui->dirty = true;
}

void ui_server_settings_open_modview(ccord_ui *ui, ccord_id guild, ccord_id user_id)
{
  if (!ui_server_settings_modview_available(ui, guild) || user_id == CCORD_ID_NONE) return;
  ui_server_settings_open(ui, guild);
  if (!ui->ss || !ui->ss->open) return;
  ui->ss->page = SS_MEMBERS;
  ui->ss->modview_uid = user_id;
  ui->ss->modview_scroll = 0.0f;
  ccord_scroller_cancel(&ui->ss->modview_glide);
  ss_request(ui, SS_MEMBERS, false);
  ui->dirty = true;
}

void ui_server_settings_close(ccord_ui *ui)
{
  if (!ui || !ui->ss || !ui->ss->open) return;
  ccord_id gid = ui->ss->guild_id;
  if (ui->d.store) ccord_store_forget_admin_lists(ui->d.store, gid);   // lists are volatile, never kept around
  ui_server_settings_destroy(ui);
  ui->dirty = true;
}

bool ccord_ui_open_server_settings(ccord_ui *ui, ccord_id guild_id)
{
  if (!ui || !ui_server_settings_available(ui, guild_id)) return false;
  ui_server_settings_open(ui, guild_id);
  return ui_server_settings_is_open(ui);
}

// ---------------------------------------------------------------- input

static int ss_hit_at(const struct ui_server_settings *ss, float x, float y)
{
  for (size_t i = ss->nhits; i > 0; i--) {   // later hits are drawn on top
    if (ui_in_rect(x, y, ss->hits[i - 1].r)) return (int)(i - 1);
  }
  return -1;
}

// Wheel notch over the page: the same column choice as ss_scroll_by, but the distance glides (scroll.h).
static void ss_wheel_by(ccord_ui *ui, const ccord_input_event *ev, float dy)
{
  struct ui_server_settings *ss = ui->ss;
  bool direct = ui_wheel_direct(ui, ev);
  if (ui->mobile_layout && !ui->mobile_server_settings_detail) {
    float max_sb = fmaxf(0.0f, ss->sb_content_h - ss->sb_view_h);
    ss->sb_scroll = fminf(fmaxf(ss->sb_scroll + dy, 0.0f), max_sb);
    ui->dirty = true;
    return;
  }
  if (ss->page == SS_ROLES && ss->list_view.w > 0.0f && ev->x < ss->list_view.x + ss->list_view.w + 12.0f) {
    ccord_scroller_scroll(&ss->list_glide, &ss->list_scroll, dy, fmaxf(0.0f, ss->list_content_h - ss->list_view.h), direct);
  } else if (ss->page == SS_MEMBERS && ss->modview_uid != CCORD_ID_NONE &&
             ss->modview_rect.w > 0.0f && ev->x >= ss->modview_rect.x) {
    ccord_scroller_scroll(&ss->modview_glide, &ss->modview_scroll, dy, fmaxf(0.0f, ss->modview_content_h - ss->modview_rect.h),
                          direct);
  } else {
    ccord_scroller_scroll(&ss->scroll_glide, &ss->scroll, dy, fmaxf(0.0f, ss->content_h - ss->view_h), direct);
  }
  ui->dirty = true;
}

// A key step (Up, Down, PageUp, PageDown) cancels the wheel glide of the column it moves, then moves that column from
// the offset at the press, so a glide still running cannot carry the list on past the step (scroll.h).
static void ss_scroll_by(ccord_ui *ui, float x, float dy)
{
  struct ui_server_settings *ss = ui->ss;
  if (ui->mobile_layout && !ui->mobile_server_settings_detail) {
    float max_sb = fmaxf(0.0f, ss->sb_content_h - ss->sb_view_h);
    ss->sb_scroll = fminf(fmaxf(ss->sb_scroll + dy, 0.0f), max_sb);
    ui->dirty = true;
    return;
  }
  if (ss->page == SS_ROLES && ss->list_view.w > 0.0f && x < ss->list_view.x + ss->list_view.w + 12.0f) {
    ccord_scroller_cancel(&ss->list_glide);
    float max = fmaxf(0.0f, ss->list_content_h - ss->list_view.h);
    ss->list_scroll = fminf(fmaxf(ss->list_scroll + dy, 0.0f), max);
  } else if (ss->page == SS_MEMBERS && ss->modview_uid != CCORD_ID_NONE &&
             ss->modview_rect.w > 0.0f && x >= ss->modview_rect.x) {
    ccord_scroller_cancel(&ss->modview_glide);
    float max = fmaxf(0.0f, ss->modview_content_h - ss->modview_rect.h);
    ss->modview_scroll = fminf(fmaxf(ss->modview_scroll + dy, 0.0f), max);
  } else {
    ccord_scroller_cancel(&ss->scroll_glide);
    float max = fmaxf(0.0f, ss->content_h - ss->view_h);
    ss->scroll = fminf(fmaxf(ss->scroll + dy, 0.0f), max);
  }
  ui->dirty = true;
}

static bool ss_handle_field_key(ccord_ui *ui, const ccord_input_event *ev)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_text_edit *ed = ss_edit_for(ss, ss->focus);
  if (!ed) return false;
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0, shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
    case CCORD_KEY_BACKSPACE:
      if (ctrl) ccord_text_edit_backspace_word(ed);
      else ccord_text_edit_backspace(ed);
      break;
    case CCORD_KEY_DELETE:
      if (ctrl) ccord_text_edit_delete_word(ed);
      else ccord_text_edit_delete(ed);
      break;
    case CCORD_KEY_LEFT: ccord_text_edit_move(ed, ctrl ? -3 : -1, shift); break;
    case CCORD_KEY_RIGHT: ccord_text_edit_move(ed, ctrl ? 3 : 1, shift); break;
    case CCORD_KEY_HOME: ccord_text_edit_move(ed, -2, shift); break;
    case CCORD_KEY_END: ccord_text_edit_move(ed, 2, shift); break;
    case CCORD_KEY_A:
      if (!ctrl) return true;
      ccord_text_edit_select_all(ed);
      break;
    case CCORD_KEY_C:
    case CCORD_KEY_X: {
      if (!ctrl) return true;
      char *sel = ccord_text_edit_copy_selection(ed);
      if (sel) {
        if (ui->d.host.clipboard_set) ui->d.host.clipboard_set(ui->d.host.ud, sel);
        ccord_free(sel);
        if (ev->key == CCORD_KEY_X) ccord_text_edit_delete(ed);
      }
      break;
    }
    case CCORD_KEY_V: {
      if (!ctrl) return true;
      char *clip = ui->d.host.clipboard_get ? ui->d.host.clipboard_get(ui->d.host.ud) : nullptr;
      if (clip) {
        for (char *p = clip; *p; p++) {
          if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        }
        ccord_error err;
        ccord_status ins = ccord_text_edit_insert(ed, clip, strlen(clip), &err);
        ccord_free(clip);
        if (ins == CCORD_OK) ss_after_edit(ui);
        else if (ins == CCORD_ERR_RANGE && ss_is_automod_list_field(ss->focus)) ss_automod_note_overflow(ui);
      }
      break;
    }
    case CCORD_KEY_RETURN:
      if (ss->focus == SSF_NAME) ss_save(ui);
      else if (ss->focus == SSF_VANITY) ss_save_vanity_code(ui, ccord_text_edit_cstr(&ss->vanity_edit));
      else if (ss->focus == SSF_EXPR_EDIT_NAME || ss->focus == SSF_EXPR_EDIT_TAGS ||
               ss->focus == SSF_EXPR_EDIT_DESC || ss->focus == SSF_EXPR_EDIT_EMOJI) {
        ss_save_expr_edit(ui);
      } else if (ss->focus == SSF_WH_NEW_NAME) {
        ss_webhook_create_submit(ui);
      } else if (ss->focus == SSF_WH_EDIT_NAME) {
        ss_webhook_edit_save_submit(ui);
      } else if (ss->focus == SSF_WS_ADD_DESC || ss->focus == SSF_WS_ADD_EMOJI) {
        ss_welcome_add_channel(ui);
      }
      ss->focus = SSF_NONE;
      ui->dirty = true;
      return true;
    case CCORD_KEY_ESCAPE:
    case CCORD_KEY_TAB:
      ss->focus = SSF_NONE;
      ui->dirty = true;
      return true;
    default: return true;
  }
  ss_after_edit(ui);
  return true;
}

bool ui_server_settings_handle_back(ccord_ui *ui)
{
  if (!ui_server_settings_is_open(ui)) return false;
  struct ui_server_settings *ss = ui->ss;
  if (ui->touch_mode) ui->touch_text_focused = false;
  if (ss->page == SS_MEMBERS && ss->modview_uid != CCORD_ID_NONE) {
    ss->modview_uid = CCORD_ID_NONE;
    ss->modview_scroll = 0.0f;
    ui->dirty = true;
    return true;
  }
  if (ss->page == SS_ROLES && ss->mobile_role_editing) {
    if (ss_dirty(ss)) {
      ss->flash_until_ms = ui->now_ms + SS_FLASH_MS;
    } else {
      ss->mobile_role_editing = false;
      ss->focus = SSF_NONE;
    }
    ui->dirty = true;
    return true;
  }
  if (ss->page == SS_AUTOMOD && ss->am_editing) {
    ss->am_editing = false;
    ss->focus = SSF_NONE;
    ui->dirty = true;
    return true;
  }
  if (ss->page == SS_COMMUNITY && ss->ob_editing_prompt) {
    ss->ob_editing_prompt = false;
    ss->focus = SSF_NONE;
    ui->dirty = true;
    return true;
  }
  if (ui->mobile_layout && ui->mobile_server_settings_detail) {
    if (ss_dirty(ss)) {
      ss->flash_until_ms = ui->now_ms + SS_FLASH_MS;
    } else {
      ui->mobile_server_settings_detail = false;
      ss->focus = SSF_NONE;
    }
    ui->dirty = true;
    return true;
  }
  ss_try_close(ui);
  ui->dirty = true;
  return true;
}

bool ui_server_settings_handle(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_server_settings_is_open(ui) || !ev) return false;
  struct ui_server_settings *ss = ui->ss;
  switch (ev->type) {
    case CCORD_IN_MOUSE_MOVE: {
      int h = ss_hit_at(ss, ev->x, ev->y);
      bool changed = h != ss->hover;
      ss->hover = h;
      return changed;
    }
    case CCORD_IN_MOUSE_DOWN: {
      if (ev->button != 1) return true;
      ccord_rect br = ss_mobile_back_rect(ui);
      if (br.w > 0.0f && ui_in_rect(ev->x, ev->y, br)) {
        return ui_server_settings_handle_back(ui);
      }
      int h = ss_hit_at(ss, ev->x, ev->y);
      if (h < 0 || ss->hits[h].act != A_FIELD) {
        ss->focus = SSF_NONE;   // a click anywhere else leaves the field
        if (ui->touch_mode) ui->touch_text_focused = false;
      }
      if (h >= 0) {
        ss_hit hit = ss->hits[h];   // activating may rebuild the page
        ss_activate(ui, &hit);
      }
      ui->dirty = true;
      return true;
    }
    case CCORD_IN_WHEEL:
      ss_wheel_by(ui, ev, -ev->wheel * 52.0f);
      return true;
    case CCORD_IN_KEY_DOWN: {
      if (ss->focus != SSF_NONE) return ss_handle_field_key(ui, ev);
      bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
      switch (ev->key) {
        case CCORD_KEY_ESCAPE: ss_try_close(ui); break;
        case CCORD_KEY_S:
          if (ctrl) ss_save(ui);
          break;
        case CCORD_KEY_DOWN: ss_scroll_by(ui, ui->mx, 48.0f); break;
        case CCORD_KEY_UP: ss_scroll_by(ui, ui->mx, -48.0f); break;
        case CCORD_KEY_PAGEDOWN: ss_scroll_by(ui, ui->mx, ss->view_h * 0.85f); break;
        case CCORD_KEY_PAGEUP: ss_scroll_by(ui, ui->mx, -ss->view_h * 0.85f); break;
        default: break;
      }
      ui->dirty = true;
      return true;
    }
    case CCORD_IN_TEXT: {
      ccord_text_edit *ed = ss_edit_for(ss, ss->focus);
      if (!ed || ev->text[0] == '\0') return true;
      ccord_error err;
      ccord_status ins = ccord_text_edit_insert(ed, ev->text, strnlen(ev->text, sizeof ev->text), &err);
      if (ins == CCORD_OK) ss_after_edit(ui);
      else if (ins == CCORD_ERR_RANGE && ss_is_automod_list_field(ss->focus)) ss_automod_note_overflow(ui);
      return true;
    }
    default: return false;
  }
}

ccord_cursor_kind ui_server_settings_cursor(const ccord_ui *ui)
{
  const struct ui_server_settings *ss = ui->ss;
  if (!ss || ss->hover < 0 || (size_t)ss->hover >= ss->nhits) return CCORD_CURSOR_DEFAULT;
  const ss_hit *h = &ss->hits[ss->hover];
  if (h->act == A_FIELD) return CCORD_CURSOR_TEXT;
  return h->why ? CCORD_CURSOR_DEFAULT : CCORD_CURSOR_POINTER;
}

bool ui_server_settings_wants_caret(const ccord_ui *ui)
{
  return ui_server_settings_is_open(ui) && ui->ss->focus != SSF_NONE;
}

// ---------------------------------------------------------------- introspection (tests, screenshots)

bool ccord_ui_server_settings_find(const ccord_ui *ui, const char *name, ccord_rect *out)
{
  const struct ui_server_settings *ss = ui ? ss_get(ui) : nullptr;
  if (!ss || !ss->open || !name) return false;
  for (size_t i = ss->nhits; i > 0; i--) {
    if (strcmp(ss->hits[i - 1].name, name) == 0) {
      if (out) *out = ss->hits[i - 1].r;
      return true;
    }
  }
  return false;
}

void ccord_ui_get_server_settings_debug(const ccord_ui *ui, ccord_ui_server_settings_debug *out)
{
  *out = (ccord_ui_server_settings_debug){0};
  const struct ui_server_settings *ss = ui ? ss_get(ui) : nullptr;
  if (!ss || !ss->open) return;
  out->open = true;
  out->guild_id = ss->guild_id;
  out->page = (int)ss->page;
  out->role_id = ss->role_sel;
  out->role_tab = (int)ss->tab;
  out->focus = (int)ss->focus;
  out->hit_count = ss->nhits;
  out->scroll = ss->scroll;
  out->content_height = ss->content_h;
  out->rows = ss->rows;
  out->unsaved = ss_dirty(ss);
  out->work_permissions = ss->work.perms;
  (void)snprintf(out->role_name, sizeof out->role_name, "%s", ccord_text_edit_cstr(&ss->role_name_edit));
  out->work_color = ss->work.color;
}
