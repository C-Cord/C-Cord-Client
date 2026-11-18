# C-Cord Architecture

C-Cord is a native desktop Discord client written in C23 (`-std=c23`, `-fno-extensions`) with out-of-process Lua 5.5.1 extensions. Layering is enforced at link time through CMake static libraries—there is no global include root, so a module can only include headers from targets it explicitly links against.

## Module Layout

| Module | Target(s) | Dependencies | Responsibility |
|---|---|---|---|
| `src/core` | `ccord_core` | — | Allocators, arenas, byte buffers, UTF-8, SHA-256, random, clock, paths, atomic files, secret buffers, logging, JSON, INI, ID map, event bus |
| `src/model` | `ccord_model` | `ccord_core` | Domain types (guilds, channels, users, messages, calls, account settings), rich content, 52-bit permission engine, audit/forum/search/report models, plugin contribution registry |
| `src/emoji` | `ccord_emoji` | `ccord_core` | Unicode emoji dataset, lookup, and shortcode mapping |
| `src/theme` | `ccord_theme` | `ccord_core` | `.theme` parser and color/metric token loading |
| `src/store` | `ccord_store` | `ccord_core`, `ccord_model` | Main client state store, O(1) indexed lookups, unread/mention counts, notifications, voice/call state, account settings state |
| `src/ipc` | `ccord_ipc` | `ccord_core` | Length-prefixed JSON IPC framing over Unix domain sockets and 3-class permission vocabulary |
| `src/rpc` | `ccord_rpc` | `ccord_core` | Local `discord-ipc-*` Rich Presence server, activity builder, and game/process detection |
| `src/net` | `ccord_net` | `ccord_core`, `ccord_model`, (`ccord_dave`, libcurl, zlib, OpenSSL, opus, libsodium, optional speexdsp) | Transport vtable, deterministic mock transport, per-route request governor, REST v9, Gateway v9 WebSocket, voice gateway, Opus/DSP audio, RTP H.264 streaming, DAVE E2EE |
| `src/session` | `ccord_session` | `ccord_core`, `ccord_model`, `ccord_store`, `ccord_net`, `Threads` | Session controller, request budgets, intent validation, local echo/retry, ranged media stream cache |
| `src/plugins` | `ccord_plugins` | `ccord_core`, `ccord_model`, `ccord_store`, `ccord_session`, `ccord_ipc`, zlib | Plugin lifecycle manager, strict manifest validation, `.cce`/`.zip` archive installer, permission enforcement, plugin storage |
| `src/plugin_host` | `ccord_host_sandbox`, `ccord-plugin-host` | `ccord_lua`, `ccord_core`, `ccord_ipc` | Isolated per-plugin Lua 5.5.1 runtime, sandboxed standard library, host IPC loop |
| `src/platform` | `ccord_platform` | `ccord_core`, SDL3, FFmpeg, optional PipeWire/X11/GIO/DBus | Windowing, input, audio/camera device enumeration, screen capture (Wayland portal + X11), audio/video codecs, tray/notifications/hotkeys, link watcher |
| `src/render` | `ccord_render` | `ccord_core`, `ccord_platform`, `ccord_theme`, SDL3, SDL3_ttf, libpng/jpeg/webp, FFmpeg, optional fontconfig | 2D hardware/software drawing primitives, glyph/text LRU cache, runtime font fallback, image/video frame decoding, PNG screenshots |
| `src/ui` | `ccord_ui_logic`, `ccord_ui` | `ccord_ui_logic` → `ccord_core`, `ccord_theme`; `ccord_ui` → `ui_logic`, `render`, `session`, `model`, `store`, `emoji`, `theme`, `platform`, `core` | SDL-free layout/virtual list/text editor/time formatting (`ccord_ui_logic`) and interactive views: chat, nav, members, profiles, calls/DAVE, server/channel/user settings (`ccord_ui`) |
| `src/app` | `ccord_account`, `ccord` | All client modules + embedded `ccord_host_sandbox` | Main loop, CLI flags, multi-account keyring, voice audio thread, media/desktop glue, warm-start cache |

## Key Architectural Boundaries

- **Transport abstraction (`src/net/include/ccord/net/transport.h`):** `ccord_session` and `ccord_ui` interact exclusively with the `ccord_transport` vtable, allowing identical execution against the live Discord transport (`--live`) and the deterministic offline mock (`--mock`).
- **Governed writes & store ownership:** User and admin actions go through the request governor in `src/net/governor.c` before mutating client state; failures restore drafts and surface API errors.
- **Out-of-process plugin isolation:** Lua 5.5.1 extensions run in dedicated `ccord-plugin-host` processes over `SO_PEERCRED`-verified Unix domain sockets using framed JSON IPC (`src/ipc`). See [`docs/plugin-api/README.md`](plugin-api/README.md) and [`SECURITY.md`](../SECURITY.md) for the 3-Class Permission Model and hardening details.
- **Build-time options:** `CCORD_LIVE_ACCOUNT=OFF` builds an offline-only client without live REST/Gateway/voice/DAVE dependencies; `CCORD_BUILD_TESTS=OFF` omits all CTest targets and `--verify-slice` verification harnesses.
