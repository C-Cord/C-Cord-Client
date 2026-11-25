// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/web/web_view.h>
#include <ccord/core/alloc.h>
#include <ccord/core/base64.h>
#include <ccord/core/clock.h>
#include <ccord/core/paths.h>
#include <ccord/platform/platform.h>
#include <ccord/render/render.h>
#include <ccord/theme/theme.h>

#include "ccord_test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(long ms)
{
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  (void)nanosleep(&ts, nullptr);
}

typedef struct msg_capture {
  char kind[32];
  char payload[256];
  int count;
} msg_capture;

static void on_web_msg(void *ud, const char *kind, const char *payload)
{
  msg_capture *mc = ud;
  snprintf(mc->kind, sizeof mc->kind, "%s", kind ? kind : "");
  snprintf(mc->payload, sizeof mc->payload, "%s", payload ? payload : "");
  mc->count++;
}

static void test_dom_css_and_mutation(void)
{
  ccord_error err = {0};
  ccord_web_config cfg = {.viewport_w = 400, .viewport_h = 300, .dark_theme = true};
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);
  if (!v) return;

  static const char html[] =
    "<!doctype html><html><head><style>"
    "#box { width: 200px; height: 80px; background: #5865f2; }"
    ".item { color: rgb(255, 0, 128); }"
    "</style></head><body>"
    "<div id='box'><span class='item'>Hello</span></div>"
    "<script>"
    "window.__muts = 0;"
    "const box = document.getElementById('box');"
    "const obs = new MutationObserver(recs => { window.__muts += recs.length; });"
    "obs.observe(box, { childList: true, attributes: true });"
    "const p = document.createElement('p');"
    "p.className = 'item added';"
    "p.textContent = 'World';"
    "box.appendChild(p);"
    "box.setAttribute('data-ready', 'yes');"
    "window.__res = document.querySelectorAll('.item').length + ':' + box.dataset.ready + ':' + window.__muts;"
    "</script></body></html>";

  CT_CHECK(ccord_web_view_load_html(v, html, sizeof html - 1u, "https://discord.com/login", &err) == CCORD_OK);
  char out[128] = {0};
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__res", out, sizeof out));
  if (strcmp(out, "2:yes:2") != 0) fprintf(stderr, "DOM out='%s' diag:\n%s\n", out, ccord_web_view_diagnostics(v));
  CT_CHECK(strcmp(out, "2:yes:2") == 0);

  // Check getBoundingClientRect() returns real computed layout!
  CT_CHECK(ccord_web_view_eval_debug(v, "Math.round(document.getElementById('box').getBoundingClientRect().width) + 'x' + Math.round(document.getElementById('box').getBoundingClientRect().height)", out, sizeof out));
  CT_CHECK(strcmp(out, "200x80") == 0);

  ccord_web_view_destroy(v);
}

static void test_wasm_execution(void)
{
  ccord_error err = {0};
  ccord_web_config cfg = {.viewport_w = 300, .viewport_h = 200};
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);
  if (!v) return;

  // Minimal valid WASM module:
  //   (module
  //     (import "env" "mul2" (func $mul2 (param i32) (result i32)))
  //     (memory (export "memory") 1)
  //     (func (export "calc") (param i32 i32) (result i32)
  //       local.get 0  local.get 1  i32.add  call $mul2))
  static const char html[] =
    "<!doctype html><html><body><script>"
    "const wasmBytes = new Uint8Array(["
    "  0x00,0x61,0x73,0x6d, 0x01,0x00,0x00,0x00,"
    "  0x01,0x0c, 0x02,"
    "    0x60,0x01,0x7f, 0x01,0x7f,"
    "    0x60,0x02,0x7f,0x7f, 0x01,0x7f,"
    "  0x02,0x0c, 0x01, 0x03,0x65,0x6e,0x76, 0x04,0x6d,0x75,0x6c,0x32, 0x00,0x00,"
    "  0x03,0x02, 0x01, 0x01,"
    "  0x05,0x03, 0x01, 0x00,0x01,"
    "  0x07,0x11, 0x02,"
    "    0x06,0x6d,0x65,0x6d,0x6f,0x72,0x79, 0x02,0x00,"
    "    0x04,0x63,0x61,0x6c,0x63, 0x00,0x01,"
    "  0x0a,0x0b, 0x01, 0x09, 0x00, 0x20,0x00, 0x20,0x01, 0x6a, 0x10,0x00, 0x0b"
    "]);"
    "window.__wasmOut = 'pending';"
    "WebAssembly.instantiate(wasmBytes, { env: { mul2: x => x * 2 } }).then(({instance}) => {"
    "  const ans = instance.exports.calc(15, 6);"
    "  const mem = new Uint8Array(instance.exports.memory.buffer);"
    "  mem[0] = 99;"
    "  window.__wasmOut = WebAssembly.validate(wasmBytes) + ':' + ans + ':' + mem.length + ':' + mem[0];"
    "}).catch(e => { window.__wasmOut = 'ERR:' + e; });"
    "</script></body></html>";

  CT_CHECK(ccord_web_view_load_html(v, html, sizeof html - 1u, "https://discord.com/", &err) == CCORD_OK);
  ccord_web_view_pump(v, ccord_clock_ms());
  char out[128] = {0};
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__wasmOut", out, sizeof out));
  if (strcmp(out, "true:42:65536:99") != 0) fprintf(stderr, "WASM out='%s' diag:\n%s\n", out, ccord_web_view_diagnostics(v));
  CT_CHECK(strcmp(out, "true:42:65536:99") == 0);
  ccord_web_view_destroy(v);
}

static void test_canvas_and_crypto(void)
{
  ccord_error err = {0};
  ccord_web_config cfg = {.viewport_w = 300, .viewport_h = 200};
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);
  if (!v) return;

  static const char html[] =
    "<!doctype html><html><body><canvas id='c' width='32' height='32'></canvas><script>"
    "const c = document.getElementById('c');"
    "const ctx = c.getContext('2d');"
    "ctx.fillStyle = '#112233';"
    "ctx.fillRect(0, 0, 16, 16);"
    "ctx.fillStyle = '#ff0000';"
    "ctx.fillText('HI', 2, 26);"
    "const id = ctx.getImageData(4, 4, 1, 1).data;"
    "window.__px = id[0] + ',' + id[1] + ',' + id[2] + ',' + id[3];"
    "window.__png = c.toDataURL('image/png');"
    "window.__sha = 'pending';"
    "crypto.subtle.digest('SHA-256', new TextEncoder().encode('abc')).then(buf => {"
    "  window.__sha = [...new Uint8Array(buf)].slice(0, 4).map(b => b.toString(16).padStart(2, '0')).join('');"
    "});"
    "</script></body></html>";

  CT_CHECK(ccord_web_view_load_html(v, html, sizeof html - 1u, "https://discord.com/", &err) == CCORD_OK);
  ccord_web_view_pump(v, ccord_clock_ms());

  char px[64] = {0}, sha[64] = {0}, png_url[2048] = {0};
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__px", px, sizeof px));
  if (strcmp(px, "17,34,51,255") != 0) fprintf(stderr, "CANVAS px='%s' sha='%s' diag:\n%s\n", px, sha, ccord_web_view_diagnostics(v));
  CT_CHECK(strcmp(px, "17,34,51,255") == 0);
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__sha", sha, sizeof sha));
  // SHA-256("abc") starts with ba7816bf
  CT_CHECK(strcmp(sha, "ba7816bf") == 0);
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__png", png_url, sizeof png_url));
  CT_CHECK(strncmp(png_url, "data:image/png;base64,", 22) == 0);
  size_t plen = 0;
  uint8_t *png_bytes = ccord_base64_decode_alloc(png_url + 22, strlen(png_url + 22), &plen);
  CT_CHECK(png_bytes != nullptr && plen > 32 && png_bytes[0] == 137 && png_bytes[1] == 'P' &&
           png_bytes[2] == 'N' && png_bytes[3] == 'G');
  ccord_free(png_bytes);
  ccord_web_view_destroy(v);
}

