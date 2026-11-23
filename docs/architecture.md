# C-Cord Architecture

C-Cord is a native desktop Discord client written in C23 (`-std=c23`, `-fno-extensions`) with out-of-process Lua 5.5.1 extensions. Layering is enforced at link time through CMake static libraries—there is no global include root, so a module can only include headers from targets it explicitly links against.

## Module Layout

| Module | Target(s) | Dependencies | Responsibility |
|---|---|---|---|
| `src/core` | `ccord_core` | — | Allocators, arenas, byte buffers, base64, UTF-8, SHA-256, random, clock, paths, atomic files, secret buffers, logging, JSON (reader + writer), INI, ID map, event bus |
| `src/assets` | `ccord_assets` | — | Build-time embedded font/UI asset bytes (`ccord_embedded_asset_find`) |
| `src/model` | `ccord_model` | `ccord_core` | Pure domain types across 11 domain headers (guilds, channels, users, messages, calls, account settings, rich content, 52-bit permission engine, audit/forum/search/report models) |
| `src/contrib` | `ccord_contrib` | `ccord_core`, `ccord_model` | Plugin UI/command/asset contribution registry, panel patch protocol, and extension point definitions |
| `src/emoji` | `ccord_emoji` | `ccord_core` | Unicode emoji dataset, lookup, and shortcode mapping |
| `src/theme` | `ccord_theme` | `ccord_core` | `.theme` parser and color/metric token loading |
| `src/store` | `ccord_store` | `ccord_core`, `ccord_model` | Main client state store, O(1) indexed lookups, unread/mention counts, notifications, voice/call state, account settings state |
| `src/ipc` | `ccord_ipc` | `ccord_core` | Length-prefixed JSON IPC framing over Unix domain sockets and 3-class permission vocabulary |
| `src/rpc` | `ccord_rpc` | `ccord_core` | Local `discord-ipc-*` Rich Presence server, activity builder, and per-OS process scanners (`linux`/`macos`/`win32`) |
| `src/net` | `ccord_net_contract`, `ccord_net_mock`, `ccord_net_tls`, `ccord_net_discord`, `ccord_net_voice`, `ccord_net_services`, `ccord_net_live`, `ccord_net` (INTERFACE umbrella) | Layered DAG rooted at `ccord_core`, `ccord_model`; live/voice layers optionally link libcurl, zlib, OpenSSL, opus, libsodium, `ccord_dave`, speexdsp | Transport vtable, deterministic mock transport, per-route request governor, REST v9, Gateway v9 WebSocket, voice gateway, Opus/DSP audio, RTP H.264 streaming, DAVE E2EE, media/HTTP services |
| `src/session` | `ccord_session` | `ccord_core`, `ccord_model`, `ccord_store`, `ccord_net_contract`, `Threads` | Session controller, request budgets, intent validation, local echo/retry, ranged media stream cache |
| `src/plugins` | `ccord_plugins` | `ccord_core`, `ccord_model`, `ccord_contrib`, `ccord_store`, `ccord_session`, `ccord_ipc`, zlib, optional libsodium | Plugin lifecycle manager, strict manifest validation, `.cce`/`.zip` archive installer, permission enforcement, plugin storage, v3 API handlers, async workers, signed plugin catalog |
| `src/plugin_host` | `ccord_host_sandbox`, `ccord_plugin_host_core`, `ccord-plugin-host` | `ccord_lua`, `ccord_core`, `ccord_ipc` | Isolated per-plugin Lua 5.5.1 runtime (`sandbox/`), OS-level sandbox & IPC host loop (`host/`), thin executable entry (`main.c`) |
| `src/platform` | `ccord_platform` | `ccord_core`, SDL3, FFmpeg, optional PipeWire/X11/GIO/DBus | Windowing/input (`window/`), audio devices & codecs (`audio/`), video/camera/screen capture (`video/`, `capture/`), tray/notifications/hotkeys/links (`desktop/`), embedded app icons (`icons/`) |
| `src/render` | `ccord_render` | `ccord_core`, `ccord_assets`, `ccord_platform`, `ccord_theme`, SDL3, SDL3_ttf, libpng/jpeg/webp, FFmpeg, optional fontconfig | 2D hardware/software drawing primitives, glyph/text LRU cache, runtime font fallback, image/video frame decoding, PNG screenshots |
| `src/ui` | `ccord_ui_logic`, `ccord_ui` | `ccord_ui_logic` → `ccord_core`, `ccord_theme`; `ccord_ui` → `ui_logic`, `render`, `session`, `model`, `contrib`, `store`, `emoji`, `theme`, `platform`, `core` | SDL-free layout/virtual list/text editor/time formatting (`logic/`) and interactive views organized by domain (`core/`, `nav/`, `chat/`, `voice/`, `overlays/`, `expressions/`, `settings/`, `plugins/`) |
| `src/app` | `ccord_account`, `ccord_verify` (tests only), `ccord` | All client modules + embedded `ccord_host_sandbox` | Main loop & CLI (`main.c`, `app_*.c`), keyring & warm-start cache (`storage/`), desktop/tray/link glue (`desktop/`), audio/camera/video pipeline (`media/`), RPC wiring (`rpc/`), verification harnesses (`verify/`) |

