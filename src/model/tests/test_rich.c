// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <stdlib.h>
#include <string.h>

#include <ccord/model/rich.h>

#include "ccord_test.h"

// A fully populated block built from stack and static data, so a clone proves it copied rather than aliased.
typedef struct sample {
  char title[32], desc[64], url[64], fname[32], sname[16], rtext[32], pq[32], pa0[16], pa1[16], tname[16], wave[48];
  ccord_id users[3], roles[2];
  ccord_attachment att[2];
  ccord_embed_field fields[2];
  ccord_embed emb[1];
  ccord_msg_sticker stk[1];
  ccord_embed_field ffields[1];
  ccord_embed fwd_emb[1];
  ccord_attachment fwd_att[1];
  ccord_poll_answer pa[2];
  ccord_message_rich r;
} sample;

static void fill(sample *s)
{
  memset(s, 0, sizeof *s);
  strcpy(s->title, "A title");
  strcpy(s->desc, "A description");
  strcpy(s->url, "https://example.com/x");
  strcpy(s->fname, "SPOILER_cat.png");
  strcpy(s->sname, "wave");
  strcpy(s->rtext, "replied text");
  strcpy(s->pq, "Pizza?");
  strcpy(s->pa0, "Yes");
  strcpy(s->pa1, "No");
  strcpy(s->tname, "side thread");
  strcpy(s->wave, "AAECAwQFBgcICQ==");
  s->users[0] = 11; s->users[1] = 12; s->users[2] = 13;
  s->roles[0] = 21; s->roles[1] = 22;
  s->att[0] = (ccord_attachment){.id = 5, .filename = s->fname, .description = "", .content_type = "image/png", .url = s->url,
                                 .proxy_url = "", .size = 1234, .width = 640, .height = 480, .spoiler = true,
                                 .duration_ms = 3200, .waveform_b64 = s->wave};
  s->att[1] = (ccord_attachment){.id = 6, .filename = "song.mp3", .description = nullptr, .content_type = nullptr, .url = s->url,
                                 .proxy_url = nullptr, .size = 99};
  s->fields[0] = (ccord_embed_field){.name = "f1", .value = "v1", .inline_ = true};
  s->fields[1] = (ccord_embed_field){.name = "f2", .value = "v2"};
  s->emb[0] = (ccord_embed){.kind = CCORD_EMBED_RICH, .title = s->title, .description = s->desc, .url = s->url, .color = 0x336699,
                            .has_color = true, .timestamp_ms = 1700000000000LL, .footer_text = "foot",
                            .image = {.url = s->url, .width = 10, .height = 20}, .fields = s->fields, .field_count = 2};
  s->stk[0] = (ccord_msg_sticker){.id = 9, .name = s->sname, .format = 3};
  s->ffields[0] = (ccord_embed_field){.name = "g", .value = "h"};
  s->fwd_emb[0] = (ccord_embed){.kind = CCORD_EMBED_LINK, .title = "fwd embed", .fields = s->ffields, .field_count = 1};
  s->fwd_att[0] = (ccord_attachment){.id = 7, .filename = "fwd.gif", .url = s->url, .content_type = "image/gif"};
  s->r = (ccord_message_rich){
    .present = CCORD_RICH_ALL, .type = 19, .flags = CCORD_MSGFLAG_HAS_THREAD | CCORD_MSGFLAG_SUPPRESS_EMBEDS,
    .mention_everyone = true,
    .mention_users = s->users, .mention_user_count = 3, .mention_roles = s->roles, .mention_role_count = 2,
    .attachments = s->att, .attachment_count = 2, .embeds = s->emb, .embed_count = 1, .stickers = s->stk, .sticker_count = 1,
    .reference = {.kind = CCORD_REF_REPLY, .message_id = 77, .channel_id = 88, .guild_id = 99},
    .ref_preview = {.present = true, .author_id = 11, .author_name = "Ann", .content = s->rtext, .has_attachments = true},
    .forward = {.present = true, .content = "forwarded", .timestamp_ms = 5, .attachments = s->fwd_att, .attachment_count = 1,
                .embeds = s->fwd_emb, .embed_count = 1},
    .poll = {.present = true, .question = s->pq, .answers = s->pa, .answer_count = 2, .expiry_ms = 42, .multiselect = true},
    .thread = {.id = 31, .name = s->tname, .message_count = 4},
  };
  s->pa[0] = (ccord_poll_answer){.id = 1, .text = s->pa0, .emoji = "x", .votes = 3, .me_voted = true};
  s->pa[1] = (ccord_poll_answer){.id = 2, .text = s->pa1, .votes = 1};
}

static void test_clone_is_deep(void)
{
  ccord_error err;
  sample *s = calloc(1, sizeof *s);
  CT_CHECK(s != nullptr);
  if (!s) return;
  fill(s);
  CT_CHECK(ccord_message_rich_validate(&s->r, &err) == CCORD_OK);
  ccord_message_rich *c = ccord_message_rich_clone(&s->r, &err);
  CT_CHECK(c != nullptr && c != &s->r);
  if (!c) { free(s); return; }
  memset(s->title, 'Z', sizeof s->title - 1);   // wreck everything the original pointed at
  memset(s->desc, 'Z', sizeof s->desc - 1);
  memset(s->url, 'Z', sizeof s->url - 1);
  memset(s->fname, 'Z', sizeof s->fname - 1);
  memset(s->rtext, 'Z', sizeof s->rtext - 1);
  memset(s->pq, 'Z', sizeof s->pq - 1);
  memset(s->tname, 'Z', sizeof s->tname - 1);
  memset(s->wave, 'Z', sizeof s->wave - 1);
  memset(s->users, 0xFF, sizeof s->users);
  memset(s->att, 0, sizeof s->att);
  memset(s->fields, 0, sizeof s->fields);
  memset(s->emb, 0, sizeof s->emb);

  CT_CHECK(c->type == 19 && c->flags == (CCORD_MSGFLAG_HAS_THREAD | CCORD_MSGFLAG_SUPPRESS_EMBEDS) && c->mention_everyone);
  CT_CHECK(c->present == CCORD_RICH_ALL);
  CT_CHECK(c->mention_user_count == 3 && c->mention_users[0] == 11 && c->mention_users[2] == 13);
  CT_CHECK(c->mention_role_count == 2 && c->mention_roles[1] == 22);
  CT_CHECK(c->attachment_count == 2 && strcmp(c->attachments[0].filename, "SPOILER_cat.png") == 0 && c->attachments[0].spoiler);
  CT_CHECK(c->attachments[0].width == 640 && c->attachments[0].size == 1234 && strcmp(c->attachments[0].content_type, "image/png") == 0);
  CT_CHECK(strcmp(c->attachments[0].url, "https://example.com/x") == 0 && c->attachments[0].description[0] == '\0');
  CT_CHECK(c->attachments[1].description != nullptr && c->attachments[1].content_type != nullptr && c->attachments[1].proxy_url != nullptr);   // null became ""
  // A voice-message waveform is deep-copied like every other attachment string (it lives in the block's arena,
  // so ccord_message_rich_free releases it).
  CT_CHECK(c->attachments[0].duration_ms == 3200 && c->attachments[0].waveform_b64 != s->wave &&
           strcmp(c->attachments[0].waveform_b64, "AAECAwQFBgcICQ==") == 0);
  CT_CHECK(c->attachments[1].waveform_b64 != nullptr && c->attachments[1].waveform_b64[0] == '\0');
  CT_CHECK(c->embed_count == 1 && strcmp(c->embeds[0].title, "A title") == 0 && strcmp(c->embeds[0].description, "A description") == 0);
  CT_CHECK(c->embeds[0].has_color && c->embeds[0].color == 0x336699 && c->embeds[0].timestamp_ms == 1700000000000LL);
  CT_CHECK(strcmp(c->embeds[0].image.url, "https://example.com/x") == 0 && c->embeds[0].image.width == 10 && c->embeds[0].thumbnail.url[0] == '\0');
  CT_CHECK(c->embeds[0].field_count == 2 && strcmp(c->embeds[0].fields[0].name, "f1") == 0 && c->embeds[0].fields[0].inline_ &&
           strcmp(c->embeds[0].fields[1].value, "v2") == 0 && !c->embeds[0].fields[1].inline_);
  CT_CHECK(strcmp(c->embeds[0].footer_text, "foot") == 0 && c->embeds[0].author_name[0] == '\0');
  CT_CHECK(c->sticker_count == 1 && strcmp(c->stickers[0].name, "wave") == 0 && c->stickers[0].format == 3 && c->stickers[0].id == 9);
  CT_CHECK(c->reference.kind == CCORD_REF_REPLY && c->reference.message_id == 77 && c->reference.channel_id == 88 && c->reference.guild_id == 99);
  CT_CHECK(c->ref_preview.present && c->ref_preview.author_id == 11 && strcmp(c->ref_preview.author_name, "Ann") == 0 &&
           strcmp(c->ref_preview.content, "replied text") == 0 && c->ref_preview.has_attachments);
  CT_CHECK(c->forward.present && strcmp(c->forward.content, "forwarded") == 0 && c->forward.timestamp_ms == 5);
  CT_CHECK(c->forward.attachment_count == 1 && strcmp(c->forward.attachments[0].filename, "fwd.gif") == 0);
  CT_CHECK(c->forward.embed_count == 1 && strcmp(c->forward.embeds[0].title, "fwd embed") == 0 &&
           c->forward.embeds[0].field_count == 1 && strcmp(c->forward.embeds[0].fields[0].value, "h") == 0);
  CT_CHECK(c->poll.present && strcmp(c->poll.question, "Pizza?") == 0 && c->poll.answer_count == 2 && c->poll.expiry_ms == 42 &&
           c->poll.multiselect && !c->poll.finalized);
  CT_CHECK(strcmp(c->poll.answers[0].text, "Yes") == 0 && strcmp(c->poll.answers[0].emoji, "x") == 0 && c->poll.answers[0].votes == 3 &&
           c->poll.answers[0].me_voted && strcmp(c->poll.answers[1].text, "No") == 0 && c->poll.answers[1].emoji[0] == '\0');
  CT_CHECK(c->thread.id == 31 && strcmp(c->thread.name, "side thread") == 0 && c->thread.message_count == 4);
  CT_CHECK(ccord_message_rich_validate(c, &err) == CCORD_OK);

  // A clone of a clone is as good as the first, and independent of it.
  ccord_message_rich *d = ccord_message_rich_clone(c, &err);
  CT_CHECK(d != nullptr && d->embeds != c->embeds && d->embeds[0].title != c->embeds[0].title);
  CT_CHECK(d && d->attachments[0].waveform_b64 != c->attachments[0].waveform_b64);
  ccord_message_rich_free(c);
  CT_CHECK(d && strcmp(d->embeds[0].title, "A title") == 0 && d->attachment_count == 2);
  CT_CHECK(d && strcmp(d->attachments[0].waveform_b64, "AAECAwQFBgcICQ==") == 0);
  ccord_message_rich_free(d);
  ccord_message_rich_free(nullptr);
  CT_CHECK(ccord_message_rich_clone(nullptr, &err) == nullptr && err.code == CCORD_OK);
  free(s);
}