// Local loopback HTTP fixture serving a synthetic cross-iframe interactive widget (no external network!)
typedef struct mock_http_srv {
  int ls;
  uint16_t port;
  volatile bool stop;
  pthread_t thr;
} mock_http_srv;

static void *mock_http_thread(void *arg)
{
  mock_http_srv *s = arg;
  while (!s->stop) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s->ls, &rfds);
    struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
    if (select(s->ls + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
    int cs = accept(s->ls, nullptr, nullptr);
    if (cs < 0) continue;
    char req[2048] = {0};
    ssize_t rn = recv(cs, req, sizeof req - 1u, 0);
    if (rn > 0) {
      const char *resp = nullptr;
      if (strncmp(req, "GET /widget.js ", 15) == 0) {
        resp =
          "HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\nConnection: close\r\n\r\n"
          "const s2 = document.createElement('script');"
          "document.head.appendChild(s2);"
          "s2.src = '/stage2.js';";
      } else if (strncmp(req, "GET /stage2.js ", 15) == 0) {
        resp =
          "HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\nConnection: close\r\n\r\n"
          "window.__widgetReady = function(){"
          "  const f = document.createElement('iframe');"
          "  f.style.width = '300px'; f.style.height = '120px';"
          "  f.src = '/frame.html#frame=checkbox&sitekey=test123';"
          "  document.getElementById('slot').appendChild(f);"
          "  window.__iframe = f;"
          "};"
          "window.addEventListener('message', e => {"
          "  const sameWin = (e.source === window.__iframe.contentWindow) && (window.__iframe.contentWindow === window.__iframe.contentWindow);"
          "  if (e.data && e.data.type === 'verified' && sameWin && e.data.hash === '#frame=checkbox&sitekey=test123') {"
          "    ccord.postMessage('solved', e.data.tok);"
          "  }"
          "});"
          "window.__widgetReady();";
      } else if (strncmp(req, "GET /frame.html ", 16) == 0) {
        resp =
          "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
          "<!doctype html><html><body style='margin:0;padding:10px;'>"
          "<button id='btn' style='width:200px;height:40px;'>Verify Me</button>"
          "<script>"
          "document.getElementById('btn').addEventListener('click', () => {"
          "  fetch('/api/verify', { method: 'POST', body: 'ok' }).then(r => r.json()).then(d => {"
          "    window.parent.postMessage({ type: 'verified', tok: d.token, hash: window.location.hash }, '*');"
          "  });"
          "});"
          "</script></body></html>";
      } else if (strncmp(req, "POST /api/verify ", 17) == 0) {
        resp =
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
          "{\"token\":\"synthetic_widget_token_ok\"}";
      } else if (strncmp(req, "GET /big_iframe.html ", 21) == 0) {
        // Generate a >1.05 MB self-contained HTML + inline JS document to stress Lexbor, QuickJS, WASM GC, and stack depth.
        static const char k_head[] =
          "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
          "<!doctype html><html><head><style>"
          "@media (prefers-color-scheme: dark) { #card { background: #2b2d31; color: #fff; } }"
          "#card { width: 300px; padding: 12px; }"
          "</style></head><body>"
          "<div id='card'><svg width='24' height='24' viewBox='0 0 24 24'>"
          "<path d='M4 12 L10 18 L20 6' stroke='#23a55a' stroke-width='3' fill='none'/>"
          "</svg><canvas id='cv' width='16' height='16'></canvas></div>"
          "<script>"
          "const __pad = '";
        static const char k_tail[] =
          "';\n"
          "// 1. Console & DOM exception hygiene with Symbol / null-proto objects\n"
          "console.log(Symbol('s'), Object.create(null));\n"
          "const card = document.getElementById('card');\n"
          "try { card.getAttribute(Symbol('x')); } catch(_) {}\n"
          "try { card.setAttribute(Symbol('x'), '1'); } catch(_) {}\n"
          "const g = document.getElementById('cv').getContext('2d');\n"
          "g.fillStyle = Object.create(null); g.fillStyle = '#5865f2'; g.fillRect(0, 0, 16, 16);\n"
          "// 2. DOM cycle prevention + DocumentFragment script activation\n"
          "const child = document.createElement('div'); card.appendChild(child); child.appendChild(card);\n"
          "let mutHits = 0; const mo = new MutationObserver(() => { mutHits++; if (mutHits < 20) card.setAttribute('data-m', String(mutHits)); });\n"
          "mo.observe(card, { attributes: true, childList: true }); card.setAttribute('data-m', '0');\n"
          "const frag = document.createDocumentFragment();\n"
          "const sc = document.createElement('script'); sc.textContent = 'window.__fragRan = true;'; frag.appendChild(sc);\n"
          "card.appendChild(frag);\n"
          "// 3. Deep recursion touching node.src and new URL(...) to verify small C stack frames\n"
          "function walk(d){ if (d <= 0) return new URL('/ok?d=0', location.href).pathname; const _ = card.parentNode; return walk(d - 1); }\n"
          "const wres = walk(60);\n"
          "// 4. WASM instance + exported memory + closure cycle\n"
          "const wb = new Uint8Array([0,97,115,109,1,0,0,0, 1,12,2, 96,1,127,1,127, 96,2,127,127,1,127,"
          " 2,12,1,3,101,110,118,4,109,117,108,50,0,0, 3,2,1,1, 5,3,1,0,1,"
          " 7,17,2,6,109,101,109,111,114,121,2,0, 4,99,97,108,99,0,1,"
          " 10,11,1,9,0,32,0,32,1,106,16,0,11]);\n"
          "let instRef = null;\n"
          "WebAssembly.instantiate(wb, { env: { mul2: x => (instRef ? x * 2 : x) } }).then(({instance}) => {\n"
          "  instRef = instance;\n"
          "  const mem = instance.exports.memory;\n"
          "  const ans = instance.exports.calc(10, 11);\n"
          "  new Uint8Array(mem.buffer)[7] = 123;\n"
          "  instRef = null;\n"
          "  const ok = __pad.length === 1048576 && window.__fragRan && wres === '/ok' && atob('SGVs\\n bG8=') === 'Hello' && ans === 42 && new Uint8Array(mem.buffer)[7] === 123 && location.hash.startsWith('#frame=challenge');\n"
          "  window.parent.postMessage({ type: 'big_ok', ok, mutHits }, '*');\n"
          "});\n"
          "</script></body></html>";
        size_t pad_len = 1024u * 1024u;
        size_t hlen = sizeof k_head - 1u, tlen = sizeof k_tail - 1u;
        char *big = ccord_malloc(hlen + pad_len + tlen);
        if (big) {
          memcpy(big, k_head, hlen);
          memset(big + hlen, 'A', pad_len);
          memcpy(big + hlen + pad_len, k_tail, tlen);
          size_t total = hlen + pad_len + tlen, off = 0;
          while (off < total) {
            ssize_t wn = send(cs, big + off, total - off, 0);
            if (wn <= 0) break;
            off += (size_t)wn;
          }
          ccord_free(big);
        }
        close(cs);
        continue;
      } else {
        resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nnot found";
      }
      (void)send(cs, resp, strlen(resp), 0);
    }
    close(cs);
  }
  return nullptr;
}

