// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "host_internal.h"

#include <ccord/core/buf.h>
#include <ccord/core/config.h>
#include <ccord/core/os.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__linux__)
#include <signal.h>
#include <sys/prctl.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <sandbox.h>
#endif
#if !CCORD_OS_WINDOWS
#include <sys/resource.h>
#endif

// Process-level hardening. Empty environment and null stdin come from the spawning side.
void ccord_host_os_harden(void)
{
#if defined(__linux__)
  (void)prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  (void)prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
#endif
#if !CCORD_OS_WINDOWS
  struct rlimit core = {0, 0};
  (void)setrlimit(RLIMIT_CORE, &core);
#endif
}

// OS-level containment for out-of-process plugin hosts (Landlock + seccomp-bpf on Linux, Seatbelt on macOS).
// All fs.* APIs are brokered over IPC through the main process, so the plugin host only needs read-only
// access to its own plugin directory (for Lua require()) and zero network/write/exec capabilities.
void ccord_host_os_sandbox(const char *plugin_dir)
{
#if defined(__linux__) && !defined(__ANDROID__)
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    fprintf(stderr, "ccord-plugin-host: PR_SET_NO_NEW_PRIVS failed: %s\n", strerror(errno));
    _exit(126);
  }

  // 1. Landlock filesystem sandbox (ABI v1+): read-only access limited strictly to plugin_dir.
#ifdef SYS_landlock_create_ruleset
  {
    long abi = syscall(SYS_landlock_create_ruleset, nullptr, (size_t)0, (uint32_t)LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
      fprintf(stderr, "ccord-plugin-host: warning: Landlock unavailable (%s)\n", strerror(errno));
    } else {
      struct landlock_ruleset_attr attr = {0};
      attr.handled_access_fs =
        LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
      if (abi >= 2) attr.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
      if (abi >= 3) attr.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
      long rfd = syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, (uint32_t)0);
      if (rfd < 0) {
        fprintf(stderr, "ccord-plugin-host: warning: landlock_create_ruleset failed (%s)\n", strerror(errno));
      } else {
        if (plugin_dir && plugin_dir[0]) {
          int dfd = open(plugin_dir, O_PATH | O_CLOEXEC);
          if (dfd >= 0) {
            struct landlock_path_beneath_attr rule = {
              .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR,
              .parent_fd = dfd,
            };
            (void)syscall(SYS_landlock_add_rule, (int)rfd, LANDLOCK_RULE_PATH_BENEATH, &rule, (uint32_t)0);
            close(dfd);
          }
        }
#ifdef CCORD_SANITIZED
        // ASan/LSan reads /proc/self at process exit for leak reporting.
        {
          int pfd = open("/proc/self", O_PATH | O_CLOEXEC);
          if (pfd >= 0) {
            struct landlock_path_beneath_attr prule = {
              .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR,
              .parent_fd = pfd,
            };
            (void)syscall(SYS_landlock_add_rule, (int)rfd, LANDLOCK_RULE_PATH_BENEATH, &prule, (uint32_t)0);
            close(pfd);
          }
        }
#endif
        if (syscall(SYS_landlock_restrict_self, (int)rfd, (uint32_t)0) != 0) {
          fprintf(stderr, "ccord-plugin-host: warning: landlock_restrict_self failed (%s)\n", strerror(errno));
        }
        close((int)rfd);
      }
    }
  }
#endif

  // 2. Seccomp-BPF denylist: block network sockets, process exec/spawn, ptrace, namespaces, io_uring, and fs mutation syscalls.