static void test_merge_keeps_what_the_update_lacks(void)
{
  ccord_error err;
  sample *s = calloc(1, sizeof *s);
  CT_CHECK(s != nullptr);
  if (!s) return;
  fill(s);
  ccord_message_rich *old = ccord_message_rich_clone(&s->r, &err);
  CT_CHECK(old != nullptr);
  if (!old) { free(s); return; }

  // A link preview arriving later: only the embeds are in the update.
  ccord_embed_field nf[1] = {{.name = "n", .value = "v"}};
  ccord_embed ne[2] = {{.kind = CCORD_EMBED_LINK, .title = "first"}, {.kind = CCORD_EMBED_IMAGE, .title = "second", .fields = nf, .field_count = 1}};
  ccord_message_rich upd = {.present = CCORD_RICH_HAS_EMBEDS, .embeds = ne, .embed_count = 2};
  ccord_message_rich *m = ccord_message_rich_merge(old, &upd, &err);
  CT_CHECK(m != nullptr && m != old);
  if (m) {
    CT_CHECK(m->embed_count == 2 && strcmp(m->embeds[0].title, "first") == 0 && strcmp(m->embeds[1].fields[0].name, "n") == 0);
    CT_CHECK(m->attachment_count == 2 && m->sticker_count == 1 && m->mention_user_count == 3 && m->mention_everyone);   // untouched
    CT_CHECK(m->type == 19 && m->flags != 0 && m->poll.present && m->thread.id == 31 && m->reference.message_id == 77);
    CT_CHECK(m->ref_preview.present && m->forward.present && strcmp(m->forward.content, "forwarded") == 0);
    CT_CHECK(m->present == CCORD_RICH_ALL);
    ccord_message_rich_free(m);
  }

  // text_absent belongs to the update only: a merged (stored) block never carries it, whichever path produced it.
  ccord_message_rich flagged = {.present = CCORD_RICH_HAS_EMBEDS, .text_absent = true, .embeds = ne, .embed_count = 2};
  m = ccord_message_rich_merge(old, &flagged, &err);
  CT_CHECK(m && !m->text_absent && m->embed_count == 2);
  ccord_message_rich_free(m);
  m = ccord_message_rich_merge(nullptr, &flagged, &err);
  CT_CHECK(m && !m->text_absent && m->embed_count == 2);
  ccord_message_rich_free(m);
  old->text_absent = true;   // even a block that somehow has it
  m = ccord_message_rich_merge(old, nullptr, &err);
  CT_CHECK(m && !m->text_absent);
  ccord_message_rich_free(m);
  m = ccord_message_rich_merge(old, &upd, &err);
  CT_CHECK(m && !m->text_absent);
  ccord_message_rich_free(m);
  old->text_absent = false;

  // An update that clears the embeds (the author removed the preview) is "present" with nothing in it.
  ccord_message_rich none = {.present = CCORD_RICH_HAS_EMBEDS};
  m = ccord_message_rich_merge(old, &none, &err);
  CT_CHECK(m && m->embed_count == 0 && m->attachment_count == 2);
  ccord_message_rich_free(m);

  // Each part is replaced on its own and nothing else moves.
  ccord_id nu[1] = {55};
  ccord_message_rich men = {.present = CCORD_RICH_HAS_MENTIONS, .mention_users = nu, .mention_user_count = 1};
  m = ccord_message_rich_merge(old, &men, &err);
  CT_CHECK(m && m->mention_user_count == 1 && m->mention_users[0] == 55 && m->mention_role_count == 0 && !m->mention_everyone);
  CT_CHECK(m && m->embed_count == 1 && m->type == 19);
  ccord_message_rich_free(m);
  ccord_message_rich ty = {.present = CCORD_RICH_HAS_TYPE, .type = 0, .flags = CCORD_MSGFLAG_URGENT};
  m = ccord_message_rich_merge(old, &ty, &err);
  CT_CHECK(m && m->type == 0 && m->flags == CCORD_MSGFLAG_URGENT && m->mention_user_count == 3);
  ccord_message_rich_free(m);
  ccord_message_rich at = {.present = CCORD_RICH_HAS_ATTACHMENTS};
  m = ccord_message_rich_merge(old, &at, &err);
  CT_CHECK(m && m->attachment_count == 0 && m->embed_count == 1 && m->sticker_count == 1);
  ccord_message_rich_free(m);
  ccord_message_rich st = {.present = CCORD_RICH_HAS_STICKERS};
  m = ccord_message_rich_merge(old, &st, &err);
  CT_CHECK(m && m->sticker_count == 0 && m->attachment_count == 2);
  ccord_message_rich_free(m);
  ccord_message_rich rf = {.present = CCORD_RICH_HAS_REFERENCE};
  m = ccord_message_rich_merge(old, &rf, &err);
  CT_CHECK(m && m->reference.kind == CCORD_REF_NONE && !m->ref_preview.present && m->forward.present);
  ccord_message_rich_free(m);
  ccord_message_rich fw = {.present = CCORD_RICH_HAS_FORWARD};
  m = ccord_message_rich_merge(old, &fw, &err);
  CT_CHECK(m && !m->forward.present && m->forward.attachment_count == 0 && m->reference.kind == CCORD_REF_REPLY);
  ccord_message_rich_free(m);
  ccord_message_rich pl = {.present = CCORD_RICH_HAS_POLL, .poll = {.present = true, .question = "Q", .answer_count = 0, .finalized = true}};
  m = ccord_message_rich_merge(old, &pl, &err);
  CT_CHECK(m && strcmp(m->poll.question, "Q") == 0 && m->poll.finalized && m->poll.answer_count == 0 && m->thread.id == 31);
  ccord_message_rich_free(m);
  ccord_message_rich th = {.present = CCORD_RICH_HAS_THREAD};
  m = ccord_message_rich_merge(old, &th, &err);
  CT_CHECK(m && m->thread.id == 0 && m->poll.present && m->thread.name[0] == '\0');
  ccord_message_rich_free(m);

  // No old block: the update stands alone. No update: a copy of the old. Neither: nothing.
  m = ccord_message_rich_merge(nullptr, &upd, &err);
  CT_CHECK(m && m->embed_count == 2 && m->attachment_count == 0);
  ccord_message_rich_free(m);
  m = ccord_message_rich_merge(old, nullptr, &err);
  CT_CHECK(m && m != old && m->embed_count == 1 && m->type == 19);
  ccord_message_rich_free(m);
  CT_CHECK(ccord_message_rich_merge(nullptr, nullptr, &err) == nullptr && err.code == CCORD_OK);

  // A block built up from partial updates remembers every part it has been told about.
  ccord_message_rich *acc = ccord_message_rich_merge(nullptr, &upd, &err);
  CT_CHECK(acc && acc->present == CCORD_RICH_HAS_EMBEDS);
  ccord_message_rich a_part = {.present = CCORD_RICH_HAS_ATTACHMENTS};
  ccord_message_rich *acc2 = ccord_message_rich_merge(acc, &a_part, &err);
  CT_CHECK(acc2 && acc2->present == (CCORD_RICH_HAS_EMBEDS | CCORD_RICH_HAS_ATTACHMENTS) && acc2->embed_count == 2);
  ccord_message_rich_free(acc2);
  ccord_message_rich_free(acc);
  ccord_message_rich_free(old);
  free(s);
}