static void test_iframe_fetch_postmsg_and_sdl_click(void)
{
  mock_http_srv srv = {.ls = socket(AF_INET, SOCK_STREAM, 0)};
  CT_CHECK(srv.ls >= 0);
  if (srv.ls < 0) return;
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
  socklen_t alen = sizeof addr;
  CT_CHECK(bind(srv.ls, (struct sockaddr *)&addr, alen) == 0);
  CT_CHECK(getsockname(srv.ls, (struct sockaddr *)&addr, &alen) == 0);
  CT_CHECK(listen(srv.ls, 8) == 0);
  srv.port = ntohs(addr.sin_port);
  pthread_create(&srv.thr, nullptr, mock_http_thread, &srv);

  msg_capture mc = {0};
  ccord_error err = {0};
  ccord_web_config cfg = {
    .viewport_w = 360, .viewport_h = 220, .dark_theme = true,
    .allow_network = false, .allow_loopback_http = true,
    .on_msg = on_web_msg, .on_msg_ud = &mc,
  };
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);

  char base_url[128], html[512];
  snprintf(base_url, sizeof base_url, "http://127.0.0.1:%u/login", (unsigned)srv.port);
  snprintf(html, sizeof html,
           "<!doctype html><html><body style='margin:0;padding:0;'>"
           "<div id='slot'></div><script src='/widget.js'></script></body></html>");
  CT_CHECK(ccord_web_view_load_html(v, html, strlen(html), base_url, &err) == CCORD_OK);

  // Pump until widget.js and iframe /frame.html finish loading
  uint64_t t0 = ccord_clock_ms();
  while (ccord_clock_ms() - t0 < 3000u) {
    ccord_web_view_pump(v, ccord_clock_ms());
    if (ccord_web_view_pending_fetches(v) == 0) {
      char has_if[16] = {0};
      if (ccord_web_view_eval_debug(v, "document.querySelector('iframe') ? '1' : '0'", has_if, sizeof has_if) &&
          strcmp(has_if, "1") == 0) {
        // Give one more tick in case iframe fetch just started
        ccord_web_view_pump(v, ccord_clock_ms());
        if (ccord_web_view_pending_fetches(v) == 0) break;
      }
    }
    sleep_ms(5);
  }

  // Simulate user clicking the button inside the iframe at (60, 25)!
  ccord_rect bounds = {0, 0, 360, 220};
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .button = 1, .x = 60, .y = 25};
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .button = 1, .x = 60, .y = 25};
  CT_CHECK(ccord_web_view_handle_input(v, bounds, &dn));
  CT_CHECK(ccord_web_view_handle_input(v, bounds, &up));

  // Pump until POST /api/verify -> parent.postMessage -> ccord.postMessage('solved', ...) completes!
  t0 = ccord_clock_ms();
  while (mc.count == 0 && ccord_clock_ms() - t0 < 3000u) {
    ccord_web_view_pump(v, ccord_clock_ms());
    sleep_ms(5);
  }
  CT_CHECK(mc.count == 1);
  CT_CHECK(strcmp(mc.kind, "solved") == 0);
  CT_CHECK(strcmp(mc.payload, "synthetic_widget_token_ok") == 0);

  ccord_web_view_destroy(v);
  srv.stop = true;
  pthread_join(srv.thr, nullptr);
  close(srv.ls);
}