## Layered Domain Organization

Large subsystems are decomposed into focused domain subfolders so related files stay easy to find and incremental rebuilds stay narrow:

- **`src/net/`** — `common/` (URL policy, shared curl/TLS + SSRF opensocket filter), `policy/` (governor, gateway pace, voice reconnect, protobuf), `admin/` (channel/guild/moderation/expression/social payload builders), `mock/` (offline mock transport), `discord/` (REST v9, Gateway v9, JSON parsers, token store), `voice/` (Voice Gateway v8, Opus/AEAD/DSP, DAVE E2EE, Go Live H.264/camera), `services/` (profile edit, HTTP fetch, video resolver, detectable games), `live/` (live transport orchestrator).
  - Unit tests link only the narrowest layer they exercise (e.g., `ccord_net_contract` tests never relink libcurl, OpenSSL, Opus, libsodium, or DAVE).
- **`src/platform/`** — `window/`, `audio/`, `video/`, `capture/`, `desktop/`, `icons/`.
- **`src/plugins/`** — `core/` (lifecycle, host proc, storage, RPC), `package/` (manifest, `.cce`/`.zip` packaging & SHA-256/manifest verification), `api_v3/` (system/UI/Discord handlers), `workers/` (async net/fs/proc queues), `catalog/` (index fetch, parse, Ed25519 signature verification, revocations, install metadata).
- **`src/plugin_host/`** — `sandbox/` (Lua 5.5.1 VM, standard libs, v1/v2/v3 API bindings) and `host/` (OS Landlock/Seccomp/Seatbelt/Job Object sandbox + IPC runtime loop).
- **`src/ui/`** — `logic/` (SDL-free widgets/editors), `core/` (UI state, layout geometry, frame driver, input routing, caches, media), `nav/` (servers, channels, DMs, members), `chat/` (markdown, timeline, cards, embeds, components, composer), `voice/` (call/DAVE overlays, voice stage, activities, Go Live viewer), `overlays/` (overlay dispatch, modals, context menus, popovers, profiles, discovery, verification), `expressions/` (emoji/GIF/sticker/soundboard picker), `settings/` (user, server, and channel settings), `plugins/` (panel compiler/renderer, v3 surfaces/modals, plugin icons).
  - Internal UI headers are scoped via a `ccord_ui_internal_inc` INTERFACE target linked only by `ccord_ui` and white-box tests; the production `ccord` binary sees only public headers under `src/ui/include/`.
- **`src/app/`** — `storage/` (multi-account keyring, state persistence), `desktop/` (notifications, tray, hotkeys, links), `media/` (voice audio thread, camera, Go Live capture, video playback), `rpc/` (Rich Presence integration), `verify/` (`ccord_verify`, compiled only when `CCORD_BUILD_TESTS=ON`).

## Key Architectural Boundaries

- **Transport abstraction (`src/net/include/ccord/net/transport.h`):** `ccord_session` and `ccord_ui` interact exclusively with the `ccord_transport` vtable, allowing identical execution against the live Discord transport (`--live`) and the deterministic offline mock (`--mock`).
- **Governed writes & store ownership:** User and admin actions go through the request governor in `src/net/policy/governor.c` before mutating client state; failures restore drafts and surface API errors.
- **Secret isolation & hygiene (`cmake/CheckHygiene.cmake`):** Lower/domain layers (`src/ipc`, `src/plugins`, `src/plugin_host`, `src/store`, `src/session`, `src/net`, `src/model`, `src/contrib`) are statically prevented from referencing `ccord/core/secret.h`, and token/secret literals are rejected across the repository by `test_hygiene`.
- **Out-of-process plugin isolation:** Lua 5.5.1 extensions run in dedicated OS-sandboxed `ccord-plugin-host` processes on desktop (64 MiB heap cap, default 32 MiB; 50M default / 100M max instructions per callback) and sandboxed in-process worker threads on Android (32 MiB heap cap, default 16 MiB) over framed JSON IPC (`src/ipc`). See [`docs/plugin-api/README.md`](plugin-api/README.md) and [`SECURITY.md`](../SECURITY.md) for the 3-Class Permission Model, network/SSRF policy, OS sandboxing, and catalog signing details.
- **Build-time options:** `CCORD_LIVE_ACCOUNT=OFF` builds an offline-only client without live REST/Gateway/voice/DAVE dependencies; `CCORD_BUILD_TESTS=OFF` omits all CTest targets and `ccord_verify` (`--verify-slice`) verification harnesses.
