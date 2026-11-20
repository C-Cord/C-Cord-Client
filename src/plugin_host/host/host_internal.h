// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

// Process-level hardening (NO_NEW_PRIVS, PDEATHSIG, RLIMIT_CORE=0) for out-of-process hosts.
void ccord_host_os_harden(void);

// OS-level containment (Linux Landlock + seccomp-bpf, macOS Seatbelt) for out-of-process hosts.
void ccord_host_os_sandbox(const char *plugin_dir);
