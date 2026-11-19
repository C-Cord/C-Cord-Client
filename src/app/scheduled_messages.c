// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "scheduled_messages.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>
#include <ccord/model/model.h>
#include <ccord/store/store.h>

#define SM_FILE "scheduled_messages.json"
#define SM_MAX_FILE (1u << 20)
#define SM_RETRY_BASE_MS 10000      // first retry 10 s after a failure, doubling
#define SM_RETRY_MAX_MS (10 * 60 * 1000)

typedef struct sm_entry {
  uint64_t id;
  ccord_id channel_id;
  int64_t due_ms;
  char *text;                // ccord_malloc'd, NUL-terminated
  char **paths;              // absolute file paths sent with the message (ccord_malloc'd), or nullptr
  bool *spoilers;
  size_t npaths;
  size_t len;
  char error[128];
  bool missed;
  uint32_t attempts;
  int64_t retry_at_ms;       // not persisted: a fresh launch retries at once
} sm_entry;

static struct {
  ccord_session *session;
  ccord_store *store;
  sm_entry *items;           // sorted by due_ms, then id
  size_t count, cap;
  uint64_t next_id;
  bool loaded;
  ccord_id account;
  char path[CCORD_PATH_MAX];
  int64_t (*clock)(void *ud);
  void *clock_ud;
} g;

int64_t ccord_scheduled_messages_now_ms(void) { return g.clock ? g.clock(g.clock_ud) : ccord_clock_unix_ms(); }

void ccord_scheduled_messages_set_clock(int64_t (*now_unix_ms)(void *ud), void *ud)
{
  g.clock = now_unix_ms;
  g.clock_ud = ud;
}

const char *ccord_scheduled_messages_path(void) { return g.path; }

static void free_entry(sm_entry *e)
{
  ccord_free(e->text);
  for (size_t k = 0; k < e->npaths; k++) ccord_free(e->paths[k]);
  ccord_free(e->paths);
  ccord_free(e->spoilers);
  e->text = nullptr;
  e->paths = nullptr;
  e->spoilers = nullptr;
  e->npaths = 0;
}

static void clear_items(void)
{
  for (size_t i = 0; i < g.count; i++) free_entry(&g.items[i]);
  ccord_free(g.items);
  g.items = nullptr;
  g.count = g.cap = 0;
}

// ---------------------------------------------------------------- persistence

// mkdir -p, every created level 0700.
static bool make_dirs(const char *dir)
{
  ccord_error err = {0};
  return ccord_mkdir_p(dir, 0700, &err) == CCORD_OK;
}

// Serializes the queue and replaces the file atomically (write temp 0600, fsync, rename).
static bool save(void)
{
  if (!g.path[0]) return false;
  ccord_buf b;
  ccord_buf_init(&b, SM_MAX_FILE * 4u);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "version"); ccord_jw_int(&w, 1);
  ccord_jw_key(&w, "next_id"); ccord_jw_int(&w, (int64_t)g.next_id);
  ccord_jw_key(&w, "entries");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < g.count; i++) {
    const sm_entry *e = &g.items[i];
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_int(&w, (int64_t)e->id);
    ccord_jw_key(&w, "channel"); ccord_jw_id(&w, e->channel_id);
    ccord_jw_key(&w, "due"); ccord_jw_int(&w, e->due_ms);
    ccord_jw_key(&w, "text"); ccord_jw_string(&w, e->text, e->len);
    ccord_jw_key(&w, "error"); ccord_jw_cstr(&w, e->error);
    ccord_jw_key(&w, "missed"); ccord_jw_bool(&w, e->missed);
    if (e->npaths > 0) {
      ccord_jw_key(&w, "attachments");
      ccord_jw_begin_array(&w);
      for (size_t k = 0; k < e->npaths; k++) {
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "path"); ccord_jw_cstr(&w, e->paths[k]);
        ccord_jw_key(&w, "spoiler"); ccord_jw_bool(&w, e->spoilers[k]);
        ccord_jw_end_object(&w);
      }
      ccord_jw_end_array(&w);
    }
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  ccord_jw_end_object(&w);
  bool ok = ccord_jw_ok(&w);
  if (ok) {
    ccord_error werr = {0};
    ok = ccord_file_write_atomic(g.path, b.data, b.len, 0600, true, &werr) == CCORD_OK;
  }
  ccord_buf_free(&b);
  if (!ok) CCORD_LOG_W("scheduled: cannot write %s", g.path);
  return ok;
}

