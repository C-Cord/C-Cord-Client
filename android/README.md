# C-Cord for Android

Native C23 Android port of C-Cord using SDL3, SDL3_ttf, Lua 5.5.1 (in-process sandboxed worker threads), libdave (MLS E2EE voice/video), FFmpeg (MediaCodec hardware decode/encode), libcurl, and OpenSSL 3.5.

## Features
- **ABIs:** `arm64-v8a` (64-bit ARM phones/tablets) and `x86_64` (Android emulators / Chromebooks), minimum Android 9 (API 28), target Android 15 (API 35) with 16 KB ELF & APK page alignment.
- **Native Mobile UI:** Responsive layout with edge-to-edge safe-area & soft-keyboard IME insets, touch drag-scroll & kinetic fling, long-press bottom-sheet context menus with in-place submenu drill-down, slide-out navigation & member drawers, 2-level mobile settings stacks, and Android Back navigation unwinding.
- **Native Android Integration:** `CCordActivity` (`SDLActivity`), `CCordBridge` JNI bridge (haptics, `ACTION_SEND` share sheet, SAF document picker, message notifications with deep-link jump, orientation & keep-screen-on, `ConnectivityManager` link monitoring, `MediaProjection` screen sharing), and `CCordVoiceService` foreground service with persistent Mute / Deafen / Disconnect controls.
- **Cross-Platform Plugins (API v2):** Runs Lua 5.5 plugins inside sandboxed worker threads with `platform.*` OS/capability detection, manifest `"platforms"` / `"requires_capabilities"` filtering, and mobile UX APIs (`ui.haptics.*`, `ui.share.*`, `ui.keyboard.*`, `ui.mobile.*`).

## Prerequisites
- Android SDK (API 35/36 platform + build-tools) and Android NDK 27+
- CMake 3.25+, Ninja/Make, `pkg-config`, Python 3, Perl, `autoconf`/`automake`, `nasm`/`yasm`
- JDK 17+ and Gradle 8.11+ (or `./gradlew`)
- Optional for local F-Droid repo generation: `pip install fdroidserver`

## 1. Cross-Compile Native Dependencies
Builds all third-party libraries into `android/deps/prefix/{arm64-v8a,x86_64}/` and fetches the Mozilla CA bundle + SDL3 Java sources:

```bash
./android/scripts/build_deps.sh arm64-v8a 28
./android/scripts/build_deps.sh x86_64 28
```

## 2. Build Android APKs Locally
Cross-compiles `libmain.so` for both ABIs with `-DCCORD_WERROR=ON`, stages `libSDL3.so`, `libc++_shared.so`, CA roots, fallback emoji/CJK fonts, and launcher icons, then builds & verifies 16 KB-aligned APKs via Gradle:

```bash
# Single debug APK (fast local development)
./android/scripts/build_apk.sh

# Release split APKs (arm64, x86_64, universal)
./android/scripts/build_apk.sh --release --splits --version=0.2.0 --out-dir=out/android
```

Options:
- `--debug` — RelWithDebInfo native symbols + `assembleDebug`
- `--release` — Stripped Release native libs + `assembleRelease`
- `--splits` — Build per-ABI (`arm64-v8a`, `x86_64`) + `universal` APKs with distinct monotonic `versionCode`s
- `--version=X.Y.Z` — Set app `versionName` and auto-compute SemVer `versionCode`
- `--abi="arm64-v8a"` — Build only specific ABI(s)
- `--out-dir=DIR` — Copy and rename release APKs (`C-Cord-<ver>-android-{arm64,x86_64,universal}.apk`)
- `--skip-native` — Re-run Gradle packaging without rebuilding C/C++ libraries

## 3. Release Signing & Official F-Droid Repository (`c-cord.org`)

Releases are built automatically on tagged pushes (`v*`) by `.github/workflows/release.yml` (`build-android` job):
1. **GitHub Releases** stores the signed APKs (`C-Cord-<ver>-android-arm64.apk`, `C-Cord-<ver>-android-x86_64.apk`, `C-Cord-<ver>-android-universal.apk`) and lightweight signed F-Droid metadata archive (`ccord-fdroid-repo.tar.gz`).
2. **`c-cord.org`** ingests `ccord-fdroid-repo.tar.gz` on release announce (`POST /api/v1/releases/announce`), serves the F-Droid Index v1 & v2 files at **`https://c-cord.org/repo/fdroid/repo`**, and 302-redirects `/repo/fdroid/repo/*.apk` straight to GitHub Releases while tracking installs.
3. Users can either add **`https://c-cord.org/repo/fdroid/repo`** in F-Droid / Droid-ify / Neo Store (one-tap `fdroidrepos://` link on `c-cord.org`) or download a direct APK.

### One-Time Signing Key Setup (Maintainers)
Generate persistent APK and F-Droid signing keys once (keep the generated `android/.keys/` folder backed up safely — never commit it):

```bash
./android/scripts/gen_signing_keys.sh
```

The script prints the SHA-256 fingerprints and exact `gh secret set` commands for:
- `ANDROID_KEYSTORE_B64`, `ANDROID_STORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`
- `FDROID_KEYSTORE_B64`, `FDROID_STORE_PASSWORD`, `FDROID_KEY_ALIAS`, `FDROID_KEY_PASSWORD`

### Testing F-Droid Repo Generation Locally
```bash
source android/.keys/secrets.env
./android/scripts/build_apk.sh --release --splits --version=0.2.0 --out-dir=/tmp/ccord-apks
./android/scripts/build_fdroid_repo.sh \
  --apks-dir=/tmp/ccord-apks \
  --out=/tmp/ccord-fdroid-repo.tar.gz
```

## 4. Testing Mobile UI on Desktop
You can iterate on the responsive mobile UI directly on Linux/macOS/Windows without an emulator:

```bash
./build-linux/src/app/ccord --mock --mobile --size 412x915
```
