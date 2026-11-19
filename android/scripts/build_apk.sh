#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
# Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
# Cross-compiles C-Cord native libraries for Android (arm64-v8a, x86_64) and builds the APK via Gradle.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$ANDROID_DIR/.." && pwd)"

SDK_DIR="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"
if [[ ! -d "$SDK_DIR/ndk" && -d "$HOME/Android/Sdk/ndk" ]]; then
  SDK_DIR="$HOME/Android/Sdk"
fi
if [[ ! -d "$SDK_DIR" ]]; then
  echo "error: Android SDK not found at $SDK_DIR (set ANDROID_SDK_ROOT)" >&2
  exit 1
fi

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  ANDROID_NDK_HOME="$(ls -d "$SDK_DIR"/ndk/* 2>/dev/null | sort -V | tail -n 1 || true)"
fi
if [[ -z "$ANDROID_NDK_HOME" || ! -d "$ANDROID_NDK_HOME" ]]; then
  echo "error: Android NDK not found under $SDK_DIR/ndk (set ANDROID_NDK_HOME)" >&2
  exit 1
fi

ABIS="${CCORD_ANDROID_ABIS:-arm64-v8a x86_64}"
API="${CCORD_ANDROID_API:-28}"
CMAKE_BUILD_TYPE="${CCORD_ANDROID_BUILD_TYPE:-Release}"
GRADLE_TASK="${CCORD_GRADLE_TASK:-assembleDebug}"
SKIP_NATIVE="${CCORD_SKIP_NATIVE:-0}"

for arg in "$@"; do
  case "$arg" in
    --debug)
      CMAKE_BUILD_TYPE="RelWithDebInfo"
      GRADLE_TASK="assembleDebug"
      ;;
    --release)
      CMAKE_BUILD_TYPE="Release"
      GRADLE_TASK="assembleRelease"
      ;;
    --skip-native)
      SKIP_NATIVE=1
      ;;
    --abi=*)
      ABIS="${arg#--abi=}"
      ;;
    *)
      echo "usage: $0 [--debug|--release] [--skip-native] [--abi=\"arm64-v8a x86_64\"]" >&2
      exit 2
      ;;
  esac
done

HOST_TAG="linux-x86_64"
LLVM_STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG/bin/llvm-strip"
SYSROOT_LIB="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG/sysroot/usr/lib"
JOBS="$(nproc 2>/dev/null || echo 4)"

MAIN_RES="$ANDROID_DIR/app/src/main"
JNILIBS_DIR="$MAIN_RES/jniLibs"
SDL_JAVA_SRC="$ANDROID_DIR/deps/sdl3-java/org/libsdl/app"
SDL_JAVA_DST="$MAIN_RES/java/org/libsdl/app"

if [[ ! -d "$SDL_JAVA_SRC" ]]; then
  echo "error: missing SDL3 Java glue at $SDL_JAVA_SRC — run android/scripts/build_deps.sh first" >&2
  exit 1
fi

if [[ "$SKIP_NATIVE" != "1" ]]; then
  rm -rf "$JNILIBS_DIR"
  for ABI in $ABIS; do
    case "$ABI" in
      arm64-v8a) TRIPLE="aarch64-linux-android" ;;
      x86_64)    TRIPLE="x86_64-linux-android" ;;
      *)
        echo "error: unsupported ABI '$ABI' (expected arm64-v8a or x86_64)" >&2
        exit 1
        ;;
    esac

    PREFIX="$ANDROID_DIR/deps/prefix/$ABI"
    if [[ ! -f "$PREFIX/lib/libSDL3.so" ]]; then
      echo "error: missing cross-compiled deps for $ABI in $PREFIX — run android/scripts/build_deps.sh first" >&2
      exit 1
    fi

    BUILD_DIR="$ROOT_DIR/build-android-$ABI"
    echo "==> Configuring C-Cord for Android $ABI (API $API, $CMAKE_BUILD_TYPE)..."
    PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
    PKG_CONFIG_PATH="" \
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
      -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" \
      -DANDROID_PLATFORM="android-$API" \
      -DANDROID_STL=c++_shared \
      -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DCCORD_WERROR=ON \
      -DCCORD_BUILD_TESTS=OFF

    echo "==> Building libmain.so for $ABI..."
    cmake --build "$BUILD_DIR" --target main -j"$JOBS"

    OUT_LIB_DIR="$JNILIBS_DIR/$ABI"
    mkdir -p "$OUT_LIB_DIR"
    cp -f "$BUILD_DIR/src/app/libmain.so" "$OUT_LIB_DIR/libmain.so"
    cp -f "$PREFIX/lib/libSDL3.so" "$OUT_LIB_DIR/libSDL3.so"
    cp -f "$SYSROOT_LIB/$TRIPLE/libc++_shared.so" "$OUT_LIB_DIR/libc++_shared.so"

    if [[ "$CMAKE_BUILD_TYPE" == "Release" && -x "$LLVM_STRIP" ]]; then
      "$LLVM_STRIP" --strip-unneeded "$OUT_LIB_DIR/libmain.so"
    fi
  done