static void test_validation(void)
{
  ccord_error err;
  CT_CHECK(ccord_message_rich_validate(nullptr, &err) == CCORD_OK);
  ccord_message_rich empty = {0};
  CT_CHECK(ccord_message_rich_validate(&empty, &err) == CCORD_OK);

  ccord_attachment atts[CCORD_RICH_MAX_ATTACHMENTS + 1];
  memset(atts, 0, sizeof atts);
  for (size_t i = 0; i < CCORD_RICH_MAX_ATTACHMENTS + 1; i++) { atts[i].filename = "f"; atts[i].url = "u"; }
  ccord_message_rich r = {.attachments = atts, .attachment_count = CCORD_RICH_MAX_ATTACHMENTS};
  CT_CHECK(ccord_message_rich_validate(&r, &err) == CCORD_OK);
  r.attachment_count = CCORD_RICH_MAX_ATTACHMENTS + 1;
  CT_CHECK(ccord_message_rich_validate(&r, &err) == CCORD_ERR_RANGE);
  r.attachment_count = 1;
  r.attachments = nullptr;
  CT_CHECK(ccord_message_rich_validate(&r, &err) != CCORD_OK);                 // a count without an array
  atts[0].filename = "bad\xFF";
  r.attachments = atts;
  CT_CHECK(ccord_message_rich_validate(&r, &err) != CCORD_OK);                 // not text
  atts[0].filename = nullptr;
  CT_CHECK(ccord_message_rich_validate(&r, &err) == CCORD_OK);                 // a missing text is an empty one
  atts[0].filename = "f";
  char longname[CCORD_RICH_SHORT_MAX_BYTES + 2];
  memset(longname, 'a', sizeof longname - 1);
  longname[sizeof longname - 1] = '\0';
  atts[0].filename = longname;
  CT_CHECK(ccord_message_rich_validate(&r, &err) != CCORD_OK);
  atts[0].filename = "f";
  CT_CHECK(ccord_message_rich_validate(&r, &err) == CCORD_OK);

  ccord_embed e[CCORD_RICH_MAX_EMBEDS + 1];
  memset(e, 0, sizeof e);
  for (size_t i = 0; i < CCORD_RICH_MAX_EMBEDS + 1; i++) {
    e[i] = (ccord_embed){.title = "", .description = "", .url = "", .provider = "", .author_name = "", .author_url = "", .author_icon = "",
                         .footer_text = "", .footer_icon = "", .image = {"", "", 0, 0}, .thumbnail = {"", "", 0, 0}, .video = {"", "", 0, 0}};
  }
  ccord_message_rich re = {.embeds = e, .embed_count = CCORD_RICH_MAX_EMBEDS};
  CT_CHECK(ccord_message_rich_validate(&re, &err) == CCORD_OK);
  re.embed_count = CCORD_RICH_MAX_EMBEDS + 1;
  CT_CHECK(ccord_message_rich_validate(&re, &err) == CCORD_ERR_RANGE);
  re.embed_count = 1;
  e[0].kind = (ccord_embed_kind)(CCORD_EMBED_OTHER + 1);
  CT_CHECK(ccord_message_rich_validate(&re, &err) != CCORD_OK);
  e[0].kind = CCORD_EMBED_RICH;
  e[0].color = 0x1000000u;
  CT_CHECK(ccord_message_rich_validate(&re, &err) != CCORD_OK);
  e[0].color = 0;
  e[0].timestamp_ms = -1;
  CT_CHECK(ccord_message_rich_validate(&re, &err) != CCORD_OK);
  e[0].timestamp_ms = 0;
  ccord_embed_field fl[CCORD_RICH_MAX_EMBED_FIELDS + 1];
  for (size_t i = 0; i < CCORD_RICH_MAX_EMBED_FIELDS + 1; i++) fl[i] = (ccord_embed_field){.name = "n", .value = "v"};
  e[0].fields = fl;
  e[0].field_count = CCORD_RICH_MAX_EMBED_FIELDS;
  CT_CHECK(ccord_message_rich_validate(&re, &err) == CCORD_OK);
  e[0].field_count = CCORD_RICH_MAX_EMBED_FIELDS + 1;
  CT_CHECK(ccord_message_rich_validate(&re, &err) == CCORD_ERR_RANGE);
  e[0].field_count = 1;
  e[0].image.url = "bad\xC0";
  CT_CHECK(ccord_message_rich_validate(&re, &err) != CCORD_OK);
  e[0].image.url = "";
  CT_CHECK(ccord_message_rich_validate(&re, &err) == CCORD_OK);

  ccord_id many[CCORD_RICH_MAX_MENTIONS + 1] = {0};
  ccord_message_rich rm = {.mention_users = many, .mention_user_count = CCORD_RICH_MAX_MENTIONS};
  CT_CHECK(ccord_message_rich_validate(&rm, &err) == CCORD_OK);
  rm.mention_user_count = CCORD_RICH_MAX_MENTIONS + 1;
  CT_CHECK(ccord_message_rich_validate(&rm, &err) == CCORD_ERR_RANGE);
  rm.mention_user_count = 0;
  rm.mention_roles = many;
  rm.mention_role_count = CCORD_RICH_MAX_MENTIONS + 1;
  CT_CHECK(ccord_message_rich_validate(&rm, &err) == CCORD_ERR_RANGE);

  ccord_msg_sticker sk[CCORD_RICH_MAX_STICKERS + 1];
  for (size_t i = 0; i < CCORD_RICH_MAX_STICKERS + 1; i++) sk[i] = (ccord_msg_sticker){.id = 1, .name = "s", .format = 1};
  ccord_message_rich rs = {.stickers = sk, .sticker_count = CCORD_RICH_MAX_STICKERS};
  CT_CHECK(ccord_message_rich_validate(&rs, &err) == CCORD_OK);
  rs.sticker_count = CCORD_RICH_MAX_STICKERS + 1;
  CT_CHECK(ccord_message_rich_validate(&rs, &err) == CCORD_ERR_RANGE);
  rs.sticker_count = 1;
  sk[0].format = 5;
  CT_CHECK(ccord_message_rich_validate(&rs, &err) != CCORD_OK);

  ccord_message_rich rp = {.reference = {.kind = (ccord_ref_kind)(CCORD_REF_FORWARD + 1)}};
  CT_CHECK(ccord_message_rich_validate(&rp, &err) != CCORD_OK);
  ccord_poll_answer pa[CCORD_RICH_MAX_POLL_ANSWERS];
  for (size_t i = 0; i < CCORD_RICH_MAX_POLL_ANSWERS; i++) pa[i] = (ccord_poll_answer){.text = "a", .emoji = ""};
  ccord_message_rich rq = {.poll = {.present = true, .question = "q", .answers = pa, .answer_count = CCORD_RICH_MAX_POLL_ANSWERS}};
  CT_CHECK(ccord_message_rich_validate(&rq, &err) == CCORD_OK);
  rq.poll.answer_count = CCORD_RICH_MAX_POLL_ANSWERS + 1;
  CT_CHECK(ccord_message_rich_validate(&rq, &err) == CCORD_ERR_RANGE);
  rq.poll.answer_count = 1;
  rq.poll.expiry_ms = -3;
  CT_CHECK(ccord_message_rich_validate(&rq, &err) != CCORD_OK);
  rq.poll.expiry_ms = 0;
  pa[0].text = "bad\xFE";
  CT_CHECK(ccord_message_rich_validate(&rq, &err) != CCORD_OK);

  // A null string in a producer-built block is an empty text (clone normalises it to "").
  ccord_message_rich rn = {.poll = {.present = true, .question = nullptr}};
  CT_CHECK(ccord_message_rich_validate(&rn, &err) == CCORD_OK);
  ccord_message_rich rt = {.forward = {.present = true, .content = "x", .timestamp_ms = -1}};
  CT_CHECK(ccord_message_rich_validate(&rt, &err) != CCORD_OK);
}