static void insert_sorted(sm_entry e, bool *ok)
{
  *ok = false;
  if (g.count == g.cap) {
    size_t cap = g.cap ? g.cap * 2 : 8;
    sm_entry *n = ccord_realloc_array(g.items, cap, sizeof *n);
    if (!n) return;
    g.items = n;
    g.cap = cap;
  }
  size_t i = g.count;
  while (i > 0 && (g.items[i - 1].due_ms > e.due_ms || (g.items[i - 1].due_ms == e.due_ms && g.items[i - 1].id > e.id))) {
    g.items[i] = g.items[i - 1];
    i--;
  }
  g.items[i] = e;
  g.count++;
  *ok = true;
}

static void remove_at(size_t i)
{
  free_entry(&g.items[i]);
  memmove(&g.items[i], &g.items[i + 1], (g.count - i - 1) * sizeof g.items[0]);
  g.count--;
}

static void load_file(int64_t now_ms)
{
  FILE *f = fopen(g.path, "rb");
  if (!f) return;   // no queue yet
  char *text = ccord_malloc(SM_MAX_FILE + 1u);
  size_t n = text ? fread(text, 1, SM_MAX_FILE, f) : 0;
  bool too_big = text && !feof(f);
  fclose(f);
  if (!text) return;
  ccord_arena arena;
  ccord_arena_init(&arena, 0);
  ccord_error err;
  const ccord_json *root = too_big ? nullptr : ccord_json_parse(&arena, text, n, nullptr, &err);
  const ccord_json *arr = root ? ccord_json_get(root, "entries") : nullptr;
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) {
    // Never overwrite what cannot be read: keep it aside so the user's text is not lost.
    char bad[CCORD_PATH_MAX + 16];
    if (snprintf(bad, sizeof bad, "%s.corrupt", g.path) < (int)sizeof bad) (void)ccord_file_rename_replace(g.path, bad);
    CCORD_LOG_W("scheduled: unreadable queue moved to %s", bad);
  } else {
    int64_t next = 1;
    (void)ccord_json_int64(ccord_json_get(root, "next_id"), &next);
    g.next_id = next > 0 ? (uint64_t)next : 1;
    for (size_t i = 0; i < ccord_json_array_len(arr) && g.count < CCORD_UI_SCHEDULED_MAX; i++) {
      const ccord_json *o = ccord_json_array_at(arr, i);
      int64_t id = 0, due = 0;
      size_t tl = 0, cl = 0, el = 0;
      const char *t = ccord_json_string(ccord_json_get(o, "text"), &tl);
      const char *c = ccord_json_string(ccord_json_get(o, "channel"), &cl);
      const char *er = ccord_json_string(ccord_json_get(o, "error"), &el);
      if (!t || !c || !ccord_json_int64(ccord_json_get(o, "id"), &id) || !ccord_json_int64(ccord_json_get(o, "due"), &due) ||
          id <= 0 || tl > CCORD_SEND_MAX_BYTES) continue;
      const ccord_json *att = ccord_json_get(o, "attachments");   // absent in queues written before attachments existed
      size_t na = att && ccord_json_type_of(att) == CCORD_JSON_ARRAY ? ccord_json_array_len(att) : 0;
      if (na > CCORD_RICH_MAX_ATTACHMENTS) na = CCORD_RICH_MAX_ATTACHMENTS;
      if (tl == 0 && na == 0) continue;
      sm_entry e = {.id = (uint64_t)id, .channel_id = strtoull(c, nullptr, 10), .due_ms = due, .len = tl};
      if (e.channel_id == CCORD_ID_NONE) continue;
      e.text = ccord_malloc(tl + 1u);
      if (!e.text) continue;
      memcpy(e.text, t, tl + 1u);
      if (na > 0) {
        e.paths = ccord_calloc(na, sizeof *e.paths);
        e.spoilers = ccord_calloc(na, sizeof *e.spoilers);
        if (!e.paths || !e.spoilers) {
          free_entry(&e);
          continue;
        }
        for (size_t k = 0; k < na; k++) {
          const ccord_json *ao = ccord_json_array_at(att, k);
          size_t pl = 0;
          const char *pp = ccord_json_string(ccord_json_get(ao, "path"), &pl);
          if (!pp || pl == 0) continue;
          e.paths[e.npaths] = ccord_malloc(pl + 1u);
          if (!e.paths[e.npaths]) continue;
          memcpy(e.paths[e.npaths], pp, pl + 1u);
          e.spoilers[e.npaths] = ccord_json_bool(ccord_json_get(ao, "spoiler"), false);
          e.npaths++;
        }
      }
      if (er) snprintf(e.error, sizeof e.error, "%s", er);
      e.missed = ccord_json_bool(ccord_json_get(o, "missed"), false) || due < now_ms - CCORD_SCHEDULED_MISSED_MS;
      bool ok;
      insert_sorted(e, &ok);
      if (!ok) free_entry(&e);
      if ((uint64_t)id >= g.next_id) g.next_id = (uint64_t)id + 1;
    }
  }
  ccord_arena_free(&arena);
  ccord_free(text);
}

