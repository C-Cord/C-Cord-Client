// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once

#include <ccord/web/web_view.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/log.h>
#include <ccord/core/alloc.h>

#include <quickjs.h>
#include <curl/curl.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_MAX_FRAMES         16u
#define WEB_MAX_TIMERS         256u
#define WEB_MAX_FETCHES        64u
#define WEB_MAX_POSTMSG        256u
#define WEB_MAX_BLOBS          64u
#define WEB_MAX_WORKERS        16u
#define WEB_DIAG_CAP           4096u
#define WEB_MAX_RESP_BYTES     (8u * 1024u * 1024u)
#if defined(__ANDROID__)
#define WEB_DEFAULT_UA \
  "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Mobile Safari/537.36"
#else
#define WEB_DEFAULT_UA \
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
#endif

typedef struct web_frame web_frame;
typedef struct web_node web_node;
typedef struct web_canvas web_canvas;
typedef struct web_image web_image;
typedef struct web_css_sheet web_css_sheet;
typedef struct web_fetch web_fetch;

// ---- Parsed URL ----

typedef struct web_url {
  char href[8512];
  char origin[256];
  char protocol[24];   // "https:"
  char host[192];      // "discord.com:443" or "discord.com"
  char hostname[160];  // "discord.com"
  char port[16];       // "" or "443"
  char pathname[2048]; // "/path"
  char search[2048];   // "?a=1" or ""
  char hash[4096];     // "#foo" or ""
} web_url;

bool web_url_parse(const char *input, const web_url *base, web_url *out);
web_url *web_url_alloc_parse(const char *input, const web_url *base);

// ---- Computed CSS style ----

typedef enum web_display : uint8_t {
  WEB_DISP_BLOCK = 0,
  WEB_DISP_INLINE,
  WEB_DISP_INLINE_BLOCK,
  WEB_DISP_FLEX,
  WEB_DISP_GRID,
  WEB_DISP_TABLE,
  WEB_DISP_TABLE_ROW,
  WEB_DISP_TABLE_CELL,
  WEB_DISP_NONE,
} web_display;

typedef enum web_position : uint8_t {
  WEB_POS_STATIC = 0,
  WEB_POS_RELATIVE,
  WEB_POS_ABSOLUTE,
  WEB_POS_FIXED,
} web_position;

typedef enum web_flex_dir : uint8_t {
  WEB_FLEX_ROW = 0,
  WEB_FLEX_COLUMN,
} web_flex_dir;

typedef enum web_align : uint8_t {
  WEB_ALIGN_START = 0,
  WEB_ALIGN_CENTER,
  WEB_ALIGN_END,
  WEB_ALIGN_STRETCH,
  WEB_ALIGN_SPACE_BETWEEN,
  WEB_ALIGN_SPACE_AROUND,
} web_align;

typedef struct web_style {
  web_display display;
  web_position position;
  web_flex_dir flex_dir;
  web_align justify_content;
  web_align align_items;
  uint8_t align_self;     // 0=auto, 1=start, 2=center, 3=end, 4=stretch
  uint8_t float_dir;      // 0=none, 1=left, 2=right
  uint8_t clear_dir;      // 0=none, 1=left, 2=right, 3=both
  bool is_inline_level;   // inline-flex / inline-grid / inline-table / inline / inline-block
  bool flex_wrap;
  bool flex_reverse;
  float flex_grow;
  int grid_cols;        // 0 = unset, >0 = fixed repeat(N, ...) column count
  float gap;

  bool has_width, has_height;
  bool width_pct, height_pct;
  bool width_fit_content;
  float width, height;
  float width_pct_val, height_pct_val;
  float min_w, min_h, max_w, max_h;
  bool border_box;

  bool has_top, has_left, has_right, has_bottom;
  bool top_pct, left_pct, right_pct, bottom_pct;
  float top, left, right, bottom;

  float margin[4];      // top, right, bottom, left
  bool margin_auto_l, margin_auto_r;
  float padding[4];
  float border_w[4];
  float border_radius;

  ccord_color bg;
  bool has_bg;
  ccord_color border_color;
  bool has_border_color;
  ccord_color color;
  bool has_color;

  ccord_color svg_fill, svg_stroke;
  bool has_svg_fill, has_svg_stroke, svg_fill_none, svg_stroke_none;
  float svg_stroke_w;

  float font_size;
  float line_height;
  bool font_bold;
  uint8_t text_align;     // 0=left, 1=center, 2=right
  uint8_t vertical_align; // 0=top, 1=middle, 2=bottom
  bool white_space_nowrap;
  bool white_space_pre;
  float opacity;
  bool hidden;          // visibility: hidden
  bool overflow_hidden;
  bool pointer_none;    // pointer-events: none
  bool has_z_index;
  int z_index;
  float translate_x, translate_y;
  bool translate_x_pct, translate_y_pct;
  float scale_x, scale_y;
  char bg_image_url[4096];
  bool bg_cover;
} web_style;