static void test_helpers(void)
{
  ccord_id us[2] = {5, 6}, rl[1] = {9};
  ccord_message_rich r = {.type = 7, .flags = 5, .mention_users = us, .mention_user_count = 2, .mention_roles = rl, .mention_role_count = 1};
  ccord_message m = {.rich = &r};
  CT_CHECK(ccord_message_type(&m) == 7 && ccord_message_flags(&m) == 5);
  ccord_message plain = {0};
  CT_CHECK(ccord_message_type(&plain) == 0 && ccord_message_flags(&plain) == 0 && ccord_message_type(nullptr) == 0);
  CT_CHECK(ccord_rich_mentions_user(&r, 5) && ccord_rich_mentions_user(&r, 6) && !ccord_rich_mentions_user(&r, 7));
  CT_CHECK(!ccord_rich_mentions_user(&r, CCORD_ID_NONE) && !ccord_rich_mentions_user(nullptr, 5));
  CT_CHECK(ccord_rich_mentions_role(&r, 9) && !ccord_rich_mentions_role(&r, 5) && !ccord_rich_mentions_role(nullptr, 9));

  // ccord_message_mentions_user: authoritative rich->mention_users when rich != nullptr; content fallback only when rich == nullptr.
  ccord_message m_rich_silent = {.content = "hi <@7> <@!7>", .rich = &r};
  CT_CHECK(ccord_message_mentions_user(&m_rich_silent, 5));
  CT_CHECK(!ccord_message_mentions_user(&m_rich_silent, 7));   // suppressed ping: <@7> in content, absent from mention_users
  ccord_message m_plain_mention = {.content = "hello <@42>!"};
  ccord_message m_plain_nick = {.content = "hello <@!42>!"};
  CT_CHECK(ccord_message_mentions_user(&m_plain_mention, 42));
  CT_CHECK(ccord_message_mentions_user(&m_plain_nick, 42));
  CT_CHECK(!ccord_message_mentions_user(&m_plain_mention, 43));

  ccord_attachment png = {.filename = "x.bin", .content_type = "image/png"};
  ccord_attachment jpg_noct = {.filename = "Photo.JPG", .content_type = ""};
  ccord_attachment mp4 = {.filename = "clip.mp4", .content_type = nullptr};
  ccord_attachment mp3 = {.filename = "a.b.MP3", .content_type = ""};
  ccord_attachment text = {.filename = "notes.txt", .content_type = "text/plain"};
  ccord_attachment lie = {.filename = "pic.png", .content_type = "application/pdf"};   // the MIME type wins when it is there
  CT_CHECK(ccord_attachment_is_image(&png) && !ccord_attachment_is_video(&png) && !ccord_attachment_is_audio(&png));
  CT_CHECK(ccord_attachment_is_image(&jpg_noct) && ccord_attachment_is_video(&mp4) && ccord_attachment_is_audio(&mp3));
  CT_CHECK(!ccord_attachment_is_image(&text) && !ccord_attachment_is_video(&text) && !ccord_attachment_is_audio(&text));
  CT_CHECK(!ccord_attachment_is_image(&lie));
  CT_CHECK(!ccord_attachment_is_image(nullptr) && !ccord_attachment_is_video(nullptr) && !ccord_attachment_is_audio(nullptr));
  ccord_attachment noext = {.filename = "README", .content_type = ""};
  ccord_attachment dot = {.filename = "x.", .content_type = ""};
  CT_CHECK(!ccord_attachment_is_image(&noext) && !ccord_attachment_is_image(&dot));
  ccord_attachment prefix = {.filename = "a.pngx", .content_type = ""};
  CT_CHECK(!ccord_attachment_is_image(&prefix));
}

