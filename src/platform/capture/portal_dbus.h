// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// XDG Desktop Portal ScreenCast negotiation (org.freedesktop.portal.ScreenCast).
//
// The protocol state machine (CreateSession -> SelectSources -> Start -> OpenPipeWireRemote,
// with org.freedesktop.portal.Request::Response signal handling) is transport-agnostic: it runs
// against an injectable ccord_portal_dbus_ops seam so unit tests can script replies, and against
// a real libdbus-1 session-bus backend when CCORD_HAVE_DBUS is set. All waits take an explicit
// timeout_ms so tests never block for seconds.
//
// Threading: a negotiator (and its bus seam) must be used from ONE thread at a time; the portal
// picker blocks, so call ccord_portal_dbus_start() from a worker thread, never the UI thread.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ccord/core/error.h>

#define CCORD_PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define CCORD_PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define CCORD_PORTAL_SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"
#define CCORD_PORTAL_REQUEST_IFACE "org.freedesktop.portal.Request"
#define CCORD_PORTAL_SESSION_IFACE "org.freedesktop.portal.Session"

// ---- DBus value tree (the seam's payload model) --------------------------------------------
// A small owned tree mirroring the DBus type system subset the portal uses. FD nodes own their
// descriptor (closed on free); the real backend hands each received message fd (a caller-owned
// dup from libdbus) to the tree, and the negotiator dup()s into ccord_portal_session.pw_fd, so
// every owner closes exactly once.

typedef enum ccord_pv_kind : uint8_t {
  CCORD_PV_U32 = 0,     // 'u'
  CCORD_PV_I32,         // 'i'
  CCORD_PV_BOOL,        // 'b'
  CCORD_PV_STRING,      // 's' / 'o' / 'g' (text payload)
  CCORD_PV_FD,          // 'h' (node owns the fd)
  CCORD_PV_VARIANT,     // 'v' — exactly one child
  CCORD_PV_STRUCT,      // '(...)' — ordered children
  CCORD_PV_DICT_ENTRY,  // '{sv}' — two children: string key, variant value
  CCORD_PV_ARRAY,       // 'a...' — ordered children
} ccord_pv_kind;

typedef struct ccord_pv ccord_pv;
struct ccord_pv {
  ccord_pv_kind kind;
  uint32_t u32;
  int32_t i32;
  bool boolean;
  int fd;             // CCORD_PV_FD only; -1 when unset
  char *text;         // CCORD_PV_STRING only (owned, never NULL after construction)
  bool object_path;   // CCORD_PV_STRING: wire type 'o' instead of 's'
  char *elem_sig;     // CCORD_PV_ARRAY only: element signature for empty arrays (owned, nullable)
  ccord_pv **kids;    // owned
  size_t nkids;
};

[[nodiscard]] ccord_pv *ccord_pv_new(ccord_pv_kind kind);
void ccord_pv_free(ccord_pv *v);   // NULL-safe; frees children, strings and owned fds
// Appends child to a container node. ALWAYS consumes child: on failure the child is freed, so
// the caller must never touch it again. Returns false on OOM or when parent is not a container
// (VARIANT accepts one child, DICT_ENTRY two).
bool ccord_pv_add(ccord_pv *parent, ccord_pv *child);
[[nodiscard]] ccord_pv *ccord_pv_u32(uint32_t v);
[[nodiscard]] ccord_pv *ccord_pv_i32(int32_t v);
[[nodiscard]] ccord_pv *ccord_pv_bool(bool v);
[[nodiscard]] ccord_pv *ccord_pv_string(const char *s);
[[nodiscard]] ccord_pv *ccord_pv_object_path(const char *s);   // STRING with wire type 'o'
[[nodiscard]] ccord_pv *ccord_pv_fd(int fd);                  // takes ownership of fd
[[nodiscard]] ccord_pv *ccord_pv_array(const char *elem_sig); // elem_sig for empty arrays, e.g. "{sv}"
// Looks up key in an a{sv} dict (ARRAY of DICT_ENTRY{STRING, VARIANT}); returns the unwrapped
// variant value, or NULL when absent or malformed.
const ccord_pv *ccord_pv_dict_get(const ccord_pv *dict, const char *key);

// ---- Injectable bus seam ---------------------------------------------------------------------