// ---- DOM node ----

typedef enum web_node_type : uint16_t {
  WEB_NODE_ELEMENT = 1,
  WEB_NODE_TEXT = 3,
  WEB_NODE_COMMENT = 8,
  WEB_NODE_DOCUMENT = 9,
  WEB_NODE_FRAGMENT = 11,
} web_node_type;

typedef struct web_attr {
  char *name;
  char *value;
} web_attr;

typedef struct web_style_prop {
  char *name;
  char *value;
} web_style_prop;

typedef struct web_listener {
  char *type;
  JSValue fn;
  bool capture;
  bool once;
  struct web_listener *next;
} web_listener;

struct web_image {
  char url[4096];
  char tex_key[96];
  uint8_t *rgba;
  char *svg_xml;      // non-null when decoded image is inline/data-URI SVG
  web_node *svg_root; // lazily parsed SVG element tree for vector painting
  int w, h;
  bool loading;
  bool loaded;
  bool failed;
  bool uploaded;
};

struct web_node {
  uint32_t id;
  web_frame *frame;
  web_node_type type;
  char *tag;          // lowercase for elements ("div", "iframe", "canvas", ...); "#text" / "#document"
  char *ns;           // optional namespace (SVG etc.)
  char *text;         // text content for TEXT/COMMENT nodes, or input value

  web_attr *attrs;
  size_t nattrs, attr_cap;

  web_style_prop *inline_props;
  size_t ninline, inline_cap;

  web_style_prop *css_vars;
  size_t nvars, var_cap;

  web_style computed;
  ccord_rect box;     // layout bounding rect in frame-local coordinates
  bool box_valid;

  web_node *parent;
  web_node *first_child;
  web_node *last_child;
  web_node *prev_sib;
  web_node *next_sib;

  web_listener *listeners;
  JSValue js_obj;     // cached JS wrapper (non-owning weak-ish ref; freed on frame teardown)
  bool has_js_obj;

  web_canvas *canvas; // non-null for <canvas>
  web_image *img;     // non-null for <img> or element with background-image
  web_frame *child_frame; // non-null for <iframe>
  bool script_executed;
  bool checked;       // for <input type="checkbox">
};

// ---- CSS stylesheet ----

typedef struct web_css_rule {
  char *selector;
  char *media_query;
  web_style_prop *props;
  size_t nprops;
  int specificity;
} web_css_rule;

struct web_css_sheet {
  web_css_rule *rules;
  size_t nrules, cap;
};

// ---- Timers, postMessage, storage, blobs, workers ----

typedef struct web_timer {
  uint32_t id;
  web_frame *frame;
  JSValue fn;
  char *code_str;
  uint64_t due_ms;
  uint32_t interval_ms;
  bool is_interval;
  bool is_raf;
  bool active;
} web_timer;

typedef struct web_postmsg {
  web_frame *target;
  web_frame *source;
  char *origin;
  char *json_payload;    // structured-clone JSON or primitive string
  bool is_raw_string;
  uint32_t port_id;      // 0 = window.postMessage; >0 = MessagePort delivery
  struct web_postmsg *next;
} web_postmsg;

typedef struct web_blob_entry {
  char url[128];
  char mime[64];
  uint8_t *data;
  size_t len;
} web_blob_entry;