static void test_headless_sdl_render_and_worker(void)
{
  ccord_error err = {0};
  ccord_theme theme = {0};
  CT_CHECK(ccord_theme_load_builtin(&theme, "c-cord-dark", &err) == CCORD_OK);

  ccord_platform_config pc = {.title = "web-test", .width = 480, .height = 360, .headless = true};
  ccord_platform *p = ccord_platform_create(&pc, &err);
  CT_CHECK(p != nullptr);
  if (!p) return;
  ccord_renderer *r = ccord_render_create(p, &theme, &err);
  CT_CHECK(r != nullptr);
  if (!r) { ccord_platform_destroy(p); return; }

  ccord_web_config cfg = {.viewport_w = 400, .viewport_h = 300, .dark_theme = true};
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);

  static const char html[] =
    "<!doctype html><html><body style='margin:0;padding:12px;background:#1e1f22;'>"
    "<div style='display:grid;grid-template-columns:repeat(3,1fr);gap:6px;'>"
    "  <div style='height:40px;background:#5865f2;border-radius:4px;'>A</div>"
    "  <div style='height:40px;background:#23a55a;border-radius:4px;'>B</div>"
    "  <div style='height:40px;background:#da373c;border-radius:4px;'>C</div>"
    "</div>"
    "<canvas id='cv' width='64' height='32' style='margin-top:8px;'></canvas>"
    "<script>"
    "localStorage.setItem('k', 'stored_val');"
    "const g = document.getElementById('cv').getContext('2d');"
    "g.fillStyle = '#00ff88'; g.fillRect(0, 0, 64, 32);"
    "const blob = new Blob([\"self.onmessage = e => self.postMessage('echo:' + e.data);\"], {type:'application/javascript'});"
    "const w = new Worker(URL.createObjectURL(blob));"
    "window.__wres = 'pending';"
    "w.onmessage = e => { window.__wres = e.data + '|' + localStorage.getItem('k'); };"
    "setTimeout(() => w.postMessage('hello'), 5);"
    "</script></body></html>";

  CT_CHECK(ccord_web_view_load_html(v, html, sizeof html - 1u, "https://discord.com/login", &err) == CCORD_OK);
  uint64_t t0 = ccord_clock_ms();
  char wout[128] = {0};
  while (ccord_clock_ms() - t0 < 1500u) {
    ccord_web_view_pump(v, ccord_clock_ms());
    if (ccord_web_view_eval_debug(v, "window.__wres", wout, sizeof wout) &&
        strcmp(wout, "echo:hello|stored_val") == 0) {
      break;
    }
    sleep_ms(5);
  }
  CT_CHECK(strcmp(wout, "echo:hello|stored_val") == 0);

  // Paint the full DOM + CSS Grid + Canvas2D onto the real headless SDL3 renderer!
  ccord_render_begin(r, theme.c.bg_primary);
  ccord_web_view_draw(v, r, (ccord_rect){20, 20, 400, 300});
  // Verify actual rendered pixels are NOT black (check the #5865f2 tile + #00ff88 canvas)!
  const char *png_path = "/tmp/ccord_web_engine_render_test.png";
  CT_CHECK(ccord_render_screenshot(r, png_path, &err) == CCORD_OK);
  ccord_render_end(r);
  CT_CHECK(!ccord_web_view_needs_redraw(v));

  FILE *fp = fopen(png_path, "rb");
  CT_CHECK(fp != nullptr);
  if (fp) {
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *fbytes = fsz > 0 ? ccord_malloc((size_t)fsz) : nullptr;
    if (fbytes && fread(fbytes, 1, (size_t)fsz, fp) == (size_t)fsz) {
      uint8_t *rgba = nullptr;
      int rw = 0, rh = 0;
      CT_CHECK(ccord_image_decode_rgba(fbytes, (size_t)fsz, &rgba, &rw, &rh, &err) == CCORD_OK);
      if (rgba && rw == 480 && rh == 360) {
        // First grid tile is at (20+12, 20+12) = (32, 32), color #5865f2 (88, 101, 242)
        const uint8_t *p1 = rgba + ((size_t)45 * (size_t)rw + 50u) * 4u;
        CT_CHECK(p1[0] > 70 && p1[0] < 110 && p1[2] > 220);
        // Green canvas is below grid (~y = 20 + 12 + 40 + 8 + 10 = 90), color #00ff88
        const uint8_t *p2 = rgba + ((size_t)90 * (size_t)rw + 50u) * 4u;
        CT_CHECK(p2[1] > 230 && p2[2] > 110);
      }
      ccord_free(rgba);
    }
    ccord_free(fbytes);
    fclose(fp);
    (void)unlink(png_path);
  }

  ccord_web_view_destroy(v);
  ccord_render_destroy(r);
  ccord_platform_destroy(p);
}

static void test_web_platform_compat_and_rejection_tracking(void)
{
  ccord_error err = {0};
  ccord_web_config cfg = {.viewport_w = 400, .viewport_h = 300, .dark_theme = true};
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);
  if (!v) return;

  static const char html[] =
    "<!doctype html><html><head><style id='st'></style></head><body>"
    "<div id='d'><span>hi</span></div>"
    "<script>"
    "const d = document.getElementById('d');"
    "const st = document.getElementById('st');"
    "st.sheet.insertRule('#d { width: 123px; }', 0);"
    "const usp = new URLSearchParams('a=1&b=2&a=3');"
    "let pairs = []; usp.forEach((v, k) => pairs.push(k + '=' + v));"
    "const tz = Intl.DateTimeFormat().resolvedOptions().timeZone;"
    "window.__ioHit = false;"
    "new IntersectionObserver(es => { if (es[0] && es[0].isIntersecting) window.__ioHit = true; }).observe(d);"
    "window.__tArg = '';"
    "setTimeout((a, b) => { window.__tArg = a + '+' + b; }, 1, 'ok', 42);"
    "Promise.reject(new Error('intentional_test_rejection'));"
    "window.__compat = ["
    "  d instanceof HTMLElement,"
    "  d instanceof Element,"
    "  d instanceof Node,"
    "  document instanceof Document,"
    "  window instanceof Window,"
    "  document.defaultView === window,"
    "  d.firstElementChild && d.firstElementChild.tagName === 'SPAN',"
    "  Math.round(d.getBoundingClientRect().width) === 123,"
    "  pairs.join(',') === 'a=1,b=2,a=3',"
    "  typeof tz === 'string' && tz.length > 0"
    "].every(Boolean) ? 'ALL_OK' : 'FAIL';"
    "</script></body></html>";

  CT_CHECK(ccord_web_view_load_html(v, html, sizeof html - 1u, "https://discord.com/login", &err) == CCORD_OK);
  uint64_t t0 = ccord_clock_ms();
  while (ccord_clock_ms() - t0 < 500u) {
    ccord_web_view_pump(v, ccord_clock_ms());
    char io[16] = {0}, ta[32] = {0};
    if (ccord_web_view_eval_debug(v, "String(window.__ioHit)", io, sizeof io) &&
        ccord_web_view_eval_debug(v, "window.__tArg", ta, sizeof ta) &&
        strcmp(io, "true") == 0 && strcmp(ta, "ok+42") == 0) {
      break;
    }
    sleep_ms(5);
  }
  char out[64] = {0};
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__compat", out, sizeof out));
  CT_CHECK(strcmp(out, "ALL_OK") == 0);
  CT_CHECK(ccord_web_view_eval_debug(v, "String(window.__ioHit) + ':' + window.__tArg", out, sizeof out));
  CT_CHECK(strcmp(out, "true:ok+42") == 0);
  CT_CHECK(strstr(ccord_web_view_diagnostics(v), "intentional_test_rejection") != nullptr);
  ccord_web_view_destroy(v);
}

static void test_large_iframe_and_crash_hardening(void)
{
  mock_http_srv srv = {.ls = socket(AF_INET, SOCK_STREAM, 0)};
  CT_CHECK(srv.ls >= 0);
  if (srv.ls < 0) return;
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
  socklen_t alen = sizeof addr;
  CT_CHECK(bind(srv.ls, (struct sockaddr *)&addr, alen) == 0);
  CT_CHECK(getsockname(srv.ls, (struct sockaddr *)&addr, &alen) == 0);
  CT_CHECK(listen(srv.ls, 8) == 0);
  srv.port = ntohs(addr.sin_port);
  pthread_create(&srv.thr, nullptr, mock_http_thread, &srv);

  msg_capture mc = {0};
  ccord_error err = {0};
  ccord_web_config cfg = {
    .viewport_w = 420, .viewport_h = 540, .dark_theme = true,
    .allow_network = false, .allow_loopback_http = true,
    .on_msg = on_web_msg, .on_msg_ud = &mc,
  };
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);

  char base_url[128], html[1024];
  snprintf(base_url, sizeof base_url, "http://127.0.0.1:%u/host", (unsigned)srv.port);
  snprintf(html, sizeof html,
           "<!doctype html><html><body>"
           "<iframe id='big' width='360' height='460' src='/big_iframe.html#frame=challenge&id=stress1&host=discord.com'></iframe>"
           "<script>"
           "window.addEventListener('message', e => {"
           "  if (e.data && e.data.type === 'big_ok') ccord.postMessage('stress', e.data.ok ? 'PASS' : 'FAIL');"
           "});"
           "</script></body></html>");
  CT_CHECK(ccord_web_view_load_html(v, html, strlen(html), base_url, &err) == CCORD_OK);

  uint64_t t0 = ccord_clock_ms();
  while (mc.count == 0 && ccord_clock_ms() - t0 < 5000u) {
    ccord_web_view_pump(v, ccord_clock_ms());
    sleep_ms(5);
  }
  if (mc.count != 1 || strcmp(mc.payload, "PASS") != 0) {
    fprintf(stderr, "STRESS count=%d kind='%s' payload='%s' diag:\n%s\n",
            mc.count, mc.kind, mc.payload, ccord_web_view_diagnostics(v));
  }
  CT_CHECK(mc.count == 1);
  CT_CHECK(strcmp(mc.kind, "stress") == 0);
  CT_CHECK(strcmp(mc.payload, "PASS") == 0);

  ccord_web_view_destroy(v);
  srv.stop = true;
  pthread_join(srv.thr, nullptr);
  close(srv.ls);
}