typedef struct ccord_portal_dbus_ops {
  void *ud;
  // Synchronous method call on object_path (the portal desktop object, or a session object for
  // Session.Close). `args` is a borrowed STRUCT of method arguments. Returns the reply body as an
  // owned STRUCT (caller frees), or NULL with err set.
  ccord_pv *(*call)(void *ud, const char *object_path, const char *iface, const char *method, const ccord_pv *args,
                    int timeout_ms, ccord_error *err);
  // Blocks until org.freedesktop.portal.Request::Response on request_path or timeout. Returns an
  // owned STRUCT{U32 response, ARRAY results} (caller frees), or NULL with err set
  // (CCORD_ERR_TIMEOUT when no signal arrives in time).
  ccord_pv *(*wait_response)(void *ud, const char *request_path, int timeout_ms, ccord_error *err);
  // Releases the connection; may be NULL. Called once by ccord_portal_dbus_destroy().
  void (*destroy)(void *ud);
} ccord_portal_dbus_ops;

typedef struct ccord_portal_dbus ccord_portal_dbus;

// Creates a negotiator over an injected seam (copied; the connection stays owned by the caller
// until destroy). On failure the seam is NOT destroyed. NULL + err on bad ops or OOM.
[[nodiscard]] ccord_portal_dbus *ccord_portal_dbus_create(const ccord_portal_dbus_ops *ops, ccord_error *err);
// Real libdbus-1 session-bus backend. NULL + err when libdbus is absent or the bus is unreachable.
[[nodiscard]] ccord_portal_dbus *ccord_portal_dbus_open_session_bus(ccord_error *err);
void ccord_portal_dbus_destroy(ccord_portal_dbus *d);

// ---- ScreenCast negotiation ------------------------------------------------------------------

// SelectSources "types" bitmask values.
typedef enum ccord_portal_source_type : uint8_t {
  CCORD_PORTAL_SOURCE_MONITOR = 1,
  CCORD_PORTAL_SOURCE_WINDOW = 2,
} ccord_portal_source_type;

// SelectSources "cursor_mode" values.
typedef enum ccord_portal_cursor_mode : uint8_t {
  CCORD_PORTAL_CURSOR_HIDDEN = 1,
  CCORD_PORTAL_CURSOR_EMBEDDED = 2,
  CCORD_PORTAL_CURSOR_METADATA = 4,
} ccord_portal_cursor_mode;

typedef struct ccord_portal_request {
  uint8_t types;                   // bitmask of ccord_portal_source_type (nonzero, known bits only)
  ccord_portal_cursor_mode cursor_mode;
  bool multiple;                   // allow selecting several sources
  char restore_token[128];         // previous persistence token ("" = none)
  char parent_window[64];          // Start parent_window identifier ("" = none)
} ccord_portal_request;

#define CCORD_PORTAL_STREAMS_MAX 4u

typedef struct ccord_portal_stream {
  uint32_t node_id;                // PipeWire stream node id
  uint32_t source_type;            // ccord_portal_source_type, 0 when not reported
  int width, height;               // 0 when not reported
  int x, y;                        // position, 0 when not reported
  char id[64];                     // portal stream id ("" when absent)
} ccord_portal_stream;

typedef struct ccord_portal_session {
  char handle[128];                // portal session object path
  char restore_token[128];         // fresh persistence token from Start ("" when absent)
  ccord_portal_stream streams[CCORD_PORTAL_STREAMS_MAX];
  size_t stream_count;
  int pw_fd;                       // dup'd PipeWire remote fd, owned; -1 until OpenPipeWireRemote
} ccord_portal_session;

// Runs CreateSession -> SelectSources -> Start -> OpenPipeWireRemote. timeout_ms bounds EACH
// request wait (method reply and Response signal). On success out holds the session and owns
// pw_fd. On failure any portal session already created is closed best-effort, out is zeroed
// (pw_fd = -1) and err describes the first failure. User cancellation maps to CCORD_ERR_PERMISSION.
[[nodiscard]] ccord_status ccord_portal_dbus_start(ccord_portal_dbus *d, const ccord_portal_request *req,
                                                   int timeout_ms, ccord_portal_session *out, ccord_error *err);
// Best-effort org.freedesktop.portal.Session.Close, then closes pw_fd and clears the session.
// NULL-safe and safe on zeroed sessions.
void ccord_portal_dbus_close_session(ccord_portal_dbus *d, ccord_portal_session *session, int timeout_ms);