static void test_preview_and_system_text(void)
{
  char buf[512];
  // Text wins and is flattened onto one line.
  ccord_message t = {.content = "line one\nline\ttwo"};
  CT_CHECK(ccord_message_preview(&t, buf, sizeof buf) == strlen("line one line two") && strcmp(buf, "line one line two") == 0);
  CT_CHECK(ccord_message_preview(nullptr, buf, sizeof buf) == 0 && buf[0] == '\0');
  CT_CHECK(ccord_message_preview(&t, nullptr, 8) == 0 && ccord_message_preview(&t, buf, 0) == 0);
  ccord_message plain = {.content = ""};
  CT_CHECK(ccord_message_preview(&plain, buf, sizeof buf) == 0 && buf[0] == '\0');

  // Cut to the buffer on a character boundary: "é" is two bytes and must not be halved.
  ccord_message accents = {.content = "aaé"};
  CT_CHECK(ccord_message_preview(&accents, buf, 4) == 2 && strcmp(buf, "aa") == 0);
  CT_CHECK(ccord_message_preview(&accents, buf, 5) == 4 && strcmp(buf, "aaé") == 0);
  CT_CHECK(ccord_message_preview(&accents, buf, 3) == 2);

  // Without text it says what the message carries, in a fixed order.
  ccord_attachment img = {.filename = "a.png", .content_type = "image/png"};
  ccord_attachment vid = {.filename = "a.mp4", .content_type = "video/mp4"};
  ccord_attachment aud = {.filename = "a.ogg", .content_type = "audio/ogg"};
  ccord_attachment doc = {.filename = "report.pdf", .content_type = "application/pdf"};
  ccord_attachment nameless = {.filename = "", .content_type = "application/pdf"};
  ccord_msg_sticker stk = {.id = 1, .name = "wave"};
  ccord_embed emb_title = {.title = "Release", .description = "notes"};
  ccord_embed emb_desc = {.description = "only description"};
  ccord_embed emb_url = {.url = "https://e.example/x"};
  ccord_embed emb_none = {.kind = CCORD_EMBED_RICH};
  ccord_message_rich r = {.present = CCORD_RICH_ALL};
  ccord_message m = {.content = "", .rich = &r};
  CT_CHECK(ccord_message_preview(&m, buf, sizeof buf) == 0);   // an empty plain-ish block says nothing
  r.attachments = &img; r.attachment_count = 1;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Image") == 0);
  r.attachments = &vid;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Video") == 0);
  r.attachments = &aud;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Audio") == 0);
  r.flags = CCORD_MSGFLAG_VOICE_MESSAGE;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Voice message") == 0);
  r.flags = 0;
  r.attachments = &doc;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Attachment: report.pdf") == 0);
  r.attachments = &nameless;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Attachment: file") == 0);
  r.stickers = &stk; r.sticker_count = 1;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Attachment: file") == 0);   // files come before stickers
  r.attachment_count = 0;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Sticker: wave") == 0);
  r.embeds = &emb_title; r.embed_count = 1;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Sticker: wave") == 0);   // stickers before embeds
  r.sticker_count = 0;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Release") == 0);
  r.embeds = &emb_desc;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "only description") == 0);
  r.embeds = &emb_url;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "https://e.example/x") == 0);
  r.embeds = &emb_none;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Embed") == 0);
  r.forward.present = true;
  r.attachments = &img; r.attachment_count = 1;
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Forwarded message") == 0);   // forwards before files
  r.poll.present = true; r.poll.question = "Pizza?";
  ccord_message_preview(&m, buf, sizeof buf); CT_CHECK(strcmp(buf, "Poll: Pizza?") == 0);        // polls first
  ccord_message with_text = {.content = "hi", .rich = &r};
  ccord_message_preview(&with_text, buf, sizeof buf); CT_CHECK(strcmp(buf, "hi") == 0);          // text beats everything
  ccord_message_rich sys = {.present = CCORD_RICH_ALL, .type = 7};
  ccord_message sysm = {.content = "", .rich = &sys};
  ccord_message_preview(&sysm, buf, sizeof buf); CT_CHECK(strcmp(buf, "System message") == 0);
  ccord_message_rich reply = {.present = CCORD_RICH_ALL, .type = 19};
  ccord_message replym = {.content = "", .rich = &reply};
  CT_CHECK(ccord_message_preview(&replym, buf, sizeof buf) == 0);   // a reply with no text is not a system message
  size_t cut = ccord_message_preview(&m, buf, 6);
  CT_CHECK(cut == 5 && strcmp(buf, "Poll:") == 0);

  // System message types and wording.
  const uint16_t ordinary[] = {0, 19, 20, 21, 23};
  for (size_t i = 0; i < sizeof ordinary / sizeof ordinary[0]; i++) {
    CT_CHECK(!ccord_message_type_is_system(ordinary[i]));
    CT_CHECK(ccord_message_type_phrase(ordinary[i]) == nullptr);
  }
  for (uint16_t ty = 1; ty <= 12; ty++) {
    CT_CHECK(ccord_message_type_is_system(ty));
    CT_CHECK(ccord_message_type_phrase(ty) != nullptr && ccord_message_type_phrase(ty)[0] != '\0');
  }
  CT_CHECK(strcmp(ccord_message_type_phrase(7), "joined the server.") == 0);
  CT_CHECK(strcmp(ccord_message_type_phrase(6), "pinned a message to this channel.") == 0);
  CT_CHECK(strcmp(ccord_message_type_phrase(18), "started a thread:") == 0);
  CT_CHECK(strcmp(ccord_message_type_phrase(4), "changed the channel name:") == 0);
  CT_CHECK(ccord_message_type_phrase(22) != nullptr);
  CT_CHECK(ccord_message_type_is_system(13) && ccord_message_type_phrase(13) == nullptr);   // a system type with no wording
  CT_CHECK(ccord_message_type_is_system(46) && ccord_message_type_phrase(46) == nullptr);
  CT_CHECK(strcmp(ccord_message_type_phrase(8), ccord_message_type_phrase(9)) != 0);
  CT_CHECK(strcmp(ccord_message_type_phrase(9), ccord_message_type_phrase(10)) != 0 &&
           strcmp(ccord_message_type_phrase(10), ccord_message_type_phrase(11)) != 0);

  // Every standard wording, exactly (it is shown to the user after the author's name).
  static const struct { uint16_t type; const char *text; } phrases[] = {
    {1, "added someone to the group."}, {2, "removed someone from the group."}, {3, "started a call."},
    {4, "changed the channel name:"}, {5, "changed the group icon."}, {6, "pinned a message to this channel."},
    {7, "joined the server."}, {8, "boosted the server!"}, {9, "boosted the server, which reached Level 1!"},
    {10, "boosted the server, which reached Level 2!"}, {11, "boosted the server, which reached Level 3!"},
    {12, "added a channel to follow."}, {18, "started a thread:"}, {22, "Wondering who to invite? Share the server link!"},
    {25, "subscribed to a server role."}, {27, "started a Stage:"}, {28, "ended the Stage:"},
    {29, "is now a speaker."}, {30, "requested to speak."}, {31, "changed the Stage topic:"},
    {44, "purchased a server product."}, {45, "invited you to a voice hangout."},
    {63, "added a new emoji."}, {65, "started a voice hangout."},
    {67, "accepted your friend request."},
  };
  for (size_t i = 0; i < sizeof phrases / sizeof phrases[0]; i++) {
    const char *got = ccord_message_type_phrase(phrases[i].type);
    CT_CHECK(got != nullptr && strcmp(got, phrases[i].text) == 0);
  }
  CT_CHECK(ccord_message_type_appends_content(4) && ccord_message_type_appends_content(18) &&
           ccord_message_type_appends_content(27) && ccord_message_type_appends_content(28) &&
           ccord_message_type_appends_content(31));
  CT_CHECK(!ccord_message_type_appends_content(0) && !ccord_message_type_appends_content(6) &&
           !ccord_message_type_appends_content(7) && !ccord_message_type_appends_content(67));

  // The cut never lands inside a character: a four-byte one, a three-byte one, and a sequence the text ends in the middle of.
  ccord_message emoji = {.content = "ab\xF0\x9F\x98\x80"};
  CT_CHECK(ccord_message_preview(&emoji, buf, 7) == 6 && strcmp(buf, "ab\xF0\x9F\x98\x80") == 0);
  CT_CHECK(ccord_message_preview(&emoji, buf, 6) == 2 && strcmp(buf, "ab") == 0);
  ccord_message euro = {.content = "ab\xE2\x82\xAC"};
  CT_CHECK(ccord_message_preview(&euro, buf, 6) == 5 && strcmp(buf, "ab\xE2\x82\xAC") == 0);
  CT_CHECK(ccord_message_preview(&euro, buf, 5) == 2 && strcmp(buf, "ab") == 0);
  ccord_message torn = {.content = "ab\xE2\x82"};   // the third byte of the character is missing
  CT_CHECK(ccord_message_preview(&torn, buf, sizeof buf) == 2 && strcmp(buf, "ab") == 0 && strlen(buf) == 2);
}