#if defined(__x86_64__)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__i386__)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_I386
#elif defined(__arm__)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_ARM
#elif defined(__riscv) && (__riscv_xlen == 64)
#define CCORD_AUDIT_ARCH AUDIT_ARCH_RISCV64
#endif
#ifdef CCORD_AUDIT_ARCH
  {
    static const uint32_t k_deny_nr[] = {
#ifdef __NR_socket
      __NR_socket,
#endif
#ifdef __NR_socketpair
      __NR_socketpair,
#endif
#ifdef __NR_connect
      __NR_connect,
#endif
#ifdef __NR_bind
      __NR_bind,
#endif
#ifdef __NR_listen
      __NR_listen,
#endif
#ifdef __NR_accept
      __NR_accept,
#endif
#ifdef __NR_accept4
      __NR_accept4,
#endif
#ifdef __NR_execve
      __NR_execve,
#endif
#ifdef __NR_execveat
      __NR_execveat,
#endif
#if !defined(CCORD_SANITIZED)
#ifdef __NR_ptrace
      __NR_ptrace,
#endif
#endif
#ifdef __NR_process_vm_readv
      __NR_process_vm_readv,
#endif
#ifdef __NR_process_vm_writev
      __NR_process_vm_writev,
#endif
#ifdef __NR_fork
      __NR_fork,
#endif
#ifdef __NR_vfork
      __NR_vfork,
#endif
#ifdef __NR_clone3
      __NR_clone3,
#endif
#ifdef __NR_io_uring_setup
      __NR_io_uring_setup,
#endif
#ifdef __NR_io_uring_enter
      __NR_io_uring_enter,
#endif
#ifdef __NR_io_uring_register
      __NR_io_uring_register,
#endif
#ifdef __NR_memfd_create
      __NR_memfd_create,
#endif
#ifdef __NR_userfaultfd
      __NR_userfaultfd,
#endif
#ifdef __NR_unshare
      __NR_unshare,
#endif
#ifdef __NR_setns
      __NR_setns,
#endif
#ifdef __NR_mount
      __NR_mount,
#endif
#ifdef __NR_umount2
      __NR_umount2,
#endif
#ifdef __NR_chroot
      __NR_chroot,
#endif
#ifdef __NR_pivot_root
      __NR_pivot_root,
#endif
#ifdef __NR_keyctl
      __NR_keyctl,
#endif
#ifdef __NR_add_key
      __NR_add_key,
#endif
#ifdef __NR_request_key
      __NR_request_key,
#endif
#ifdef __NR_bpf
      __NR_bpf,
#endif
#ifdef __NR_perf_event_open
      __NR_perf_event_open,
#endif
#ifdef __NR_kexec_load
      __NR_kexec_load,
#endif
#ifdef __NR_init_module
      __NR_init_module,
#endif
#ifdef __NR_finit_module
      __NR_finit_module,
#endif
#ifdef __NR_open_by_handle_at
      __NR_open_by_handle_at,
#endif
#ifdef __NR_name_to_handle_at
      __NR_name_to_handle_at,
#endif
#ifdef __NR_mknod
      __NR_mknod,
#endif
#ifdef __NR_mknodat
      __NR_mknodat,
#endif
#ifdef __NR_unlink
      __NR_unlink,
#endif
#ifdef __NR_unlinkat
      __NR_unlinkat,
#endif
#ifdef __NR_rename
      __NR_rename,
#endif
#ifdef __NR_renameat
      __NR_renameat,
#endif
#ifdef __NR_renameat2
      __NR_renameat2,
#endif
#ifdef __NR_mkdir
      __NR_mkdir,
#endif
#ifdef __NR_mkdirat
      __NR_mkdirat,
#endif
#ifdef __NR_rmdir
      __NR_rmdir,
#endif
#ifdef __NR_chmod
      __NR_chmod,
#endif
#ifdef __NR_fchmod
      __NR_fchmod,
#endif
#ifdef __NR_fchmodat
      __NR_fchmodat,
#endif
#ifdef __NR_chown
      __NR_chown,
#endif
#ifdef __NR_fchown
      __NR_fchown,
#endif
#ifdef __NR_fchownat
      __NR_fchownat,
#endif
#ifdef __NR_symlink
      __NR_symlink,
#endif
#ifdef __NR_symlinkat
      __NR_symlinkat,
#endif
#ifdef __NR_link
      __NR_link,
#endif
#ifdef __NR_linkat
      __NR_linkat,
#endif
#ifdef __NR_creat
      __NR_creat,
#endif
#ifdef __NR_truncate
      __NR_truncate,
#endif
#ifdef __NR_ftruncate
      __NR_ftruncate,
#endif
    };
    const size_t ndeny = sizeof k_deny_nr / sizeof k_deny_nr[0];
    static_assert((sizeof k_deny_nr / sizeof k_deny_nr[0]) < 250u, "seccomp denylist exceeds uint8_t BPF jump range");
    struct sock_filter insns[6 + (sizeof k_deny_nr / sizeof k_deny_nr[0])];
    const size_t cap = sizeof insns / sizeof insns[0];
    size_t pc = 0;
    // Load arch and kill on mismatch.
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, arch));
    insns[pc++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)CCORD_AUDIT_ARCH, 1, 0);
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    // Load syscall number.
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, nr));
    for (size_t i = 0; i < ndeny && pc + 2u < cap; i++) {
      uint8_t jt = (uint8_t)(ndeny - i);   // jump to RET_ERRNO (after RET_ALLOW)
      insns[pc++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, k_deny_nr[i], jt, 0);
    }
    if (pc + 2u > cap) {
      fprintf(stderr, "ccord-plugin-host: seccomp program buffer overflow\n");
      _exit(126);
    }
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    insns[pc++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    struct sock_fprog prog = {.len = (unsigned short)pc, .filter = insns};
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
      int e = errno;
      fprintf(stderr, "ccord-plugin-host: PR_SET_SECCOMP failed: %s\n", strerror(e));
      if (e != ENOSYS) _exit(126);
    }
  }
