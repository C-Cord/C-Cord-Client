// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/ipc/ipc.h>
#include <ccord/ipc/perm.h>

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ccord_test.h"

static void put_frame_header(unsigned char h[4], uint32_t n)
{
  h[0] = (unsigned char)(n & 0xffu); h[1] = (unsigned char)((n >> 8) & 0xffu);
  h[2] = (unsigned char)((n >> 16) & 0xffu); h[3] = (unsigned char)((n >> 24) & 0xffu);
}

static void send_raw(int fd, const void *p, size_t n)
{
  ssize_t w = write(fd, p, n);
  CT_CHECK(w == (ssize_t)n);
}

int main(void)
{
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 1;
  ccord_error err = {0};
  ccord_ipc *a = ccord_ipc_open(sv[0], &err);
  ccord_ipc *b = ccord_ipc_open(sv[1], &err);
  CT_CHECK(a && b);

  // Round trip: request with params, then a notification, then an ok and an error response.
  CT_CHECK(ccord_ipc_send_request(a, 7, "clipboard.write", "{\"text\":\"123\"}", &err) == CCORD_OK);
  CT_CHECK(ccord_ipc_send_notify(a, "event", "{\"name\":\"channel.select\"}", &err) == CCORD_OK);
  CT_CHECK(ccord_ipc_send_ok(a, 7, nullptr, &err) == CCORD_OK);
  CT_CHECK(ccord_ipc_send_error(b, 9, "permission_denied", "clipboard.write is not in the manifest permissions", &err) == CCORD_OK);
  CT_CHECK(ccord_ipc_flush(a, 100, &err) == CCORD_OK);
  CT_CHECK(ccord_ipc_flush(b, 100, &err) == CCORD_OK);

  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_ipc_msg m;
  CT_CHECK(ccord_ipc_wait(b, &arena, &m, 1000, &err) == CCORD_OK);
  CT_CHECK(m.kind == CCORD_IPC_REQ && m.id == 7 && strcmp(m.method, "clipboard.write") == 0);
  size_t tl = 0;
  const char *text = m.params ? ccord_json_string(ccord_json_get(m.params, "text"), &tl) : nullptr;
  CT_CHECK(text && strcmp(text, "123") == 0);
  ccord_arena_reset(&arena);
  CT_CHECK(ccord_ipc_wait(b, &arena, &m, 1000, &err) == CCORD_OK);
  CT_CHECK(m.kind == CCORD_IPC_NTF && strcmp(m.method, "event") == 0);
  ccord_arena_reset(&arena);
  CT_CHECK(ccord_ipc_wait(b, &arena, &m, 1000, &err) == CCORD_OK);
  CT_CHECK(m.kind == CCORD_IPC_RES && m.ok && m.id == 7);
  ccord_arena_reset(&arena);
  CT_CHECK(ccord_ipc_wait(a, &arena, &m, 1000, &err) == CCORD_OK);
  CT_CHECK(m.kind == CCORD_IPC_RES && !m.ok && strcmp(m.err_code, "permission_denied") == 0);
  ccord_arena_reset(&arena);

  // Partial frame: header plus two bytes is EMPTY, not an error; the rest completes it.
  unsigned char hdr[4];
  const char *part = "{\"t\":\"ntf\",\"m\":\"partial\"}";
  put_frame_header(hdr, (uint32_t)strlen(part));
  send_raw(sv[0], hdr, 4);
  send_raw(sv[0], part, 2);
  CT_CHECK(ccord_ipc_try_read(b, &arena, &m, &err) == CCORD_IPC_READ_EMPTY);
  send_raw(sv[0], part + 2, strlen(part) - 2);
  CT_CHECK(ccord_ipc_try_read(b, &arena, &m, &err) == CCORD_IPC_READ_MSG);
  CT_CHECK(m.kind == CCORD_IPC_NTF && strcmp(m.method, "partial") == 0);
  ccord_arena_reset(&arena);

  // Unknown envelope type is a protocol error, never a message.
  const char *bad = "{\"t\":\"zzz\",\"id\":1}";
  put_frame_header(hdr, (uint32_t)strlen(bad));
  send_raw(sv[0], hdr, 4);
  send_raw(sv[0], bad, strlen(bad));
  CT_CHECK(ccord_ipc_try_read(b, &arena, &m, &err) == CCORD_IPC_READ_ERROR);
  CT_CHECK(err.code == CCORD_ERR_PROTOCOL);
  ccord_arena_reset(&arena);
  ccord_ipc_close(b);
  ccord_ipc_close(a);
  ccord_arena_free(&arena);

  // Oversize length header is refused before the payload arrives.
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
    ccord_ipc *c = ccord_ipc_open(sv[1], &err);
    put_frame_header(hdr, (1u << 20) + 1);
    send_raw(sv[0], hdr, 4);
    ccord_arena arena2;
    ccord_arena_init(&arena2, 0);
    ccord_ipc_msg m2;
    CT_CHECK(ccord_ipc_try_read(c, &arena2, &m2, &err) == CCORD_IPC_READ_ERROR);
    CT_CHECK(err.code == CCORD_ERR_PROTOCOL);
    ccord_arena_free(&arena2);
    ccord_ipc_close(c);
    close(sv[0]);
  }

  CT_CHECK(ccord_perm_from_name("clipboard.write", nullptr));
  CT_CHECK(!ccord_perm_from_name("unknown.perm", nullptr));
  ccord_perm p;
  CT_CHECK(ccord_perm_from_name("fs.read", &p) && p == CCORD_PERM_FS_READ && ccord_perm_class_of(p) == CCORD_PERM_CLASS_SYSTEM);
  char list[64];
  CT_CHECK(ccord_perm_list(CCORD_PERM_STORAGE | CCORD_PERM_COMMANDS, list, sizeof list) > 0);
  CT_CHECK(strcmp(list, "storage,commands") == 0);

  return CT_RESULT("test_ipc");
}