static void test_components_clone_merge_validate_preview(void)
{
  ccord_error err;
  char lbl[32] = "Approve", cid[32] = "btn:ok", url[64] = "https://example.com/docs";
  char opt_lbl[32] = "Option A", opt_val[32] = "opt_a", opt_desc[32] = "First pick", opt_emj[16] = "star";

  ccord_select_option opts[2] = {
    {.label = opt_lbl, .value = opt_val, .description = opt_desc, .emoji = opt_emj, .emoji_id = 901, .default_ = true},
    {.label = nullptr, .value = "opt_b", .description = nullptr, .emoji = nullptr, .default_ = false},
  };
  ccord_component row0_cs[3] = {
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = lbl, .custom_id = cid, .emoji = "ok", .emoji_id = 55},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_LINK, .label = "Docs", .url = url, .disabled = true},
    {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PREMIUM, .label = nullptr},
  };
  ccord_component row1_cs[1] = {
    {.kind = CCORD_COMP_STRING_SELECT, .custom_id = "sel:pick", .placeholder = "Choose...",
     .min_values = 1, .max_values = 2, .options = opts, .option_count = 2},
  };
  ccord_component_row rows[2] = {
    {.components = row0_cs, .component_count = 3},
    {.components = row1_cs, .component_count = 1},
  };
  ccord_message_rich src = {
    .present = CCORD_RICH_ALL,
    .application_id = 777001,
    .components = rows,
    .component_row_count = 2,
  };
  CT_CHECK(ccord_message_rich_validate(&src, &err) == CCORD_OK);

  ccord_message_rich *c = ccord_message_rich_clone(&src, &err);
  CT_CHECK(c != nullptr);
  if (!c) return;
  memset(lbl, 'X', sizeof lbl - 1);
  memset(cid, 'X', sizeof cid - 1);
  memset(url, 'X', sizeof url - 1);
  memset(opt_lbl, 'X', sizeof opt_lbl - 1);
  memset(opt_val, 'X', sizeof opt_val - 1);
  memset(opt_desc, 'X', sizeof opt_desc - 1);
  memset(opt_emj, 'X', sizeof opt_emj - 1);

  CT_CHECK(c->application_id == 777001 && c->component_row_count == 2);
  CT_CHECK(c->components[0].component_count == 3);
  CT_CHECK(strcmp(c->components[0].components[0].label, "Approve") == 0 &&
           strcmp(c->components[0].components[0].custom_id, "btn:ok") == 0 &&
           strcmp(c->components[0].components[0].emoji, "ok") == 0 &&
           c->components[0].components[0].emoji_id == 55);
  CT_CHECK(c->components[0].components[1].style == CCORD_BTN_LINK &&
           strcmp(c->components[0].components[1].url, "https://example.com/docs") == 0 &&
           c->components[0].components[1].disabled);
  CT_CHECK(c->components[0].components[2].style == CCORD_BTN_PREMIUM &&
           c->components[0].components[2].label != nullptr && c->components[0].components[2].label[0] == '\0');
  CT_CHECK(c->components[1].component_count == 1);
  const ccord_component *sc = &c->components[1].components[0];
  CT_CHECK(sc->kind == CCORD_COMP_STRING_SELECT && sc->min_values == 1 && sc->max_values == 2 && sc->option_count == 2);
  CT_CHECK(strcmp(sc->options[0].label, "Option A") == 0 && strcmp(sc->options[0].value, "opt_a") == 0 &&
           strcmp(sc->options[0].description, "First pick") == 0 && strcmp(sc->options[0].emoji, "star") == 0 &&
           sc->options[0].emoji_id == 901 && sc->options[0].default_);
  CT_CHECK(sc->options[1].label != nullptr && sc->options[1].description != nullptr && sc->options[1].emoji != nullptr);
  CT_CHECK(strcmp(sc->options[1].value, "opt_b") == 0 && !sc->options[1].default_);

  // Partial merge: non-component update keeps components & application_id; component update replaces rows and preserves application_id when 0.
  ccord_message_rich flag_upd = {.present = CCORD_RICH_HAS_TYPE, .flags = CCORD_MSGFLAG_URGENT};
  ccord_message_rich *m1 = ccord_message_rich_merge(c, &flag_upd, &err);
  CT_CHECK(m1 && m1->component_row_count == 2 && m1->application_id == 777001 && m1->flags == CCORD_MSGFLAG_URGENT);
  ccord_message_rich_free(m1);

  ccord_component upd_btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_DANGER, .label = "Stop", .custom_id = "btn:stop"};
  ccord_component_row upd_row = {.components = &upd_btn, .component_count = 1};
  ccord_message_rich comp_upd = {.present = CCORD_RICH_HAS_COMPONENTS, .application_id = 0, .components = &upd_row, .component_row_count = 1};
  ccord_message_rich *m2 = ccord_message_rich_merge(c, &comp_upd, &err);
  CT_CHECK(m2 && m2->component_row_count == 1 && m2->application_id == 777001 &&
           strcmp(m2->components[0].components[0].custom_id, "btn:stop") == 0);
  ccord_message_rich_free(m2);

  comp_upd.application_id = 888002;
  ccord_message_rich *m3 = ccord_message_rich_merge(c, &comp_upd, &err);
  CT_CHECK(m3 && m3->application_id == 888002);
  ccord_message_rich_free(m3);

  ccord_message_rich clear_upd = {.present = CCORD_RICH_HAS_COMPONENTS};
  ccord_message_rich *m4 = ccord_message_rich_merge(c, &clear_upd, &err);
  CT_CHECK(m4 && m4->component_row_count == 0 && m4->components == nullptr && m4->application_id == 777001);
  ccord_message_rich_free(m4);

  // Preview: empty message with components shows "Interactive message", beaten by embeds/text.
  char pbuf[64];
  ccord_message cm = {.content = "", .rich = c};
  CT_CHECK(ccord_message_preview(&cm, pbuf, sizeof pbuf) > 0 && strcmp(pbuf, "Interactive message") == 0);
  ccord_message_rich_free(c);

  // Validation bounds and invariants.
  ccord_component_row many_rows[CCORD_RICH_MAX_COMPONENT_ROWS + 1] = {0};
  ccord_message_rich bad = {.components = many_rows, .component_row_count = CCORD_RICH_MAX_COMPONENT_ROWS + 1};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_RANGE);
  bad = (ccord_message_rich){.components = nullptr, .component_row_count = 1};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);

  ccord_component many_cs[CCORD_RICH_MAX_ROW_COMPONENTS + 1] = {0};
  ccord_component_row bad_row = {.components = many_cs, .component_count = CCORD_RICH_MAX_ROW_COMPONENTS + 1};
  bad = (ccord_message_rich){.components = &bad_row, .component_row_count = 1};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_RANGE);
  bad_row = (ccord_component_row){.components = nullptr, .component_count = 1};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);

  // TEXT_INPUT (4) is modal-only, never valid inside a message's component rows.
  ccord_component ti = {.kind = CCORD_COMP_TEXT_INPUT, .custom_id = "ti"};
  bad_row = (ccord_component_row){.components = &ti, .component_count = 1};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);

  // Link button requires url; interactive button requires custom_id; invalid style rejected.
  ccord_component link_no_url = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_LINK, .label = "Go"};
  bad_row.components = &link_no_url;
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  ccord_component btn_no_id = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Click"};
  bad_row.components = &btn_no_id;
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  ccord_component btn_bad_style = {.kind = CCORD_COMP_BUTTON, .style = 99, .custom_id = "x"};
  bad_row.components = &btn_bad_style;
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);

  // Select bounds: missing custom_id, min > max, > 25 options, empty option value.
  ccord_component sel_no_id = {.kind = CCORD_COMP_ROLE_SELECT};
  bad_row.components = &sel_no_id;
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  ccord_component sel_minmax = {.kind = CCORD_COMP_USER_SELECT, .custom_id = "u", .min_values = 3, .max_values = 2};
  bad_row.components = &sel_minmax;
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  ccord_component sel_over = {.kind = CCORD_COMP_CHANNEL_SELECT, .custom_id = "c", .max_values = CCORD_RICH_MAX_SELECT_OPTIONS + 1};
  bad_row.components = &sel_over;
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_RANGE);
  ccord_select_option empty_opt = {.label = "L", .value = ""};
  ccord_component sel_bad_opt = {.kind = CCORD_COMP_STRING_SELECT, .custom_id = "s", .options = &empty_opt, .option_count = 1};
  bad_row.components = &sel_bad_opt;
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
}

// Container > Section(TextDisplay + Thumbnail accessory) + MediaGallery + File + Separator + ActionRow(Button), built from
// stack data so a clone proves it copied rather than aliased.
typedef struct v2_sample {
  char text[32], thumb_url[48], fname[16], btn_id[16], gal_url[48];
  ccord_component text_node, thumb, btn;
  ccord_media_ref gallery[2];
  ccord_component kids[5];
  ccord_component container;
  ccord_attachment atts[2];
  ccord_message_rich r;
} v2_sample;