fi

echo "==> Staging SDL3 Java sources, CA bundle, fonts, and launcher icons..."
rm -rf "$SDL_JAVA_DST"
mkdir -p "$SDL_JAVA_DST"
cp -f "$SDL_JAVA_SRC"/*.java "$SDL_JAVA_DST/"

mkdir -p "$MAIN_RES/assets/fonts"
if [[ -f "$ANDROID_DIR/deps/cacert.pem" ]]; then
  cp -f "$ANDROID_DIR/deps/cacert.pem" "$MAIN_RES/assets/cacert.pem"
else
  echo "error: missing $ANDROID_DIR/deps/cacert.pem — run android/scripts/build_deps.sh first" >&2
  exit 1
fi

for FONT in NotoColorEmoji-Subset.ttf NotoSansCJK-Subset.ttf; do
  if [[ -f "$ROOT_DIR/assets/fonts/$FONT" ]]; then
    cp -f "$ROOT_DIR/assets/fonts/$FONT" "$MAIN_RES/assets/fonts/$FONT"
  fi
done

mkdir -p \
  "$MAIN_RES/res/mipmap-mdpi" \
  "$MAIN_RES/res/mipmap-hdpi" \
  "$MAIN_RES/res/mipmap-xhdpi" \
  "$MAIN_RES/res/mipmap-xxhdpi" \
  "$MAIN_RES/res/mipmap-xxxhdpi"
cp -f "$ROOT_DIR/assets/icons/ccord-48.png"  "$MAIN_RES/res/mipmap-mdpi/ic_launcher.png"
cp -f "$ROOT_DIR/assets/icons/ccord-64.png"  "$MAIN_RES/res/mipmap-hdpi/ic_launcher.png"
cp -f "$ROOT_DIR/assets/icons/ccord-128.png" "$MAIN_RES/res/mipmap-xhdpi/ic_launcher.png"
cp -f "$ROOT_DIR/assets/icons/ccord-128.png" "$MAIN_RES/res/mipmap-xxhdpi/ic_launcher.png"
cp -f "$ROOT_DIR/assets/icons/ccord-256.png" "$MAIN_RES/res/mipmap-xxxhdpi/ic_launcher.png"

printf 'sdk.dir=%s\n' "$SDK_DIR" > "$ANDROID_DIR/local.properties"

GRADLE_BIN=""
if [[ -x "$ANDROID_DIR/gradlew" ]]; then
  GRADLE_BIN="$ANDROID_DIR/gradlew"
elif command -v gradle >/dev/null 2>&1; then
  GRADLE_BIN="$(command -v gradle)"
else
  GRADLE_BIN="$(find "$HOME/.gradle/wrapper/dists" -path "*/gradle-8.14*/bin/gradle" 2>/dev/null | head -n 1 || true)"
  if [[ -z "$GRADLE_BIN" ]]; then
    GRADLE_BIN="$(find "$HOME/.gradle/wrapper/dists" -path "*/gradle-8.1*/bin/gradle" 2>/dev/null | sort -V | tail -n 1 || true)"
  fi
fi
if [[ -z "$GRADLE_BIN" || ! -x "$GRADLE_BIN" ]]; then
  echo "error: Gradle 8.11+ not found (install gradle or place gradlew in android/)" >&2
  exit 1
fi

echo "==> Running Gradle $GRADLE_TASK via $GRADLE_BIN..."
(
  cd "$ANDROID_DIR"
  ANDROID_HOME="$SDK_DIR" ANDROID_SDK_ROOT="$SDK_DIR" "$GRADLE_BIN" "$GRADLE_TASK" --no-daemon
)

if [[ "$GRADLE_TASK" == "assembleRelease" ]]; then
  APK_PATH="$ANDROID_DIR/app/build/outputs/apk/release/app-release.apk"
else
  APK_PATH="$ANDROID_DIR/app/build/outputs/apk/debug/app-debug.apk"
fi

if [[ ! -f "$APK_PATH" ]]; then
  echo "error: expected APK not found at $APK_PATH" >&2
  exit 1
fi

echo "==> Built APK: $APK_PATH ($(du -h "$APK_PATH" | cut -f1))"

ZIPALIGN="$(ls -d "$SDK_DIR"/build-tools/*/zipalign 2>/dev/null | sort -V | tail -n 1 || true)"
if [[ -n "$ZIPALIGN" && -x "$ZIPALIGN" ]]; then
  if "$ZIPALIGN" -c -P 16 4 "$APK_PATH" >/dev/null 2>&1; then
    echo "==> Verified 16 KB page alignment (Android 15+ compliant)"
  else
    echo "warning: zipalign 16 KB check reported a mismatch" >&2
  fi
fi