#endif
#elif defined(__APPLE__)
  {
    char real_dir[PATH_MAX];
    const char *dir = nullptr;
    if (plugin_dir && plugin_dir[0]) {
      dir = realpath(plugin_dir, real_dir) ? real_dir : plugin_dir;
    }
    ccord_buf prof;
    ccord_buf_init(&prof, 8192);
    (void)ccord_buf_append_str(
      &prof,
      "(version 1)\n"
      "(deny default)\n"
      "(allow process-info-pidinfo (target self))\n"
      "(allow sysctl-read)\n"
      "(allow file-read* (subpath \"/usr/lib\") (subpath \"/System/Library\") (subpath \"/dev/urandom\"))\n");
    if (dir && dir[0] == '/') {
      bool safe = true;
      for (const unsigned char *p = (const unsigned char *)dir; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) {
          safe = false;
          break;
        }
      }
      if (safe) {
        (void)ccord_buf_append_str(&prof, "(allow file-read* (subpath \"");
        for (const char *p = dir; *p; p++) {
          if (*p == '\\' || *p == '"') (void)ccord_buf_append(&prof, "\\", 1);
          (void)ccord_buf_append(&prof, p, 1);
        }
        (void)ccord_buf_append_str(&prof, "\"))\n");
      }
    }
    if (!prof.failed) {
      char *errbuf = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      (void)sandbox_init(ccord_buf_cstr(&prof), 0u, &errbuf);
      if (errbuf) sandbox_free_error(errbuf);
#pragma clang diagnostic pop
    }
    ccord_buf_free(&prof);
  }
#elif defined(_WIN32)
  (void)plugin_dir;
  // 1. Apply Job Object restrictions (single process, full UI isolation) if not already restricted.
  {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
      JOBOBJECT_BASIC_LIMIT_INFORMATION bli = {0};
      bli.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
      bli.ActiveProcessLimit = 1;
      (void)SetInformationJobObject(job, JobObjectBasicLimitInformation, &bli, sizeof bli);

      JOBOBJECT_BASIC_UI_RESTRICTIONS uir = {0};
      uir.UIRestrictionsClass = JOB_OBJECT_UILIMIT_ALL;
      (void)SetInformationJobObject(job, JobObjectBasicUIRestrictions, &uir, sizeof uir);

      if (!AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
      }
    }
  }
  // 2. Disable all token privileges on self.
  {
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
      (void)AdjustTokenPrivileges(tok, TRUE, nullptr, 0, nullptr, nullptr);
      CloseHandle(tok);
    }
  }
#else
  (void)plugin_dir;
#endif
}
