// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/os.h>

// Standalone out-of-process plugin host entrypoint (applies OS-level hardening + sandbox).
int ccord_plugin_host_main(int argc, char **argv);

// In-process worker-thread plugin host entrypoint (Android & mobile integration tests; does NOT sandbox caller process).
int ccord_plugin_host_run_fd(ccord_socket_t fd, const _Atomic bool *cancel);