typedef struct web_storage_kv {
  char *origin;
  char *key;
  char *val;
  struct web_storage_kv *next;
} web_storage_kv;

typedef struct web_mut_obs {
  web_frame *frame;
  JSValue cb;
  web_node *target;
  bool child_list;
  bool attributes;
  bool subtree;
  bool char_data;
  struct web_mut_obs *next;
} web_mut_obs;

typedef struct web_port {
  uint32_t id;
  uint32_t peer_id;
  web_frame *frame;
  JSValue js_obj;
  JSValue onmessage;
  web_listener *listeners;
  bool started;
  struct web_port *next;
} web_port;

// ---- Frame (browsing context / JS realm) ----

struct web_frame {
  uint32_t id;
  ccord_web_view *view;
  web_frame *parent;
  web_node *host_iframe; // parent's <iframe> node (null for root)
  bool is_root;
  bool is_worker;        // lightweight Worker realm (no DOM paint)

  JSContext *ctx;
  web_url url;
  web_node *doc;
  web_node *head;
  web_node *body;
  web_node *current_script;
  web_node *focused_node;
  web_node *hovered_node;

  web_css_sheet sheet;
  bool style_dirty;
  bool layout_dirty;
  float viewport_w, viewport_h;
  float scroll_y;
  bool dom_content_loaded;
  bool loaded;
  bool diag_dumped;
  bool diag_dumped_populated;
  bool resize_pending;
  uint32_t pending_scripts;
  uint32_t pending_subresources;
  uint32_t last_raf_err_hash;
  uint32_t last_raf_err_count;

  web_listener *win_listeners;
  JSValue win_obj;
  JSValue doc_obj;
  JSValue worker_host_obj; // if is_worker: parent's Worker object
  web_frame *worker_owner;

  uint32_t next_node_id;
  web_node **all_nodes;
  size_t all_nodes_len, all_nodes_cap;
};

// ---- Async HTTP fetch ----

typedef enum web_fetch_kind : uint8_t {
  WEB_FETCH_SCRIPT = 0,
  WEB_FETCH_STYLE,
  WEB_FETCH_IFRAME,
  WEB_FETCH_IMAGE,
  WEB_FETCH_JS_FETCH,
  WEB_FETCH_XHR,
  WEB_FETCH_WORKER,
  WEB_FETCH_TOP_DOC,
} web_fetch_kind;

typedef struct web_fetch_hdr {
  char *name;
  char *value;
} web_fetch_hdr;

struct web_fetch {
  web_fetch_kind kind;
  ccord_web_view *view;
  web_frame *frame;
  uint32_t frame_id;
  web_node *node;        // <script>, <link>, <iframe>, <img>, or null
  web_image *img;        // target image struct if WEB_FETCH_IMAGE
  char url[8512];
  char final_url[8512];
  char method[16];
  char *req_body;
  size_t req_body_len;
  struct curl_slist *req_headers;
  CURL *easy;
  ccord_buf resp_body;
  ccord_buf resp_headers;
  long http_status;
  bool failed;
  bool done;
  // JS callbacks (JS_UNDEFINED when unused)
  JSValue resolve_fn;
  JSValue reject_fn;
  JSValue xhr_obj;
  struct web_fetch *next;
};

// ---- Main web view ----

struct ccord_web_view {
  ccord_web_config cfg;
  char user_agent[256];
  uint64_t start_ms;
  uint64_t now_ms;
  bool dirty;
  bool destroying;

  JSRuntime *rt;
  JSClassID cls_node;
  JSClassID cls_canvas_ctx;
  JSClassID cls_xhr;
  JSClassID cls_port;
  JSClassID cls_mut_obs;
  JSClassID cls_worker;
  JSClassID cls_wasm_mod;
  JSClassID cls_wasm_inst;
  JSClassID cls_wasm_mem;

  web_frame *root;
  web_frame *frames[WEB_MAX_FRAMES];
  size_t nframes;
  uint32_t next_frame_id;

  CURLM *multi;
  CURLSH *cookie_share;
  web_fetch *fetches;
  size_t nfetches;