// Loads the queue for the signed-in user once known; reloads when the account changed. False while the user is unknown.
static bool ensure_loaded(ccord_session *session, int64_t now_ms)
{
  if (!session) return false;
  ccord_id self = g.store ? ccord_store_self_id(g.store) : CCORD_ID_NONE;
  if (self == CCORD_ID_NONE) return false;
  if (g.loaded && g.account == self) return true;
  clear_items();
  g.next_id = 1;
  g.loaded = false;
  g.path[0] = '\0';
  g.account = self;
  const char *base = ccord_paths_data_dir();
  char dir[CCORD_PATH_MAX];
  if (!base || !base[0] || snprintf(dir, sizeof dir, "%s/accounts/%llu", base, (unsigned long long)self) >= (int)sizeof dir ||
      !make_dirs(dir) || snprintf(g.path, sizeof g.path, "%s/" SM_FILE, dir) >= (int)sizeof g.path) {
    g.path[0] = '\0';
    CCORD_LOG_W("scheduled: no data directory for the queue");
    return false;
  }
  load_file(now_ms);
  g.loaded = true;
  return true;
}

// ---------------------------------------------------------------- queue API

void ccord_scheduled_messages_init(ccord_session *session, ccord_store *store)
{
  clear_items();
  g.session = session;
  g.store = store;
  g.loaded = false;
  g.account = CCORD_ID_NONE;
  g.path[0] = '\0';
  g.next_id = 1;
  (void)ensure_loaded(session, ccord_scheduled_messages_now_ms());
}

void ccord_scheduled_messages_shutdown(void)
{
  clear_items();
  g.session = nullptr;
  g.store = nullptr;
  g.loaded = false;
  g.account = CCORD_ID_NONE;
  g.path[0] = '\0';
}

ccord_status ccord_scheduled_messages_add(ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                                          int64_t now_unix_ms, uint64_t *out_id, ccord_error *err)
{
  return ccord_scheduled_messages_add_files(channel_id, utf8, len, due_unix_ms, now_unix_ms, nullptr, nullptr, 0, out_id, err);
}

static bool is_abs_disk_path(const char *p)
{
  if (!p || !p[0]) return false;
  if (p[0] == '/') return true;
#if CCORD_OS_WINDOWS
  if ((p[0] == '\\' && p[1] == '\\') ||
      (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
       (p[2] == '\\' || p[2] == '/'))) {
    return true;
  }
#endif
  return false;
}