static void fill_v2(v2_sample *s)
{
  memset(s, 0, sizeof *s);
  strcpy(s->text, "**Hello** v2");
  strcpy(s->thumb_url, "attachment://cat.png");
  strcpy(s->fname, "doc.pdf");
  strcpy(s->btn_id, "v2:go");
  strcpy(s->gal_url, "https://cdn.example/x.png");
  s->text_node = (ccord_component){.kind = CCORD_COMP_TEXT_DISPLAY, .content = s->text};
  s->thumb = (ccord_component){.kind = CCORD_COMP_THUMBNAIL,
                               .media = {.url = s->thumb_url, .proxy_url = "https://proxy.example/cat", .width = 64, .height = 48,
                                         .content_type = "image/png", .description = "a cat", .spoiler = true}};
  s->btn = (ccord_component){.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Go", .custom_id = s->btn_id};
  s->gallery[0] = (ccord_media_ref){.url = "attachment://cat.png", .description = "first"};
  s->gallery[1] = (ccord_media_ref){.url = s->gal_url, .proxy_url = nullptr, .width = 10, .height = 20, .spoiler = true};
  s->kids[0] = (ccord_component){.kind = CCORD_COMP_SECTION, .children = &s->text_node, .child_count = 1, .accessory = &s->thumb};
  s->kids[1] = (ccord_component){.kind = CCORD_COMP_MEDIA_GALLERY, .gallery = s->gallery, .gallery_count = 2};
  s->kids[2] = (ccord_component){.kind = CCORD_COMP_FILE, .media = {.url = "attachment://doc.pdf"}, .file_name = s->fname,
                                 .file_size = 4242, .spoiler = true};
  s->kids[3] = (ccord_component){.kind = CCORD_COMP_SEPARATOR, .divider = true, .spacing = 2};
  s->kids[4] = (ccord_component){.kind = CCORD_COMP_ACTION_ROW, .children = &s->btn, .child_count = 1};
  s->container = (ccord_component){.kind = CCORD_COMP_CONTAINER, .children = s->kids, .child_count = 5,
                                   .accent_color = 0xFF8800, .has_accent_color = true};
  s->atts[0] = (ccord_attachment){.id = 1, .filename = "cat.png", .url = "https://cdn.example/att/cat.png"};
  s->atts[1] = (ccord_attachment){.id = 2, .filename = "doc.pdf", .url = "", .proxy_url = "https://proxy.example/doc"};
  s->r = (ccord_message_rich){.present = CCORD_RICH_ALL, .flags = CCORD_MSGFLAG_IS_COMPONENTS_V2, .application_id = 4242,
                              .attachments = s->atts, .attachment_count = 2,
                              .v2_components = &s->container, .v2_component_count = 1};
}

static void test_components_v2(void)
{
  ccord_error err;
  v2_sample *s = malloc(sizeof *s);
  CT_CHECK(s != nullptr);
  if (!s) return;
  fill_v2(s);
  CT_CHECK(CCORD_MSGFLAG_IS_COMPONENTS_V2 == 32768u);
  CT_CHECK(ccord_message_rich_validate(&s->r, &err) == CCORD_OK);

  // Clone is a deep copy: scribbling over the source leaves the clone intact, and free releases it all (the sanitizer
  // build catches a leak).
  ccord_message_rich *c = ccord_message_rich_clone(&s->r, &err);
  CT_CHECK(c != nullptr);
  if (!c) { free(s); return; }
  memset(s->text, 'x', sizeof s->text - 1u);
  memset(s->thumb_url, 'x', sizeof s->thumb_url - 1u);
  memset(s->fname, 'x', sizeof s->fname - 1u);
  memset(s->btn_id, 'x', sizeof s->btn_id - 1u);
  memset(s->gal_url, 'x', sizeof s->gal_url - 1u);
  CT_CHECK(c->v2_component_count == 1 && c->v2_components != &s->container && (c->flags & CCORD_MSGFLAG_IS_COMPONENTS_V2));
  const ccord_component *ct = &c->v2_components[0];
  CT_CHECK(ct->kind == CCORD_COMP_CONTAINER && ct->child_count == 5 && ct->children != s->kids &&
           ct->has_accent_color && ct->accent_color == 0xFF8800 && !ct->spoiler);
  const ccord_component *sec = &ct->children[0];
  CT_CHECK(sec->kind == CCORD_COMP_SECTION && sec->child_count == 1 && sec->accessory != nullptr && sec->accessory != &s->thumb);
  CT_CHECK(strcmp(sec->children[0].content, "**Hello** v2") == 0);
  CT_CHECK(sec->accessory->kind == CCORD_COMP_THUMBNAIL && strcmp(sec->accessory->media.url, "attachment://cat.png") == 0 &&
           strcmp(sec->accessory->media.proxy_url, "https://proxy.example/cat") == 0 &&
           strcmp(sec->accessory->media.content_type, "image/png") == 0 && strcmp(sec->accessory->media.description, "a cat") == 0 &&
           sec->accessory->media.width == 64 && sec->accessory->media.height == 48 && sec->accessory->media.spoiler);
  const ccord_component *gal = &ct->children[1];
  CT_CHECK(gal->kind == CCORD_COMP_MEDIA_GALLERY && gal->gallery_count == 2 && gal->gallery != s->gallery &&
           strcmp(gal->gallery[0].description, "first") == 0 && strcmp(gal->gallery[1].url, "https://cdn.example/x.png") == 0 &&
           gal->gallery[1].proxy_url != nullptr && gal->gallery[1].proxy_url[0] == '\0' && gal->gallery[1].spoiler);
  const ccord_component *file = &ct->children[2];
  CT_CHECK(file->kind == CCORD_COMP_FILE && strcmp(file->file_name, "doc.pdf") == 0 && file->file_size == 4242 && file->spoiler &&
           strcmp(file->media.url, "attachment://doc.pdf") == 0);
  const ccord_component *sep = &ct->children[3];
  CT_CHECK(sep->kind == CCORD_COMP_SEPARATOR && sep->divider && sep->spacing == 2);
  const ccord_component *row = &ct->children[4];
  CT_CHECK(row->kind == CCORD_COMP_ACTION_ROW && row->child_count == 1 && row->children[0].kind == CCORD_COMP_BUTTON &&
           strcmp(row->children[0].custom_id, "v2:go") == 0 && strcmp(row->children[0].label, "Go") == 0);
  CT_CHECK(ccord_message_rich_validate(c, &err) == CCORD_OK);

  // A nested button is found by custom_id; the pointer is the clone's own node; misses and empty ids find nothing.
  const ccord_component *hit = ccord_rich_find_component(c, "v2:go");
  CT_CHECK(hit == &row->children[0]);
  CT_CHECK(ccord_rich_find_component(c, "nope") == nullptr && ccord_rich_find_component(c, "") == nullptr &&
           ccord_rich_find_component(c, nullptr) == nullptr && ccord_rich_find_component(nullptr, "v2:go") == nullptr);
  // ... and so are an accessory button and a v1 row button.
  ccord_component acc_btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_DANGER, .label = "X", .custom_id = "acc:x"};
  ccord_component acc_text = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "t"};
  ccord_component acc_sec = {.kind = CCORD_COMP_SECTION, .children = &acc_text, .child_count = 1, .accessory = &acc_btn};
  ccord_component v1_btn = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .label = "Old", .custom_id = "v1:old"};
  ccord_component_row v1_row = {.components = &v1_btn, .component_count = 1};
  ccord_message_rich mixed = {.v2_components = &acc_sec, .v2_component_count = 1, .components = &v1_row, .component_row_count = 1};
  CT_CHECK(ccord_message_rich_validate(&mixed, &err) == CCORD_OK);
  CT_CHECK(ccord_rich_find_component(&mixed, "acc:x") == &acc_btn && ccord_rich_find_component(&mixed, "v1:old") == &v1_btn);

  // attachment:// resolves against the message's attachments; anything else is its own url.
  CT_CHECK(strcmp(ccord_rich_media_url(c, &sec->accessory->media), "https://cdn.example/att/cat.png") == 0);
  CT_CHECK(strcmp(ccord_rich_media_url(c, &gal->gallery[1]), "https://cdn.example/x.png") == 0);
  CT_CHECK(strcmp(ccord_rich_media_url(c, &file->media), "https://proxy.example/doc") == 0);   // attachment has no url: its proxy_url
  ccord_media_ref missing = {.url = "attachment://gone.png", .proxy_url = "https://proxy.example/gone"};
  CT_CHECK(strcmp(ccord_rich_media_url(c, &missing), "https://proxy.example/gone") == 0);
  missing.proxy_url = nullptr;
  CT_CHECK(strcmp(ccord_rich_media_url(c, &missing), "") == 0 && strcmp(ccord_rich_media_url(nullptr, &missing), "") == 0);
  ccord_media_ref proxied = {.url = "", .proxy_url = "https://proxy.example/p"};
  CT_CHECK(strcmp(ccord_rich_media_url(c, &proxied), "https://proxy.example/p") == 0 && strcmp(ccord_rich_media_url(c, nullptr), "") == 0);

  // Preview: the first text, else a file, else media, else "Interactive message".
  char pbuf[64];
  ccord_message pm = {.content = "", .rich = c};
  CT_CHECK(ccord_message_preview(&pm, pbuf, sizeof pbuf) > 0 && strcmp(pbuf, "**Hello** v2") == 0);
  ccord_component only_file[1] = {s->kids[2]};
  only_file[0].file_name = "doc.pdf";
  ccord_message_rich fr = {.v2_components = only_file, .v2_component_count = 1};
  pm.rich = &fr;
  CT_CHECK(ccord_message_preview(&pm, pbuf, sizeof pbuf) > 0 && strcmp(pbuf, "Attachment: doc.pdf") == 0);
  ccord_component only_gal[1] = {*gal};
  fr.v2_components = only_gal;
  CT_CHECK(ccord_message_preview(&pm, pbuf, sizeof pbuf) > 0 && strcmp(pbuf, "Media") == 0);
  ccord_component only_sep[1] = {*sep};
  fr.v2_components = only_sep;
  CT_CHECK(ccord_message_preview(&pm, pbuf, sizeof pbuf) > 0 && strcmp(pbuf, "Interactive message") == 0);
  ccord_message text_wins = {.content = "plain", .rich = c};
  CT_CHECK(ccord_message_preview(&text_wins, pbuf, sizeof pbuf) > 0 && strcmp(pbuf, "plain") == 0);

  // Merge: an update without components keeps the tree; one that carries components replaces it (and drops the v2 tree
  // when it brings none).
  ccord_message_rich flag_upd = {.present = CCORD_RICH_HAS_TYPE, .flags = CCORD_MSGFLAG_IS_COMPONENTS_V2 | CCORD_MSGFLAG_URGENT};
  ccord_message_rich *m1 = ccord_message_rich_merge(c, &flag_upd, &err);
  CT_CHECK(m1 && m1->v2_component_count == 1 && strcmp(ccord_rich_find_component(m1, "v2:go")->label, "Go") == 0 &&
           m1->application_id == 4242);
  ccord_message_rich_free(m1);
  ccord_component new_text = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "edited"};
  ccord_message_rich comp_upd = {.present = CCORD_RICH_HAS_COMPONENTS, .v2_components = &new_text, .v2_component_count = 1};
  ccord_message_rich *m2 = ccord_message_rich_merge(c, &comp_upd, &err);
  CT_CHECK(m2 && m2->v2_component_count == 1 && strcmp(m2->v2_components[0].content, "edited") == 0 &&
           ccord_rich_find_component(m2, "v2:go") == nullptr && m2->attachment_count == 2);
  ccord_message_rich_free(m2);
  ccord_message_rich clear_upd = {.present = CCORD_RICH_HAS_COMPONENTS};
  ccord_message_rich *m3 = ccord_message_rich_merge(c, &clear_upd, &err);
  CT_CHECK(m3 && m3->v2_component_count == 0 && m3->v2_components == nullptr);
  ccord_message_rich_free(m3);
  ccord_message_rich_free(c);

  // The budget: 40 nodes pass, 41 do not; depth 4 passes the budget, depth 5 does not.
  ccord_component texts[40];
  for (size_t i = 0; i < 40; i++) texts[i] = (ccord_component){.kind = CCORD_COMP_TEXT_DISPLAY, .content = "t"};
  ccord_component big = {.kind = CCORD_COMP_CONTAINER, .children = texts, .child_count = 39};   // 1 + 39 = 40 nodes
  ccord_message_rich br = {.v2_components = &big, .v2_component_count = 1};
  CT_CHECK(ccord_message_rich_validate(&br, &err) == CCORD_OK);
  big.child_count = 40;                                                                          // 41 nodes
  CT_CHECK(ccord_message_rich_validate(&br, &err) == CCORD_ERR_RANGE && strstr(err.message, "too many") != nullptr);
  ccord_component flat[41];
  for (size_t i = 0; i < 41; i++) flat[i] = (ccord_component){.kind = CCORD_COMP_SEPARATOR, .divider = true, .spacing = 1};
  br = (ccord_message_rich){.v2_components = flat, .v2_component_count = 41};
  CT_CHECK(ccord_message_rich_validate(&br, &err) == CCORD_ERR_RANGE);
  br.v2_component_count = 40;
  CT_CHECK(ccord_message_rich_validate(&br, &err) == CCORD_OK);

  // Nested containers are not valid Discord, but the depth verdict comes first: 4 levels fail on kind, 5 on depth.
  ccord_component chain[5];
  memset(chain, 0, sizeof chain);
  for (size_t i = 0; i < 5; i++) {
    chain[i].kind = CCORD_COMP_CONTAINER;
    if (i + 1 < 5) { chain[i].children = &chain[i + 1]; chain[i].child_count = 1; }
  }
  br = (ccord_message_rich){.v2_components = chain, .v2_component_count = 1};
  CT_CHECK(ccord_message_rich_validate(&br, &err) == CCORD_ERR_RANGE && strstr(err.message, "deep") != nullptr);
  chain[3].child_count = 0;
  CT_CHECK(ccord_message_rich_validate(&br, &err) == CCORD_ERR_INVALID);   // depth 4: inside the budget, wrong placement
  // A clone of a too-deep tree stays too deep (clone never validates, but it must not recurse without bound).
  chain[3].child_count = 1;
  ccord_message_rich *deep = ccord_message_rich_clone(&br, &err);
  CT_CHECK(deep != nullptr && ccord_message_rich_validate(deep, &err) == CCORD_ERR_RANGE);
  ccord_message_rich_free(deep);

  // Shape rules.
  ccord_component btn_row_child = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY, .custom_id = "b"};
  ccord_component thumb = {.kind = CCORD_COMP_THUMBNAIL, .media = {.url = "https://e/x.png"}};
  ccord_component text = {.kind = CCORD_COMP_TEXT_DISPLAY, .content = "t"};
  ccord_component bad_node;
  ccord_message_rich bad = {.v2_components = &bad_node, .v2_component_count = 1};
  bad_node = thumb;                                         // a Thumbnail is only an accessory
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad_node = btn_row_child;                                 // so is a button, outside a row or section
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad_node = (ccord_component){.kind = (ccord_component_kind)15, .custom_id = "z"};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad_node = (ccord_component){.kind = CCORD_COMP_SECTION, .children = &text, .child_count = 1};   // no accessory
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad_node = (ccord_component){.kind = CCORD_COMP_SECTION, .children = &btn_row_child, .child_count = 1, .accessory = &thumb};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);                         // text slot holds a button
  bad_node = (ccord_component){.kind = CCORD_COMP_SECTION, .children = nullptr, .child_count = 0, .accessory = &thumb};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_RANGE);
  bad_node = (ccord_component){.kind = CCORD_COMP_MEDIA_GALLERY};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_RANGE);
  bad_node = (ccord_component){.kind = CCORD_COMP_FILE, .file_name = "f"};                         // no media url
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad_node = (ccord_component){.kind = CCORD_COMP_SEPARATOR, .spacing = 3};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad_node = (ccord_component){.kind = CCORD_COMP_CONTAINER, .accent_color = 0x1000000u};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad_node = (ccord_component){.kind = CCORD_COMP_TEXT_DISPLAY, .content = "\xff\xfe"};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  ccord_component no_id = {.kind = CCORD_COMP_BUTTON, .style = CCORD_BTN_PRIMARY};
  bad_node = (ccord_component){.kind = CCORD_COMP_ACTION_ROW, .children = &no_id, .child_count = 1};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad_node = (ccord_component){.kind = CCORD_COMP_CONTAINER, .children = nullptr, .child_count = 1};
  CT_CHECK(ccord_message_rich_validate(&bad, &err) == CCORD_ERR_INVALID);
  free(s);
}