  web_timer timers[WEB_MAX_TIMERS];
  uint32_t next_timer_id;

  web_postmsg *msg_head;
  web_postmsg *msg_tail;
  uint32_t msg_count;

  web_port *ports;
  uint32_t next_port_id;

  web_mut_obs *mut_obs;

  web_blob_entry blobs[WEB_MAX_BLOBS];
  size_t nblobs;
  uint32_t next_blob_id;

  web_storage_kv *storage;

  // Pointer/drag & primary-finger touch tracking
  bool mouse_down;
  bool touch_down;
  bool touch_moved;
  bool touch_prevented;
  int64_t touch_finger;
  uint32_t touch_frame_id;
  uint32_t touch_node_id;
  float touch_start_x, touch_start_y;
  float last_mx, last_my;
  web_frame *captured_frame;
  web_node *captured_node;

  // Diagnostic ring buffer
  char diag[WEB_DIAG_CAP];
  size_t diag_len;

  // Active renderer pointer during draw (for text measurements in layout if available)
  ccord_renderer *measure_r;
};

// ---- Shared internal functions ----

// web_view.c
bool web_frame_alive(const ccord_web_view *v, uint32_t id);
CCORD_PRINTF(3, 4) void web_diag_append(ccord_web_view *v, const char *level, const char *fmt, ...);
void web_mark_dirty(ccord_web_view *v);
web_frame *web_frame_create(ccord_web_view *v, web_frame *parent, web_node *host_iframe,
                            const char *url, float w, float h, bool is_worker);
void web_frame_destroy(web_frame *f);
void web_drain_jobs(ccord_web_view *v);
uint32_t web_add_timer(web_frame *f, JSValue fn, const char *code, uint32_t delay_ms,
                       bool is_interval, bool is_raf);
void web_cancel_timer(ccord_web_view *v, uint32_t id);
void web_enqueue_postmsg(ccord_web_view *v, web_frame *target, web_frame *source,
                         const char *origin, const char *payload, bool is_raw_str, uint32_t port_id);
const char *web_blob_register(ccord_web_view *v, const uint8_t *data, size_t len, const char *mime);
const web_blob_entry *web_blob_find(const ccord_web_view *v, const char *url);
const char *web_storage_get(const ccord_web_view *v, const char *origin, const char *key);
void web_storage_set(ccord_web_view *v, const char *origin, const char *key, const char *val);
void web_storage_remove(ccord_web_view *v, const char *origin, const char *key);

// web_net.c
bool web_net_init(ccord_web_view *v);
void web_net_shutdown(ccord_web_view *v);
bool web_net_url_allowed(const ccord_web_view *v, const char *url);
bool web_net_decode_data_uri(const char *uri, uint8_t **out_bytes, size_t *out_len, char *out_mime, size_t mime_cap);
web_fetch *web_net_start(ccord_web_view *v, web_frame *f, web_fetch_kind kind, const char *url,
                         const char *method, const char *const *hdr_kv, size_t nhdrs,
                         const char *body, size_t body_len);
void web_net_pump(ccord_web_view *v);
void web_net_cancel_frame(ccord_web_view *v, web_frame *f);
void web_net_fetch_image(web_frame *f, web_node *node, web_image *img, const char *raw_url);

