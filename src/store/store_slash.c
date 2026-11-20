// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "store_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>

void store_slash_clear(ccord_store *s)
{
  if (!s) return;
  for (size_t i = 0; i < s->nslash; i++) ccord_free(s->slash[i].cmds);
  ccord_free(s->slash);
  s->slash = nullptr;
  s->nslash = 0;
  s->capslash = 0;
}


ccord_status ccord_store_set_slash_commands(ccord_store *s, ccord_id guild_id,
                                            const ccord_slash_command *cmds, size_t count,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null");
  if (guild_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: slash guild_id");
  if (count > CCORD_SLASH_MAX_COMMANDS) return ccord_error_set(err, CCORD_ERR_RANGE, "store: too many slash commands");
  if (count > 0 && !cmds) return ccord_error_set(err, CCORD_ERR_INVALID, "store: null slash commands");
  for (size_t i = 0; i < count; i++) {
    ccord_status vst = ccord_slash_command_validate(&cmds[i], err);
    if (vst != CCORD_OK) return vst;
  }

  ccord_slash_command *copy = nullptr;
  size_t kept = 0;
  if (count > 0) {
    copy = ccord_alloc_array(count, sizeof *copy);
    if (!copy) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    for (size_t i = 0; i < count; i++) {
      bool dup = false;
      for (size_t k = 0; k < kept; k++) {
        if (copy[k].id == cmds[i].id) {
          dup = true;
          break;
        }
      }
      if (!dup) copy[kept++] = cmds[i];
    }
  }

  for (size_t i = 0; i < s->nslash; i++) {
    if (s->slash[i].guild_id == guild_id) {
      ccord_free(s->slash[i].cmds);
      s->slash[i].cmds = copy;
      s->slash[i].n = kept;
      store_bump_traffic(s);
      return CCORD_OK;
    }
  }

  if (s->nslash == s->capslash) {
    size_t ncap = s->capslash ? s->capslash * 2 : 8;
    slash_bucket *grown = ccord_realloc_array(s->slash, ncap, sizeof *grown);
    if (!grown) {
      ccord_free(copy);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    s->slash = grown;
    s->capslash = ncap;
  }
  s->slash[s->nslash++] = (slash_bucket){.guild_id = guild_id, .cmds = copy, .n = kept};
  store_bump_traffic(s);
  return CCORD_OK;
}

const ccord_slash_command *ccord_store_slash_commands(const ccord_store *s, ccord_id guild_id, size_t *out_count)
{
  if (out_count) *out_count = 0;
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nslash; i++) {
    if (s->slash[i].guild_id == guild_id) {
      if (out_count) *out_count = s->slash[i].n;
      return s->slash[i].cmds;
    }
  }
  return nullptr;
}


