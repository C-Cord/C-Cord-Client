// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <stddef.h>

struct ccord_rest_queue;
// Internal zero-copy handoff for non-sensitive, unshared REST responses (callable only inside ccord_rest_callback).
char *ccord_rest_take_active_resp(struct ccord_rest_queue *q, size_t *out_len);