// web_dom.c
void web_dom_register_classes(ccord_web_view *v);
void web_dom_init_frame(web_frame *f);
web_node *web_node_create(web_frame *f, web_node_type type, const char *tag);
void web_node_free_all_in_frame(web_frame *f);
void web_node_append(web_node *parent, web_node *child);
void web_node_insert_before(web_node *parent, web_node *child, web_node *ref);
void web_node_remove(web_node *child);
const char *web_node_get_attr(const web_node *n, const char *name);
void web_node_set_attr(web_node *n, const char *name, const char *val);
void web_node_remove_attr(web_node *n, const char *name);
void web_node_set_inline_style(web_node *n, const char *prop, const char *val);
const char *web_node_get_inline_style(const web_node *n, const char *prop);
JSValue web_node_wrap(web_frame *f, web_node *n);
web_node *web_node_unwrap(JSContext *ctx, JSValueConst val);
bool web_dom_parse_document(web_frame *f, const char *html, size_t len);
bool web_dom_parse_fragment(web_frame *f, web_node *parent, const char *html, size_t len);
bool web_dom_parse_fragment_ex(web_frame *f, web_node *parent, const char *html, size_t len, bool clear_existing);
void web_dom_run_scripts(web_frame *f);
void web_dom_exec_script_code(web_frame *f, web_node *script_node, const char *code, size_t len, const char *filename);
void web_dom_on_fetch_done(web_fetch *ft);
void web_dom_dispatch_event(web_frame *f, web_node *target, JSValue ev_obj);
void web_dom_dispatch_win_event(web_frame *f, JSValue ev_obj);
void web_dom_dispatch_simple(web_frame *f, web_node *target, const char *type, bool bubbles);
void web_dom_fire_mutations(ccord_web_view *v, web_node *target, const char *type,
                            web_node *added, web_node *removed, const char *attr_name);
web_node *web_dom_query_one(web_node *root, const char *selector);
size_t web_dom_query_all(web_node *root, const char *selector, web_node **out, size_t cap);
bool web_dom_matches_selector(const web_node *n, const char *selector);

// web_js_env.c
void web_js_register_classes(ccord_web_view *v);
void web_js_init_frame(web_frame *f);
void web_js_sync_location(web_frame *f);
JSValue web_js_get_frame_proxy(web_frame *viewer, uint32_t target_frame_id);
void web_js_log_exception(web_frame *f, const char *where);
void web_js_format_stack(const char *raw, char *out, size_t cap);
JSValue web_js_make_event(JSContext *ctx, const char *type, bool bubbles, bool cancelable);
JSValue web_js_make_pointer_event(JSContext *ctx, const char *type, float cx, float cy,
                                  float ox, float oy, int button, int buttons, bool is_pointer);
JSValue web_js_make_pointer_event_ex(JSContext *ctx, const char *type, float cx, float cy,
                                     float ox, float oy, int button, int buttons,
                                     bool is_pointer, const char *pointer_type, int pointer_id);
JSValue web_js_make_touch_event(JSContext *ctx, const char *type, JSValueConst target_obj,
                                float cx, float cy, int pointer_id, bool is_active);
JSValue web_js_make_key_event(JSContext *ctx, const char *type, const char *key, const char *code, uint32_t keycode);
void web_js_deliver_postmsg(ccord_web_view *v, web_postmsg *m);
void web_js_on_fetch_done(web_fetch *ft);
void web_js_on_xhr_done(web_fetch *ft);
void web_js_on_worker_script_done(web_fetch *ft);

// web_wasm.c
void web_wasm_register_classes(ccord_web_view *v);
void web_wasm_install(web_frame *f);

// web_canvas.c
void web_canvas_register_classes(ccord_web_view *v);
web_canvas *web_canvas_create(int w, int h);
void web_canvas_destroy(web_canvas *c);
void web_canvas_resize(web_canvas *c, int w, int h);
int web_canvas_width(const web_canvas *c);
int web_canvas_height(const web_canvas *c);
JSValue web_canvas_get_context_2d(web_frame *f, web_node *node);
bool web_canvas_to_data_url(web_canvas *c, const char *mime, ccord_buf *out);
void web_canvas_paint(web_canvas *c, ccord_renderer *r, ccord_rect dst);

// web_layout_paint.c
void web_css_sheet_free(web_css_sheet *s);
void web_css_parse_sheet(web_css_sheet *s, const char *css, size_t len);
void web_css_parse_inline(web_node *n, const char *style_attr);
bool web_css_parse_color(const char *val, ccord_color *out);
void web_frame_ensure_layout(web_frame *f, ccord_renderer *r);
void web_frame_paint(web_frame *f, ccord_renderer *r, ccord_rect bounds);
void web_frame_dump_tree(web_frame *f);
bool web_view_dispatch_input(ccord_web_view *v, ccord_rect bounds, const ccord_input_event *ev);