ccord_status ccord_scheduled_messages_add_files(ccord_id channel_id, const char *utf8, size_t len, int64_t due_unix_ms,
                                                int64_t now_unix_ms, const char *const *paths, const bool *spoilers,
                                                size_t npaths, uint64_t *out_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (npaths > CCORD_RICH_MAX_ATTACHMENTS) return ccord_error_set(err, CCORD_ERR_RANGE, "Too many attachments.");
  for (size_t k = 0; k < npaths; k++) {
    if (!paths || !is_abs_disk_path(paths[k])) return ccord_error_set(err, CCORD_ERR_INVALID, "Attachments must be files on disk.");
  }
  if (!utf8 || channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "Pick a channel first.");
  if (!ensure_loaded(g.session, now_unix_ms)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "Not signed in yet.");
  size_t first = 0;
  while (first < len && (utf8[first] == ' ' || utf8[first] == '\n' || utf8[first] == '\t' || utf8[first] == '\r')) first++;
  if (first == len && npaths == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "Write a message to schedule.");
  if (len > CCORD_SEND_MAX_BYTES) return ccord_error_set(err, CCORD_ERR_RANGE, "The message is too long.");
  if (memchr(utf8, 0, len) || !ccord_utf8_valid_text(utf8, len)) return ccord_error_set(err, CCORD_ERR_INVALID, "The message is not valid text.");
  if (due_unix_ms < now_unix_ms + CCORD_SCHEDULED_MIN_LEAD_MS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "Pick a time at least 1 minute from now.");
  }
  if (g.count >= CCORD_UI_SCHEDULED_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "Too many scheduled messages.");
  sm_entry e = {.id = g.next_id, .channel_id = channel_id, .due_ms = due_unix_ms, .len = len};
  e.text = ccord_malloc(len + 1u);
  if (!e.text) return ccord_error_set(err, CCORD_ERR_NOMEM, "Out of memory.");
  memcpy(e.text, utf8, len);
  e.text[len] = '\0';
  if (npaths > 0) {
    e.paths = ccord_calloc(npaths, sizeof *e.paths);
    e.spoilers = ccord_calloc(npaths, sizeof *e.spoilers);
    bool pok = e.paths && e.spoilers;
    for (size_t k = 0; pok && k < npaths; k++) {
      size_t pl = strlen(paths[k]);
      e.paths[k] = ccord_malloc(pl + 1u);
      if (!e.paths[k]) {
        pok = false;
        break;
      }
      memcpy(e.paths[k], paths[k], pl + 1u);
      e.spoilers[k] = spoilers && spoilers[k];
      e.npaths = k + 1u;
    }
    if (!pok) {
      free_entry(&e);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "Out of memory.");
    }
  }
  bool ok;
  insert_sorted(e, &ok);
  if (!ok) {
    free_entry(&e);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "Out of memory.");
  }
  g.next_id++;
  if (!save()) {
    for (size_t i = 0; i < g.count; i++) {
      if (g.items[i].id == e.id) {
        remove_at(i);
        break;
      }
    }
    return ccord_error_set(err, CCORD_ERR_IO, "Could not save the scheduled message.");
  }
  if (out_id) *out_id = e.id;
  return CCORD_OK;
}

// One line, cut on a character boundary, "..." when shortened.
static void make_preview(const sm_entry *e, char *out, size_t cap)
{
  size_t n = e->len;
  bool cut = false;
  if (n == 0) {
    snprintf(out, cap, "(attachments only)");
    return;
  }
  if (n > cap - 4) {
    n = cap - 4;
    while (n > 0 && ((unsigned char)e->text[n] & 0xC0u) == 0x80u) n--;
    cut = true;
  }
  memcpy(out, e->text, n);
  for (size_t i = 0; i < n; i++) {
    if (out[i] == '\n' || out[i] == '\r' || out[i] == '\t') out[i] = ' ';
  }
  if (cut) memcpy(out + n, "...", 3), n += 3;
  out[n] = '\0';
}

size_t ccord_scheduled_messages_list(ccord_ui_scheduled_item *out, size_t cap)
{
  if (!out) return g.count;
  size_t n = g.count < cap ? g.count : cap;
  for (size_t i = 0; i < n; i++) {
    const sm_entry *e = &g.items[i];
    out[i] = (ccord_ui_scheduled_item){.id = e->id, .channel_id = e->channel_id, .due_unix_ms = e->due_ms, .attachments = e->npaths, .missed = e->missed};
    make_preview(e, out[i].preview, sizeof out[i].preview);
    snprintf(out[i].error, sizeof out[i].error, "%s", e->error);
  }
  return n;
}

static size_t find(uint64_t id)
{
  for (size_t i = 0; i < g.count; i++) {
    if (g.items[i].id == id) return i;
  }
  return SIZE_MAX;
}

bool ccord_scheduled_messages_cancel(uint64_t id)
{
  size_t i = find(id);
  if (i == SIZE_MAX) return false;
  remove_at(i);
  (void)save();
  return true;
}

