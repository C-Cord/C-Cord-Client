// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ui/account_identity.h>

#include <string.h>

void ccord_identity_clear(ccord_identity *identity)
{
  if (identity) {
    memset(identity, 0, sizeof *identity);
  }
}

static size_t utf8_seq_len(unsigned char c)
{
  if (c < 0x80u) {
    return 1;
  }
  if ((c & 0xE0u) == 0xC0u) {
    return 2;
  }
  if ((c & 0xF0u) == 0xE0u) {
    return 3;
  }
  if ((c & 0xF8u) == 0xF0u) {
    return 4;
  }
  return 1; // invalid lead byte: copy it alone rather than hide it
}

void ccord_identity_set_name(char *dst, const char *src, size_t len)
{
  size_t cut = 0;
  if (dst && src && len > 0) {
    size_t limit = len < CCORD_IDENTITY_NAME_BYTES ? len : CCORD_IDENTITY_NAME_BYTES;
    size_t i = 0;
    size_t chars = 0;
    while (i < limit && chars < CCORD_IDENTITY_NAME_MAX) {
      size_t seq = utf8_seq_len((unsigned char)src[i]);
      if (i + seq > limit) {
        break; // never end in the middle of a UTF-8 sequence
      }
      i += seq;
      chars++;
    }
    cut = i;
    memcpy(dst, src, cut);
  }
  if (dst) {
    dst[cut] = '\0';
  }
}

bool ccord_identity_add_guild(ccord_identity *identity, uint64_t id, const char *name, size_t name_len, bool owner)
{
  if (!identity) {
    return false;
  }
  if (identity->guild_count >= CCORD_IDENTITY_MAX_GUILDS) {
    identity->guilds_truncated = true;
    return false;
  }
  ccord_identity_guild *guild = &identity->guilds[identity->guild_count];
  guild->id = id;
  guild->owner = owner;
  ccord_identity_set_name(guild->name, name, name_len);
  identity->guild_count++;
  return true;
}