static void test_css_layout_cascade_and_widget_paint(void)
{
  ccord_error err = {0};
  ccord_theme theme = {0};
  CT_CHECK(ccord_theme_load_builtin(&theme, "c-cord-dark", &err) == CCORD_OK);

  ccord_platform_config pc = {.title = "web-css-test", .width = 400, .height = 200, .headless = true};
  ccord_platform *p = ccord_platform_create(&pc, &err);
  CT_CHECK(p != nullptr);
  if (!p) return;
  ccord_renderer *r = ccord_render_create(p, &theme, &err);
  CT_CHECK(r != nullptr);
  if (!r) { ccord_platform_destroy(p); return; }

  ccord_web_config cfg = {.viewport_w = 360, .viewport_h = 160, .dark_theme = true};
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);

  // Synthetic 303x78 checkbox-style widget exercising CSS vars, @media dark/light, @supports,
  // specificity, :not(), style.innerHTML, display:table/table-cell, flex-row, absolute % centering,
  // transform: scale(0), and SVG path with A arc command.
  static const char html[] =
    "<!doctype html><html><head><style id='dyn'></style></head><body style='margin:0;'>"
    "<div id='card'>"
    "  <div id='chk-cell' class='cell'><div id='chk'></div><div id='hidden-scale'></div></div>"
    "  <div id='lbl-cell' class='cell'><span id='lbl'>\n  I am human  \n</span></div>"
    "  <div id='ico-cell' class='cell'><div id='svg-bg'></div></div>"
    "</div>"
    "<div id='z-top' style='position:absolute;top:100px;left:0;width:60px;height:40px;background:#22c55e;z-index:2;'></div>"
    "<div id='z-back' style='position:absolute;top:100px;left:0;width:60px;height:40px;background:#ef4444;z-index:1;'></div>"
    "<script>"
    "window.__zClicked = 'none';"
    "document.getElementById('z-top').addEventListener('click', () => { window.__zClicked = 'top'; });"
    "document.getElementById('z-back').addEventListener('click', () => { window.__zClicked = 'back'; });"
    "const st = document.getElementById('dyn');"
    "st.innerHTML = `"
    "  :root { --card-bg: #2a2d34; --chk-bg: #3b82f6; --op: 1; }"
    "  :unknown-pseudo { width: 10px; height: 10px; }"
    "  html, body { width: 30px; height: 30px; overflow: hidden; }"
    "  @media (prefers-color-scheme: light) { :root { --card-bg: #ff0000; } }"
    "  @media (max-width: 100px) { #card { width: 50px; } }"
    "  @supports (display: table) { #card { display: table; width: 303px; height: 78px; background: var(--card-bg); border: solid 1px #555; border-radius: 4px; } }"
    "  div { width: 100%; }" // lower specificity than #card / #chk!
    "  .cell { display: table-cell; vertical-align: middle; }"
    "  #chk-cell { width: 48px; padding-left: 12px; position: relative; }"
    "  #chk { width: 28px; height: 28px; background: var(--chk-bg); opacity: var(--op); border-radius: 4px; }"
    "  #chk:not(.checked) { border: 2px solid white; }"
    "  #hidden-scale { position: absolute; top: 0; left: 0; width: 48px; height: 78px; background: red; transform: scale(0); }"
    "  #lbl-cell { width: 180px; color: white; }"
    "  #ico-cell { width: 60px; }"
    "  #svg-bg { width: 28px; height: 28px; background-image: url(\"data:image/svg+xml,<svg viewBox='0 0 24 24'><rect width='24' height='24' fill='%2300d4ff'/></svg>\"); }"
    "`;"
    "</script></body></html>";

  CT_CHECK(ccord_web_view_load_html(v, html, sizeof html - 1u, "https://discord.com/", &err) == CCORD_OK);
  // Pump once so the immediate data:image/svg+xml background-image fetch completes
  ccord_web_view_pump(v, ccord_clock_ms());

  ccord_render_begin(r, theme.c.bg_primary);
  ccord_web_view_draw(v, r, (ccord_rect){10, 10, 360, 160});
  const char *png_path = "/tmp/ccord_web_css_widget_test.png";
  CT_CHECK(ccord_render_screenshot(r, png_path, &err) == CCORD_OK);
  ccord_render_end(r);

  // Verify layout boxes: #card is 303x78, cells are side-by-side horizontally!
  char out[128] = {0};
  CT_CHECK(ccord_web_view_eval_debug(v,
    "(() => {"
    "  const c = document.getElementById('card').getBoundingClientRect();"
    "  const k = document.getElementById('chk').getBoundingClientRect();"
    "  const l = document.getElementById('lbl-cell').getBoundingClientRect();"
    "  const i = document.getElementById('ico-cell').getBoundingClientRect();"
    "  return Math.round(c.width) + 'x' + Math.round(c.height) + ':' + Math.round(k.width) + ':' + (l.x > k.x && i.x > l.x && Math.abs(l.y - k.y) < 30);"
    "})()", out, sizeof out));
  if (strcmp(out, "303x78:28:true") != 0) fprintf(stderr, "CSS widget layout='%s'\n", out);
  CT_CHECK(strcmp(out, "303x78:28:true") == 0);

  // Verify rendered pixels: card bg is dark (#2a2d34, NOT clipped by html 30x30), checkbox is blue (#3b82f6), SVG bg is cyan (#00d4ff)
  FILE *fp = fopen(png_path, "rb");
  CT_CHECK(fp != nullptr);
  if (fp) {
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *fbytes = fsz > 0 ? ccord_malloc((size_t)fsz) : nullptr;
    if (fbytes && fread(fbytes, 1, (size_t)fsz, fp) == (size_t)fsz) {
      uint8_t *rgba = nullptr;
      int rw = 0, rh = 0;
      CT_CHECK(ccord_image_decode_rgba(fbytes, (size_t)fsz, &rgba, &rw, &rh, &err) == CCORD_OK);
      if (rgba && rw == 400 && rh == 200) {
        // Inside card background at (10+100, 10+12) = (110, 22): dark #2a2d34 (42, 45, 52), NOT red and NOT clipped
        const uint8_t *pbg = rgba + ((size_t)22 * (size_t)rw + 110u) * 4u;
        CT_CHECK(pbg[0] < 80 && pbg[1] < 80 && pbg[2] < 80);
        // Inside blue checkbox at (10+12+14, 10+39) = (36, 49): #3b82f6 (59, 130, 246)
        const uint8_t *pchk = rgba + ((size_t)49 * (size_t)rw + 36u) * 4u;
        CT_CHECK(pchk[2] > 210 && pchk[0] < 100);
        // Inside cyan SVG background-image in #ico-cell at (10+48+180+14, 10+39) = (252, 49): #00d4ff (0, 212, 255)
        const uint8_t *psvg = rgba + ((size_t)49 * (size_t)rw + 252u) * 4u;
        CT_CHECK(psvg[0] < 40 && psvg[1] > 180 && psvg[2] > 220);
        // Overlapping z-index test at (10+30, 10+120) = (40, 130): #z-top (z=2, green #22c55e) must paint above #z-back (z=1, red)
        const uint8_t *pz = rgba + ((size_t)130 * (size_t)rw + 40u) * 4u;
        CT_CHECK(pz[0] < 60 && pz[1] > 170 && pz[2] < 130);
      }
      ccord_free(rgba);
    }
    ccord_free(fbytes);
    fclose(fp);
    (void)unlink(png_path);
  }

  // Click overlapping z-index area (40, 130): #z-top (z=2) must win hit-testing over #z-back (z=1)
  ccord_rect vb = {10, 10, 360, 160};
  ccord_input_event dn = {.type = CCORD_IN_MOUSE_DOWN, .x = 40.0f, .y = 130.0f, .button = 1};
  ccord_input_event up = {.type = CCORD_IN_MOUSE_UP, .x = 40.0f, .y = 130.0f, .button = 1};
  CT_CHECK(ccord_web_view_handle_input(v, vb, &dn));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &up));
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__zClicked", out, sizeof out));
  CT_CHECK(strcmp(out, "top") == 0);

  // Viewport resize must update innerWidth/innerHeight and dispatch window 'resize' event
  CT_CHECK(ccord_web_view_eval_debug(v,
    "window.__resized = 'no'; window.addEventListener('resize', () => { window.__resized = window.innerWidth + 'x' + window.innerHeight; }); 'ok'",
    out, sizeof out));
  ccord_web_view_set_viewport(v, 440.0f, 520.0f);
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__resized", out, sizeof out));
  CT_CHECK(strcmp(out, "440x520") == 0);

  // Captcha state probe: hidden challenge iframe reports challenge_open=false; unhidden reports true + height
  CT_CHECK(ccord_web_view_eval_debug(v,
    "(() => {"
    "  const wrap = document.createElement('div'); wrap.id = 'ch-wrap';"
    "  wrap.style.position = 'fixed'; wrap.style.top = '0'; wrap.style.left = '0';"
    "  wrap.style.visibility = 'hidden'; wrap.style.opacity = '0';"
    "  const fr = document.createElement('iframe');"
    "  fr.style.width = '400px'; fr.style.height = '490px';"
    "  fr.src = 'data:text/html,<html><body><div>challenge</div></body></html>#frame=challenge';"
    "  wrap.appendChild(fr); document.body.appendChild(wrap); return 'ok';"
    "})()", out, sizeof out));
  ccord_web_view_pump(v, ccord_clock_ms());
  ccord_web_captcha_state cs = {0};
  ccord_web_view_captcha_state(v, &cs);
  CT_CHECK(!cs.challenge_open);
  CT_CHECK(ccord_web_view_eval_debug(v,
    "(() => { const w = document.getElementById('ch-wrap'); w.style.visibility = 'visible'; w.style.opacity = '1'; return 'ok'; })()",
    out, sizeof out));
  ccord_web_view_captcha_state(v, &cs);
  CT_CHECK(cs.challenge_open && cs.challenge_h >= 480.0f);

  // Verify Canvas2D curves/roundRect/Path2D/isPointInPath/OffscreenCanvas/DOMMatrix/decode/animate + high z-index clamp
  CT_CHECK(ccord_web_view_eval_debug(v,
    "(() => {"
    "  const c = document.createElement('canvas'); c.width = 100; c.height = 100;"
    "  const ctx = c.getContext('2d');"
    "  const p = new Path2D(); p.roundRect(10, 10, 40, 40, 6);"
    "  ctx.fillStyle = '#00ff00'; ctx.fill(p);"
    "  const hitIn = ctx.isPointInPath(p, 25, 25), hitOut = ctx.isPointInPath(p, 80, 80);"
    "  ctx.beginPath(); ctx.moveTo(0,0); ctx.bezierCurveTo(10,20,30,40,50,50); ctx.quadraticCurveTo(60,10,80,80); ctx.stroke();"
    "  ctx.setLineDash([4, 2]); const dl = ctx.getLineDash().length;"
    "  const g = ctx.createLinearGradient(0,0,10,10); g.addColorStop(0,'#fff'); ctx.fillStyle = g;"
    "  ctx.resetTransform(); const m = ctx.getTransform();"
    "  const oc = new OffscreenCanvas(32, 32); const octx = oc.getContext('2d'); octx.fillRect(0,0,32,32);"
    "  ctx.drawImage(oc.transferToImageBitmap(), 0, 0);"
    "  const img = new Image(); const hasDec = typeof img.decode === 'function';"
    "  const anim = c.animate([], {duration:100}); const hasAnim = anim && typeof anim.cancel === 'function';"
    "  const zd = document.createElement('div'); zd.style.zIndex = '2978832385'; document.body.appendChild(zd);"
    "  const zi = parseInt(getComputedStyle(zd).zIndex, 10);"
    "  return [hitIn, !hitOut, dl === 2, m.a === 1, hasDec, hasAnim, zi === 2147483647].every(Boolean) ? 'ok' : 'fail:' + [hitIn,hitOut,dl,m.a,hasDec,hasAnim,zi].join(',');"
    "})()", out, sizeof out));
  if (strcmp(out, "ok") != 0) fprintf(stderr, "Canvas2D/DOM compat check='%s'\n", out);
  CT_CHECK(strcmp(out, "ok") == 0);

  ccord_web_view_destroy(v);
  ccord_render_destroy(r);
  ccord_platform_destroy(p);
}

