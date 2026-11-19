# C-Cord for Android

Native C23 Android port of C-Cord using SDL3, SDL3_ttf, Lua 5.5.1 (in-process sandboxed worker threads), libdave (MLS E2EE voice/video), FFmpeg (MediaCodec hardware decode/encode), libcurl, and OpenSSL 3.5.

## Features
- **ABIs:** `arm64-v8a` (64-bit ARM phones/tablets) and `x86_64` (Android emulators / Chromebooks), minimum Android 9 (API 28), target Android 15 (API 35) with 16 KB ELF & APK page alignment.
- **Native Mobile UI:** Responsive layout with edge-to-edge safe-area & soft-keyboard IME insets, touch drag-scroll & kinetic fling, long-press bottom-sheet context menus with in-place submenu drill-down, slide-out navigation & member drawers, 2-level mobile settings stacks, and Android Back navigation unwinding.
- **Native Android Integration:** `CCordActivity` (`SDLActivity`), `CCordBridge` JNI bridge (haptics, `ACTION_SEND` share sheet, SAF document picker, message notifications with deep-link jump, orientation & keep-screen-on, `ConnectivityManager` link monitoring, `MediaProjection` screen sharing), and `CCordVoiceService` foreground service with persistent Mute / Deafen / Disconnect controls.
- **Cross-Platform Plugins (API v2):** Runs Lua 5.5 plugins inside sandboxed worker threads with `platform.*` OS/capability detection, manifest `"platforms"` / `"requires_capabilities"` filtering, and mobile UX APIs (`ui.haptics.*`, `ui.share.*`, `ui.keyboard.*`, `ui.mobile.*`).

## Prerequisites
- Android SDK (API 35/36 platform + build-tools) and Android NDK 27+
- CMake 3.25+, Ninja/Make, `pkg-config`, Python 3, Perl, `autoconf`/`automake`, `nasm`/`yasm` (for x86_64 libjpeg-turbo/FFmpeg)
- JDK 17+ and Gradle 8.11+ (or `./gradlew`)

## 1. Cross-Compile Native Dependencies
Builds all third-party libraries into `android/deps/prefix/{arm64-v8a,x86_64}/` and fetches the Mozilla CA bundle + SDL3 Java sources:

```bash
./android/scripts/build_deps.sh
```

To build a single ABI (e.g. only `arm64-v8a`):

```bash
CCORD_ANDROID_ABIS="arm64-v8a" ./android/scripts/build_deps.sh
```

## 2. Build the Android APK
Cross-compiles `libmain.so` for both ABIs with `-DCCORD_WERROR=ON`, stages `libSDL3.so`, `libc++_shared.so`, CA roots, fallback emoji/CJK fonts, and launcher icons, then runs Gradle to assemble and verify the 16 KB-aligned APK:

```bash
./android/scripts/build_apk.sh
```

Options:
- `--debug` — RelWithDebInfo native symbols + `assembleDebug`
- `--release` — Stripped Release native libs + `assembleRelease`
- `--abi="arm64-v8a"` — Build only specific ABI(s)
- `--skip-native` — Re-run Gradle packaging without rebuilding C/C++ libraries

Output APK:
- `android/app/build/outputs/apk/debug/app-debug.apk`

## 3. Testing Mobile UI on Desktop
You can iterate on the responsive mobile UI directly on Linux/macOS/Windows without an emulator:

```bash
./build-linux/src/app/ccord --mock --mobile --size 412x915
```
