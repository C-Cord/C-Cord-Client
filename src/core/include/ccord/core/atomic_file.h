// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/core/config.h>
#include <ccord/core/error.h>

// Crash-safe, permission-hardened file helpers for on-disk caches that hold account data.
// POSIX only. None of these functions follow a symlink at the final path component.

// Creates `path` and every missing parent. Each directory this call creates gets exactly `mode`
// (chmod after mkdir, so the umask cannot widen it); existing directories are left as they are.
// Fails with CCORD_ERR_IO if a component exists and is not a directory.
[[nodiscard]] ccord_status ccord_mkdir_p(const char *path, unsigned mode, ccord_error *err);

// Writes `data` to `<path>.tmp.<pid>.<n>` (O_CREAT|O_EXCL, `mode`, fchmod so the umask cannot widen it), optionally
// fsyncs, then renames over `path`. On any failure the temp file is removed and `path` keeps its old, complete
// content. With `sync` the parent directory is fsynced after the rename. The parent directory must exist.
[[nodiscard]] ccord_status ccord_file_write_atomic(const char *path, const void *data, size_t len, unsigned mode,
                                                   bool sync, ccord_error *err);

// Reads a whole file into a NUL-terminated heap buffer (free with ccord_free); *out_len excludes the NUL.
// Returns nullptr, with *err set, and leaves the file alone when: the path is a symlink, not a regular file,
// owned by another user, readable or writable by group/other ((st_mode & 077) != 0), or larger than `max` bytes.
[[nodiscard]] char *ccord_file_read_max(const char *path, size_t max, size_t *out_len, ccord_error *err);

// Recursively deletes `path` (a directory tree, or a single file/symlink). Symlinks are unlinked, never followed,
// so nothing outside the tree is touched. A missing path is success. Refuses "" and "/".
[[nodiscard]] ccord_status ccord_rmtree(const char *path, ccord_error *err);