static void test_challenge_footer_layout_and_stacking_click(void)
{
  ccord_error err = {0};
  ccord_web_config cfg = {.viewport_w = 464, .viewport_h = 520, .dark_theme = true};
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);
  if (!v) return;

  // Synthetic layout fixture mirroring challenge popup wrapper, positioned footer Verify button
  // overlapped in DOM order by static .interface-user, float:left/right band, position:relative
  // offset, fixed translateX(-50%) centering, and flex margin-left:auto.
  static const char html[] =
    "<!doctype html><html><head><style>"
    "html,body{margin:0;padding:0;width:100%;height:100%;}"
    "#popup{position:absolute;top:10px;left:50px;border:1px solid #ccc;background:#fff;}"
    "#popup-in{position:relative;}"
    "#frame-box{width:360px;height:180px;position:relative;background:#fff;}"
    "#if-ch{}"
    "#verify{position:absolute;right:12px;bottom:10px;min-width:92px;height:36px;padding:0 16px;background:#00838f;color:#fff;}"
    "#if-user{margin-top:120px;height:60px;}"
    "#menu-btn{float:left;width:36px;height:36px;margin-left:12px;background:#333;}"
    "#refresh-btn{float:left;width:36px;height:36px;margin-left:8px;background:#333;}"
    "#logo{position:absolute;left:50%;bottom:10px;width:36px;height:36px;transform:translateX(-50%);background:#00d4ff;}"
    "#float-row{width:360px;margin-top:220px;}"
    "#fl-l{float:left;width:44px;height:30px;}"
    "#fl-r{float:right;min-width:84px;height:30px;}"
    "#rel-a{position:relative;left:10px;top:8px;width:50px;height:20px;}"
    "#rel-b{width:50px;height:20px;}"
    "#flex-auto{display:flex;width:300px;height:24px;}"
    "#fa-l{width:40px;height:20px;}"
    "#fa-r{margin-left:auto;width:60px;height:20px;}"
    "</style></head><body>"
    "<div id='popup'><div id='popup-in'><div id='frame-box'>"
    "  <div id='if-ch'><div id='verify'><div class='text'>Verify</div></div></div>"
    "  <div id='if-user'><div id='menu-btn'></div><div id='refresh-btn'></div><div id='logo'></div></div>"
    "</div></div></div>"
    "<div id='float-row'><div id='fl-l'>L</div><div id='fl-r'>Submit</div></div>"
    "<div id='rel-a'>A</div><div id='rel-b'>B</div>"
    "<div id='flex-auto'><div id='fa-l'>L</div><div id='fa-r'>R</div></div>"
    "<script>"
    "window.__hits = [];"
    "document.getElementById('verify').addEventListener('click', () => window.__hits.push('verify'));"
    "document.getElementById('if-user').addEventListener('click', () => window.__hits.push('user'));"
    "document.getElementById('menu-btn').addEventListener('click', () => window.__hits.push('menu'));"
    "document.getElementById('fl-r').addEventListener('click', () => window.__hits.push('fl-r'));"
    "</script></body></html>";

  CT_CHECK(ccord_web_view_load_html(v, html, sizeof html - 1u, "https://discord.com/", &err) == CCORD_OK);

  char out[256] = {0};
  CT_CHECK(ccord_web_view_eval_debug(v,
    "(() => {"
    "  const p = document.getElementById('popup').getBoundingClientRect();"
    "  const fb = document.getElementById('frame-box').getBoundingClientRect();"
    "  const vr = document.getElementById('verify').getBoundingClientRect();"
    "  const lg = document.getElementById('logo').getBoundingClientRect();"
    "  const fl = document.getElementById('fl-l').getBoundingClientRect();"
    "  const fr = document.getElementById('fl-r').getBoundingClientRect();"
    "  const ra = document.getElementById('rel-a').getBoundingClientRect();"
    "  const rb = document.getElementById('rel-b').getBoundingClientRect();"
    "  const far = document.getElementById('fa-r').getBoundingClientRect();"
    "  const checks = ["
    "    Math.round(p.width) === 362,"
    "    vr.width >= 90 && vr.width <= 120,"
    "    Math.round(vr.right - fb.left) === 348,"
    "    Math.abs((lg.left + lg.width / 2) - (fb.left + fb.width / 2)) <= 1.5,"
    "    vr.left > lg.right + 10,"
    "    Math.abs(fl.top - fr.top) <= 1 && fr.left > fl.right && Math.round(fr.right) === 360,"
    "    Math.round(ra.left) === 10 && Math.round(rb.top - (ra.top - 8)) === 20,"
    "    Math.round(far.right) === 300"
    "  ];"
    "  return checks.every(Boolean) ? 'ok' : 'fail:' + JSON.stringify({checks,p,fb,vr,lg,fl,fr,ra,rb,far});"
    "})()", out, sizeof out));
  if (strcmp(out, "ok") != 0) fprintf(stderr, "Challenge footer layout check: %s\n", out);
  CT_CHECK(strcmp(out, "ok") == 0);

  // Click #verify at center of its right-aligned box (must NOT be stolen by later static sibling #if-user!)
  CT_CHECK(ccord_web_view_eval_debug(v,
    "(() => { const r = document.getElementById('verify').getBoundingClientRect(); return Math.round(r.left + r.width/2) + ',' + Math.round(r.top + r.height/2); })()",
    out, sizeof out));
  float vx = 0, vy = 0;
  CT_CHECK(sscanf(out, "%f,%f", &vx, &vy) == 2);
  ccord_rect vb = {0, 0, 464, 520};
  ccord_input_event dn1 = {.type = CCORD_IN_MOUSE_DOWN, .x = vx, .y = vy, .button = 1};
  ccord_input_event up1 = {.type = CCORD_IN_MOUSE_UP, .x = vx, .y = vy, .button = 1};
  CT_CHECK(ccord_web_view_handle_input(v, vb, &dn1));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &up1));

  // Click #menu-btn on the left and #fl-r floated right
  CT_CHECK(ccord_web_view_eval_debug(v,
    "(() => { const m = document.getElementById('menu-btn').getBoundingClientRect(); const f = document.getElementById('fl-r').getBoundingClientRect();"
    "  return Math.round(m.left + m.width/2) + ',' + Math.round(m.top + m.height/2) + ',' + Math.round(f.left + f.width/2) + ',' + Math.round(f.top + f.height/2); })()",
    out, sizeof out));
  float mx = 0, my = 0, fx = 0, fy = 0;
  CT_CHECK(sscanf(out, "%f,%f,%f,%f", &mx, &my, &fx, &fy) == 4);
  ccord_input_event dn2 = {.type = CCORD_IN_MOUSE_DOWN, .x = mx, .y = my, .button = 1};
  ccord_input_event up2 = {.type = CCORD_IN_MOUSE_UP, .x = mx, .y = my, .button = 1};
  ccord_input_event dn3 = {.type = CCORD_IN_MOUSE_DOWN, .x = fx, .y = fy, .button = 1};
  ccord_input_event up3 = {.type = CCORD_IN_MOUSE_UP, .x = fx, .y = fy, .button = 1};
  CT_CHECK(ccord_web_view_handle_input(v, vb, &dn2));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &up2));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &dn3));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &up3));

  CT_CHECK(ccord_web_view_eval_debug(v, "window.__hits.join(',')", out, sizeof out));
  if (strcmp(out, "verify,menu,user,fl-r") != 0) fprintf(stderr, "Challenge footer hits='%s'\n", out);
  CT_CHECK(strcmp(out, "verify,menu,user,fl-r") == 0);

  ccord_web_view_destroy(v);
}