static void test_ephemeral_and_slash_commands(void)
{
  ccord_error err;
  ccord_message_rich er = {.flags = CCORD_MSGFLAG_EPHEMERAL};
  ccord_message em = {.rich = &er};
  ccord_message nm = {0};
  CT_CHECK(ccord_message_is_ephemeral(&em));
  CT_CHECK(!ccord_message_is_ephemeral(&nm));
  CT_CHECK(!ccord_message_is_ephemeral(nullptr));

  const ccord_slash_option opts[2] = {
    {.kind = CCORD_SLASH_OPT_STRING, .name = "subject", .description = "Topic name", .required = true, .autocomplete = true},
    {.kind = CCORD_SLASH_OPT_BOOLEAN, .name = "ephemeral", .description = "Private reply", .required = false, .autocomplete = false},
  };
  ccord_slash_command cmd = {
    .id = 9001,
    .application_id = 1011,
    .version = 9001,
    .name = "topic",
    .description = "Look up a topic",
    .default_member_permissions = UINT64_MAX,
    .dm_permission = true,
    .option_count = 2,
    .options = opts,
  };
  CT_CHECK(ccord_slash_command_validate(&cmd, &err) == CCORD_OK);
  CT_CHECK(ccord_slash_command_validate(nullptr, &err) == CCORD_ERR_INVALID);

  ccord_slash_command bad = cmd;
  bad.id = CCORD_ID_NONE;
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  snprintf(bad.name, sizeof bad.name, "bad name");
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  bad.option_count = CCORD_SLASH_MAX_OPTIONS + 1;
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_RANGE);
  ccord_slash_option bad_opts[2];
  bad = cmd;
  memcpy(bad_opts, opts, sizeof opts);
  bad.options = bad_opts;
  bad_opts[0].kind = (ccord_slash_opt_kind)99;
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
  bad = cmd;
  memcpy(bad_opts, opts, sizeof opts);
  bad.options = bad_opts;
  bad_opts[1].autocomplete = true; // BOOLEAN cannot have autocomplete
  CT_CHECK(ccord_slash_command_validate(&bad, &err) == CCORD_ERR_INVALID);
}

int main(void)
{
  test_clone_is_deep();
  test_merge_keeps_what_the_update_lacks();
  test_validation();
  test_helpers();
  test_preview_and_system_text();
  test_components_clone_merge_validate_preview();
  test_components_v2();
  test_ephemeral_and_slash_commands();
  return CT_RESULT("test_rich");
}