// Reads every file now; any that is missing, unreadable, empty or over the cap fails the whole send (naming the file),
// so a message never goes out without its files.
static ccord_status send_with_files(ccord_session *session, const sm_entry *e, ccord_error *err)
{
  ccord_upload_file up[CCORD_RICH_MAX_ATTACHMENTS] = {0};
  void *data[CCORD_RICH_MAX_ATTACHMENTS] = {0};
  ccord_status st = CCORD_OK;
  size_t n = e->npaths <= CCORD_RICH_MAX_ATTACHMENTS ? e->npaths : CCORD_RICH_MAX_ATTACHMENTS;
  for (size_t k = 0; k < n && st == CCORD_OK; k++) {
    const char *path = e->paths[k];
    const char *base = strrchr(path, '/');
#if CCORD_OS_WINDOWS
    const char *bs = strrchr(path, '\\');
    if (bs && (!base || bs > base)) base = bs;
#endif
    base = base ? base + 1 : path;
    uint64_t fsz = 0;
    FILE *f = nullptr;
    if (!ccord_path_is_file(path) || !ccord_file_size(path, &fsz)) {
      st = ccord_error_set(err, CCORD_ERR_NOT_FOUND, "Attachment %.80s is missing.", base);
    } else if (fsz == 0 || fsz > (uint64_t)(25u * 1024u * 1024u)) {
      st = ccord_error_set(err, CCORD_ERR_RANGE, "Attachment %.80s must be between 1 B and 25 MB.", base);
    } else if (!(f = fopen(path, "rb"))) {
      st = ccord_error_set(err, CCORD_ERR_IO, "Attachment %.80s cannot be read.", base);
    } else {
      data[k] = ccord_malloc((size_t)fsz);
      if (!data[k]) {
        st = ccord_error_set(err, CCORD_ERR_NOMEM, "Out of memory.");
      } else if (fread(data[k], 1, (size_t)fsz, f) != (size_t)fsz) {
        st = ccord_error_set(err, CCORD_ERR_IO, "Attachment %.80s cannot be read.", base);
      }
      up[k] = (ccord_upload_file){.filename = base, .data = data[k], .size = (size_t)fsz, .spoiler = e->spoilers[k]};
    }
    if (f) fclose(f);
  }
  if (st == CCORD_OK) {
    st = ccord_session_send_message_with_files(session, e->channel_id, CCORD_ID_NONE, false, e->text, e->len, up, n, err);
  }
  for (size_t k = 0; k < n; k++) ccord_free(data[k]);
  return st;
}

// Sends entry i through the session; removes it on success, records the error and the backoff on failure.
static ccord_status send_entry(ccord_session *session, size_t i, int64_t now_ms, ccord_error *err)
{
  sm_entry *e = &g.items[i];
  ccord_error serr;
  ccord_status st;
  if (e->npaths == 0) {
    st = ccord_session_send_message(session, e->channel_id, e->text, e->len, &serr);
  } else {
    st = send_with_files(session, e, &serr);
  }
  if (st == CCORD_OK) {
    remove_at(i);
    (void)save();
    return CCORD_OK;
  }
  snprintf(e->error, sizeof e->error, "%.127s", serr.message[0] ? serr.message : "Send failed");
  e->attempts++;
  int64_t back = SM_RETRY_BASE_MS;
  for (uint32_t k = 1; k < e->attempts && back < SM_RETRY_MAX_MS; k++) back *= 2;
  if (back > SM_RETRY_MAX_MS) back = SM_RETRY_MAX_MS;
  e->retry_at_ms = now_ms + back;
  (void)save();
  return ccord_error_set(err, st, "%s", e->error);
}

ccord_status ccord_scheduled_messages_send_now(uint64_t id, ccord_error *err)
{
  ccord_error_clear(err);
  size_t i = find(id);
  if (i == SIZE_MAX) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "That message is no longer scheduled.");
  if (!g.session) return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "Not connected.");
  return send_entry(g.session, i, ccord_scheduled_messages_now_ms(), err);
}

void ccord_scheduled_messages_tick(ccord_session *session, int64_t now_unix_ms)
{
  if (!session) return;
  g.session = session;
  if (!ensure_loaded(session, now_unix_ms)) return;
  if (g.count == 0 || ccord_session_state(session) != CCORD_CONN_CONNECTED) return;
  for (size_t i = 0; i < g.count; i++) {
    const sm_entry *e = &g.items[i];
    if (e->due_ms > now_unix_ms) break;   // sorted: nothing later is due
    if (e->missed || now_unix_ms < e->retry_at_ms) continue;
    ccord_error err;
    (void)send_entry(session, i, now_unix_ms, &err);
    break;   // one send per tick keeps a backlog from flooding the channel
  }
}