static void test_mobile_touch_events_and_iframe_drag(void)
{
  ccord_error err = {0};
  ccord_web_config cfg = {.viewport_w = 350, .viewport_h = 480, .dark_theme = true, .touch_enabled = true};
  ccord_web_view *v = ccord_web_view_create(&cfg, &err);
  CT_CHECK(v != nullptr);
  if (!v) return;

  static const char html[] =
    "<!doctype html><html><head><style>"
    "html,body{margin:0;padding:0;width:100%;height:100%;}"
    "#btn{position:absolute;left:220px;top:400px;width:100px;height:40px;background:#00838f;}"
    "#drag{position:absolute;left:20px;top:20px;width:160px;height:120px;background:#333;}"
    "#other{position:absolute;left:200px;top:20px;width:120px;height:120px;background:#555;}"
    "</style></head><body>"
    "<div id='drag'></div><div id='other'></div><div id='btn'>Verify</div>"
    "<script>"
    "window.__btnEv = []; window.__dragEv = []; window.__otherEv = [];"
    "const b = document.getElementById('btn');"
    "for (const t of ['pointerdown','touchstart','mousedown','pointerup','touchend','mouseup','click']) {"
    "  b.addEventListener(t, e => {"
    "    if (t === 'pointerdown') window.__btnEv.push(t + ':' + e.pointerType + ':' + (e.pointerId >= 2));"
    "    else if (t === 'touchstart' || t === 'touchend') window.__btnEv.push(t + ':' + e.touches.length + '/' + e.changedTouches.length);"
    "    else window.__btnEv.push(t);"
    "  });"
    "}"
    "const d = document.getElementById('drag');"
    "d.addEventListener('touchstart', e => { e.preventDefault(); window.__dragEv.push('ts:' + Math.round(e.touches[0].clientX)); });"
    "d.addEventListener('touchmove', e => { window.__dragEv.push('tm:' + Math.round(e.touches[0].clientX)); });"
    "d.addEventListener('touchend', e => { window.__dragEv.push('te:' + Math.round(e.changedTouches[0].clientX) + '/' + e.touches.length); });"
    "d.addEventListener('touchcancel', e => { window.__dragEv.push('tc'); });"
    "d.addEventListener('mousedown', () => window.__dragEv.push('md-BAD'));"
    "d.addEventListener('click', () => window.__dragEv.push('clk-BAD'));"
    "document.getElementById('other').addEventListener('touchmove', () => window.__otherEv.push('stolen'));"
    "</script></body></html>";

  CT_CHECK(ccord_web_view_load_html(v, html, sizeof html - 1u, "https://discord.com/", &err) == CCORD_OK);

  char out[256] = {0};
  CT_CHECK(ccord_web_view_eval_debug(v,
    "(() => [navigator.maxTouchPoints === 5, 'ontouchstart' in window, matchMedia('(pointer: coarse)').matches, navigator.userAgentData.mobile === true].every(Boolean) ? 'ok' : 'fail')()",
    out, sizeof out));
  CT_CHECK(strcmp(out, "ok") == 0);

  ccord_rect vb = {10, 20, 350, 480};
  // 1) Tap #btn (at local 270,420 -> view 280,440): fires pointer + touch + compat click
  ccord_input_event td1 = {.type = CCORD_IN_TOUCH_DOWN, .finger_id = 7, .x = 280.0f, .y = 440.0f};
  ccord_input_event tu1 = {.type = CCORD_IN_TOUCH_UP, .finger_id = 7, .x = 282.0f, .y = 441.0f};
  CT_CHECK(ccord_web_view_handle_input(v, vb, &td1));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &tu1));
  CT_CHECK(ccord_web_view_eval_debug(v, "window.__btnEv.join(',')", out, sizeof out));
  if (strcmp(out, "pointerdown:touch:true,touchstart:1/1,mousedown,pointerup,touchend:0/1,mouseup,click") != 0) {
    fprintf(stderr, "Touch button events='%s'\n", out);
  }
  CT_CHECK(strcmp(out, "pointerdown:touch:true,touchstart:1/1,mousedown,pointerup,touchend:0/1,mouseup,click") == 0);

  // 2) Drag starting on #drag (local x=60 -> view x=70) across onto #other (local x=240 -> view x=250):
  //    implicit touch capture keeps touchmove/touchend on #drag; preventDefault suppresses mousedown/click;
  //    secondary finger (id=99) is ignored while primary finger (id=11) is active.
  ccord_input_event td2 = {.type = CCORD_IN_TOUCH_DOWN, .finger_id = 11, .x = 70.0f, .y = 80.0f};
  ccord_input_event td_sec = {.type = CCORD_IN_TOUCH_DOWN, .finger_id = 99, .x = 260.0f, .y = 80.0f};
  ccord_input_event tm2 = {.type = CCORD_IN_TOUCH_MOVE, .finger_id = 11, .x = 250.0f, .y = 80.0f};
  ccord_input_event tu2 = {.type = CCORD_IN_TOUCH_UP, .finger_id = 11, .x = 250.0f, .y = 80.0f};
  CT_CHECK(ccord_web_view_handle_input(v, vb, &td2));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &td_sec));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &tm2));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &tu2));

  // 3) Touch cancel releases active touch cleanly
  ccord_input_event td3 = {.type = CCORD_IN_TOUCH_DOWN, .finger_id = 12, .x = 70.0f, .y = 80.0f};
  ccord_input_event tc3 = {.type = CCORD_IN_TOUCH_CANCEL, .finger_id = 12, .x = 70.0f, .y = 80.0f};
  CT_CHECK(ccord_web_view_handle_input(v, vb, &td3));
  CT_CHECK(ccord_web_view_handle_input(v, vb, &tc3));

  CT_CHECK(ccord_web_view_eval_debug(v, "window.__dragEv.join(',') + '|' + window.__otherEv.length", out, sizeof out));
  if (strcmp(out, "ts:60,tm:240,te:240/0,ts:60,tc|0") != 0) {
    fprintf(stderr, "Touch drag events='%s'\n", out);
  }
  CT_CHECK(strcmp(out, "ts:60,tm:240,te:240/0,ts:60,tc|0") == 0);

  ccord_web_view_destroy(v);
}

int main(void)
{
  test_dom_css_and_mutation();
  test_wasm_execution();
  test_canvas_and_crypto();
  test_iframe_fetch_postmsg_and_sdl_click();
  test_headless_sdl_render_and_worker();
  test_web_platform_compat_and_rejection_tracking();
  test_large_iframe_and_crash_hardening();
  test_css_layout_cascade_and_widget_paint();
  test_challenge_footer_layout_and_stacking_click();
  test_mobile_touch_events_and_iframe_drag();
  return CT_RESULT("test_web_engine");
}
