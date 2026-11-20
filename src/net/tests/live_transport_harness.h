// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "fake_discord.h"  // first: defines _POSIX_C_SOURCE / _DEFAULT_SOURCE

#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/alloc.h>
#include <ccord/core/utf8.h>
#include <ccord/model/rich.h>
#include <ccord/net/admin.h>
#include <ccord/net/discord_proto.h>
#include <ccord/net/live_transport.h>
#include <ccord/net/rtp_h264.h>
#include <ccord/net/voice_camera.h>
#include <ccord/net/voice_crypto.h>

#include <sodium.h>

#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ccord_test.h"

[[maybe_unused]] static bool make_temp(char *out, size_t cap)
{
  const char *base = getenv("TMPDIR");
  if (!base || base[0] == '\0') base = ".";
  int n = snprintf(out, cap, "%s/ccord-live-XXXXXX", base);
  if (n < 0 || (size_t)n >= cap) return false;
  return mkdtemp(out) != nullptr;
}


typedef struct hook_fx_cfg {
  char initial_proto_json[512];
  const fd_server *srv;   // the server forks inside fd_start, so its bound port (srv->port) is what the hook can see
} hook_fx_cfg;

