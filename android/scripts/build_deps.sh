#!/usr/bin/env bash
# Cross-compile all C-Cord native dependencies for Android (NDK r27+, Clang 18+ / C23).
# Usage:
#   ./android/scripts/build_deps.sh [abi] [api_level]
# Examples:
#   ./android/scripts/build_deps.sh arm64-v8a 28
#   ./android/scripts/build_deps.sh x86_64 28
#
# Outputs:
#   android/deps/prefix/<abi>/          - headers, static libs (.a), libSDL3.so, pkgconfig/, cmake/
#   android/deps/sdl3-java/             - official SDL3 org/libsdl/app/*.java sources
#   android/deps/cacert.pem             - Mozilla CA bundle for OpenSSL / libcurl on Android
set -euo pipefail

ABI="${1:-arm64-v8a}"
API="${2:-28}"
JOBS="${JOBS:-$(nproc)}"

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
ANDROID_DIR="${ROOT_DIR}/android"
DEPS_DIR="${ANDROID_DIR}/deps"
DL_DIR="${DEPS_DIR}/downloads"
SRC_DIR="${DEPS_DIR}/src"
BUILD_DIR="${DEPS_DIR}/build/${ABI}"
PREFIX="${DEPS_DIR}/prefix/${ABI}"
STAMP_DIR="${DEPS_DIR}/stamps/${ABI}"

# Auto-detect Android NDK (prefer NDK r27+ with Clang >= 18 for C23).
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  for cand in \
    "${ANDROID_SDK_ROOT:-}/ndk/"* \
    "${ANDROID_HOME:-}/ndk/"* \
    "${HOME}/Android/Sdk/ndk/"*; do
    if [[ -d "${cand}/toolchains/llvm/prebuilt" ]]; then
      ANDROID_NDK_HOME="${cand}"
    fi
  done
fi
if [[ -z "${ANDROID_NDK_HOME:-}" || ! -d "${ANDROID_NDK_HOME}" ]]; then
  echo "ERROR: ANDROID_NDK_HOME not found. Set ANDROID_NDK_HOME to NDK r27+." >&2
  exit 1
fi

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"
CMAKE_TOOLCHAIN="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake"
SYSROOT="${TOOLCHAIN}/sysroot"

case "${ABI}" in
  arm64-v8a)
    TRIPLE="aarch64-linux-android"
    OPENSSL_TARGET="android-arm64"
    FFMPEG_ARCH="aarch64"
    FFMPEG_CPU="armv8-a"
    FFMPEG_EXTRA="--enable-neon"
    ;;
  x86_64)
    TRIPLE="x86_64-linux-android"
    OPENSSL_TARGET="android-x86_64"
    FFMPEG_ARCH="x86_64"
    FFMPEG_CPU="x86-64"
    FFMPEG_EXTRA="--disable-x86asm"
    ;;
  armeabi-v7a)
    TRIPLE="armv7a-linux-androideabi"
    OPENSSL_TARGET="android-arm"
    FFMPEG_ARCH="arm"
    FFMPEG_CPU="armv7-a"
    FFMPEG_EXTRA="--enable-neon --enable-thumb"
    ;;
  *)
    echo "ERROR: Unsupported ABI '${ABI}' (supported: arm64-v8a, x86_64, armeabi-v7a)" >&2
    exit 1
    ;;
esac

export CC="${TOOLCHAIN}/bin/${TRIPLE}${API}-clang"
export CXX="${TOOLCHAIN}/bin/${TRIPLE}${API}-clang++"
export AR="${TOOLCHAIN}/bin/llvm-ar"
export AS="${CC}"
export LD="${TOOLCHAIN}/bin/ld.lld"
export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
export STRIP="${TOOLCHAIN}/bin/llvm-strip"
export NM="${TOOLCHAIN}/bin/llvm-nm"

mkdir -p "${DL_DIR}" "${SRC_DIR}" "${BUILD_DIR}" "${PREFIX}/lib/pkgconfig" "${PREFIX}/include" "${STAMP_DIR}"

# Isolate pkg-config completely from the Linux host!
export PKG_CONFIG_DIR=""
export PKG_CONFIG_LIBDIR="${PREFIX}/lib/pkgconfig"
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""

# Provide a zlib.pc pointing at Android NDK's built-in libz so pkg-config chains
# (libpng, freetype2, harfbuzz, ffmpeg) resolve cleanly without host leaks.
cat > "${PREFIX}/lib/pkgconfig/zlib.pc" <<EOF
prefix=${SYSROOT}/usr
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: zlib
Description: Android NDK zlib
Version: 1.3.1
Libs: -lz
Cflags:
EOF

# Helper: download with resume/cache
fetch() {
  local name="$1" url="$2"
  local out="${DL_DIR}/${name}"
  if [[ ! -s "${out}" ]]; then
    echo "==> Downloading ${name}"
    curl -fL --retry 3 --retry-delay 2 -o "${out}.tmp" "${url}"
    mv "${out}.tmp" "${out}"
  fi
}

# Helper: extract tarball once into SRC_DIR/<dir>
extract() {
  local archive="$1" dir="$2"
  if [[ ! -d "${SRC_DIR}/${dir}" ]]; then
    echo "==> Extracting ${archive}"
    tar -xf "${DL_DIR}/${archive}" -C "${SRC_DIR}"
  fi
}

# Common CMake flags for static cross-builds
cmake_build() {
  local name="$1" src="$2"
  shift 2
  local bdir="${BUILD_DIR}/${name}"
  rm -rf "${bdir}"
  cmake -S "${src}" -B "${bdir}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN}" \
    -DANDROID_ABI="${ABI}" \
    -DANDROID_PLATFORM="android-${API}" \
    -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DCMAKE_FIND_ROOT_PATH="${PREFIX}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    "$@"
  cmake --build "${bdir}" --parallel "${JOBS}"
  cmake --install "${bdir}"
}

echo "============================================================"
echo "C-Cord Android Dependencies Build"
echo "  ABI:    ${ABI} (API ${API})"
echo "  NDK:    ${ANDROID_NDK_HOME}"
echo "  Prefix: ${PREFIX}"
echo "  Jobs:   ${JOBS}"
echo "============================================================"

# 0. CA cert bundle (for OpenSSL + libcurl TLS verification on Android)
fetch "cacert.pem" "https://curl.se/ca/cacert.pem"
cp "${DL_DIR}/cacert.pem" "${DEPS_DIR}/cacert.pem"

# 1. nlohmann_json (header-only, required by mlspp/libdave)
if [[ ! -f "${STAMP_DIR}/nlohmann_json" ]]; then
  fetch "json-3.12.0.tar.xz" "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz"
  extract "json-3.12.0.tar.xz" "json"
  cmake_build "nlohmann_json" "${SRC_DIR}/json" \
    -DJSON_BuildTests=OFF \
    -DJSON_Install=ON
  touch "${STAMP_DIR}/nlohmann_json"
fi

# 2. OpenSSL 3.5.0 (static libcrypto + libssl)
if [[ ! -f "${STAMP_DIR}/openssl" ]]; then
  fetch "openssl-3.5.0.tar.gz" "https://github.com/openssl/openssl/releases/download/openssl-3.5.0/openssl-3.5.0.tar.gz"
  extract "openssl-3.5.0.tar.gz" "openssl-3.5.0"
  rm -rf "${BUILD_DIR}/openssl"
  cp -a "${SRC_DIR}/openssl-3.5.0" "${BUILD_DIR}/openssl"
  (
    cd "${BUILD_DIR}/openssl"
    export ANDROID_NDK_ROOT="${ANDROID_NDK_HOME}"
    export PATH="${TOOLCHAIN}/bin:${PATH}"
    ./Configure "${OPENSSL_TARGET}" "-D__ANDROID_API__=${API}" \
      no-shared no-tests no-ui-console no-apps \
      --prefix="${PREFIX}" --openssldir="${PREFIX}/ssl"
    make -j"${JOBS}" build_libs
    make install_dev
  )
  touch "${STAMP_DIR}/openssl"
fi

# 3. libcurl 8.13.0 (static, OpenSSL 3 + WebSockets + NDK zlib)
if [[ ! -f "${STAMP_DIR}/curl" ]]; then
  fetch "curl-8.13.0.tar.xz" "https://curl.se/download/curl-8.13.0.tar.xz"
  extract "curl-8.13.0.tar.xz" "curl-8.13.0"
  cmake_build "curl" "${SRC_DIR}/curl-8.13.0" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_CURL_EXE=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF \
    -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_OPENSSL=ON \
    -DOPENSSL_ROOT_DIR="${PREFIX}" \
    -DOPENSSL_INCLUDE_DIR="${PREFIX}/include" \
    -DOPENSSL_CRYPTO_LIBRARY="${PREFIX}/lib/libcrypto.a" \
    -DOPENSSL_SSL_LIBRARY="${PREFIX}/lib/libssl.a" \
    -DCURL_ZLIB=ON \
    -DCURL_USE_LIBPSL=OFF \
    -DCURL_USE_LIBIDN2=OFF \
    -DUSE_NGHTTP2=OFF \
    -DCURL_BROTLI=OFF \
    -DCURL_ZSTD=OFF \
    -DCURL_DISABLE_LDAP=ON \
    -DCURL_DISABLE_LDAPS=ON \
    -DCURL_DISABLE_FTP=ON \
    -DCURL_DISABLE_TFTP=ON \
    -DCURL_DISABLE_TELNET=ON \
    -DCURL_DISABLE_DICT=ON \
    -DCURL_DISABLE_GOPHER=ON \
    -DCURL_DISABLE_RTSP=ON \
    -DCURL_DISABLE_POP3=ON \
    -DCURL_DISABLE_IMAP=ON \
    -DCURL_DISABLE_SMTP=ON \
    -DCURL_DISABLE_SMB=ON \
    -DCURL_DISABLE_MQTT=ON \
    -DCURL_DISABLE_WEBSOCKETS=OFF
  touch "${STAMP_DIR}/curl"
fi

# 4. opus 1.5.2 (static)
if [[ ! -f "${STAMP_DIR}/opus" ]]; then
  fetch "opus-1.5.2.tar.gz" "https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz"
  extract "opus-1.5.2.tar.gz" "opus-1.5.2"
  cmake_build "opus" "${SRC_DIR}/opus-1.5.2" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DOPUS_BUILD_PROGRAMS=OFF \
    -DOPUS_BUILD_SHARED_LIBRARY=OFF \
    -DOPUS_INSTALL_PKG_CONFIG_MODULE=ON
  touch "${STAMP_DIR}/opus"
fi

# 5. libsodium 1.0.20 (static)
if [[ ! -f "${STAMP_DIR}/libsodium" ]]; then
  fetch "libsodium-1.0.20.tar.gz" "https://download.libsodium.org/libsodium/releases/libsodium-1.0.20.tar.gz"
  extract "libsodium-1.0.20.tar.gz" "libsodium-1.0.20"
  rm -rf "${BUILD_DIR}/libsodium"
  mkdir -p "${BUILD_DIR}/libsodium"
  (
    cd "${BUILD_DIR}/libsodium"
    CFLAGS="-O2 -fPIC" "${SRC_DIR}/libsodium-1.0.20/configure" \
      --host="${TRIPLE}" \
      --prefix="${PREFIX}" \
      --enable-static --disable-shared --disable-soname-versions
    make -j"${JOBS}"
    make install
  )
  touch "${STAMP_DIR}/libsodium"
fi

# 6. speexdsp 1.2.1 (static, for voice NS/AEC/AGC)
if [[ ! -f "${STAMP_DIR}/speexdsp" ]]; then
  fetch "speexdsp-1.2.1.tar.gz" "https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz"
  extract "speexdsp-1.2.1.tar.gz" "speexdsp-1.2.1"
  rm -rf "${BUILD_DIR}/speexdsp"
  mkdir -p "${BUILD_DIR}/speexdsp"
  (
    cd "${BUILD_DIR}/speexdsp"
    CFLAGS="-O2 -fPIC" "${SRC_DIR}/speexdsp-1.2.1/configure" \
      --host="${TRIPLE}" \
      --prefix="${PREFIX}" \
      --enable-static --disable-shared --disable-examples
    make -j"${JOBS}"
    make install
  )
  touch "${STAMP_DIR}/speexdsp"
fi

# 7. libpng 1.6.47 (static)
if [[ ! -f "${STAMP_DIR}/libpng" ]]; then
  fetch "libpng-1.6.47.tar.xz" "https://download.sourceforge.net/libpng/libpng-1.6.47.tar.xz"
  extract "libpng-1.6.47.tar.xz" "libpng-1.6.47"
  cmake_build "libpng" "${SRC_DIR}/libpng-1.6.47" \
    -DPNG_SHARED=OFF \
    -DPNG_STATIC=ON \
    -DPNG_TESTS=OFF \
    -DPNG_TOOLS=OFF
  touch "${STAMP_DIR}/libpng"
fi

# 8. libjpeg-turbo 3.1.0 (static)
if [[ ! -f "${STAMP_DIR}/libjpeg-turbo" ]]; then
  fetch "libjpeg-turbo-3.1.0.tar.gz" "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz"
  extract "libjpeg-turbo-3.1.0.tar.gz" "libjpeg-turbo-3.1.0"
  cmake_build "libjpeg-turbo" "${SRC_DIR}/libjpeg-turbo-3.1.0" \
    -DENABLE_SHARED=OFF \
    -DENABLE_STATIC=ON \
    -DWITH_TURBOJPEG=OFF \
    -DWITH_TOOLS=OFF \
    -DWITH_TESTS=OFF
  touch "${STAMP_DIR}/libjpeg-turbo"
fi

# 9. libwebp 1.5.0 (static)
if [[ ! -f "${STAMP_DIR}/libwebp" ]]; then
  fetch "libwebp-1.5.0.tar.gz" "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.5.0.tar.gz"
  extract "libwebp-1.5.0.tar.gz" "libwebp-1.5.0"
  cmake_build "libwebp" "${SRC_DIR}/libwebp-1.5.0" \
    -DBUILD_SHARED_LIBS=OFF \
    -DWEBP_BUILD_ANIM_UTILS=OFF \
    -DWEBP_BUILD_CWEBP=OFF \
    -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF \
    -DWEBP_BUILD_IMG2WEBP=OFF \
    -DWEBP_BUILD_VWEBP=OFF \
    -DWEBP_BUILD_WEBPINFO=OFF \
    -DWEBP_BUILD_WEBPMUX=OFF \
    -DWEBP_BUILD_EXTRAS=OFF
  touch "${STAMP_DIR}/libwebp"
fi

# 10. FreeType 2.13.3 (static, with PNG color emoji support, before HarfBuzz & SDL3_ttf)
if [[ ! -f "${STAMP_DIR}/freetype" ]]; then
  fetch "freetype-2.13.3.tar.xz" "https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz"
  extract "freetype-2.13.3.tar.xz" "freetype-2.13.3"
  cmake_build "freetype" "${SRC_DIR}/freetype-2.13.3" \
    -DBUILD_SHARED_LIBS=OFF \
    -DFT_REQUIRE_ZLIB=ON \
    -DFT_REQUIRE_PNG=ON \
    -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_BROTLI=ON \
    -DFT_DISABLE_HARFBUZZ=ON \
    -DPNG_PNG_INCLUDE_DIR="${PREFIX}/include" \
    -DPNG_LIBRARY="${PREFIX}/lib/libpng.a"
  touch "${STAMP_DIR}/freetype"
fi

# 11. HarfBuzz 11.0.1 (static, for SDL3_ttf complex script & emoji shaping)
if [[ ! -f "${STAMP_DIR}/harfbuzz" ]]; then
  fetch "harfbuzz-11.0.1.tar.xz" "https://github.com/harfbuzz/harfbuzz/releases/download/11.0.1/harfbuzz-11.0.1.tar.xz"
  extract "harfbuzz-11.0.1.tar.xz" "harfbuzz-11.0.1"
  cmake_build "harfbuzz" "${SRC_DIR}/harfbuzz-11.0.1" \
    -DBUILD_SHARED_LIBS=OFF \
    -DHB_HAVE_FREETYPE=ON \
    -DHB_HAVE_GLIB=OFF \
    -DHB_HAVE_ICU=OFF \
    -DHB_BUILD_UTILS=OFF \
    -DHB_BUILD_SUBSET=OFF \
    -DHB_BUILD_TESTS=OFF \
    -DFREETYPE_INCLUDE_DIRS="${PREFIX}/include/freetype2" \
    -DFREETYPE_LIBRARY="${PREFIX}/lib/libfreetype.a"
  touch "${STAMP_DIR}/harfbuzz"
fi

# 12. SDL3 3.4.20 (SHARED libSDL3.so for Android SDLActivity + copy Java glue)
if [[ ! -f "${STAMP_DIR}/sdl3" ]]; then
  fetch "SDL3-3.4.20.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz"
  extract "SDL3-3.4.20.tar.gz" "SDL3-3.4.20"
  cmake_build "sdl3" "${SRC_DIR}/SDL3-3.4.20" \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF \
    -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384"
  mkdir -p "${DEPS_DIR}/sdl3-java"
  cp -a "${SRC_DIR}/SDL3-3.4.20/android-project/app/src/main/java/org" "${DEPS_DIR}/sdl3-java/"
  touch "${STAMP_DIR}/sdl3"
fi

# 13. SDL3_ttf 3.2.2 (static, linked against FreeType + HarfBuzz + SDL3)
if [[ ! -f "${STAMP_DIR}/sdl3_ttf" ]]; then
  fetch "SDL3_ttf-3.2.2.tar.gz" "https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-3.2.2.tar.gz"
  extract "SDL3_ttf-3.2.2.tar.gz" "SDL3_ttf-3.2.2"
  cmake_build "sdl3_ttf" "${SRC_DIR}/SDL3_ttf-3.2.2" \
    -DBUILD_SHARED_LIBS=OFF \
    -DSDLTTF_VENDORED=OFF \
    -DSDLTTF_SAMPLES=OFF \
    -DSDLTTF_HARFBUZZ=ON \
    -DSDLTTF_PLUTOSVG=OFF \
    -DFREETYPE_INCLUDE_DIRS="${PREFIX}/include/freetype2" \
    -DFREETYPE_LIBRARY="${PREFIX}/lib/libfreetype.a" \
    -DHarfBuzz_INCLUDE_DIR="${PREFIX}/include/harfbuzz" \
    -DHarfBuzz_LIBRARY="${PREFIX}/lib/libharfbuzz.a"
  touch "${STAMP_DIR}/sdl3_ttf"
fi

# 14. FFmpeg 7.1.1 (static, Android MediaCodec HW + libopus + core media codecs)
if [[ ! -f "${STAMP_DIR}/ffmpeg" ]]; then
  fetch "ffmpeg-7.1.1.tar.xz" "https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz"
  extract "ffmpeg-7.1.1.tar.xz" "ffmpeg-7.1.1"
  rm -rf "${BUILD_DIR}/ffmpeg"
  mkdir -p "${BUILD_DIR}/ffmpeg"
  (
    cd "${BUILD_DIR}/ffmpeg"
    # shellcheck disable=SC2086
    "${SRC_DIR}/ffmpeg-7.1.1/configure" \
      --prefix="${PREFIX}" \
      --target-os=android \
      --arch="${FFMPEG_ARCH}" \
      --cpu="${FFMPEG_CPU}" \
      --enable-cross-compile \
      --cc="${CC}" \
      --cxx="${CXX}" \
      --ar="${AR}" \
      --nm="${NM}" \
      --ranlib="${RANLIB}" \
      --strip="${STRIP}" \
      --sysroot="${SYSROOT}" \
      --pkg-config=pkg-config \
      --pkg-config-flags="--static" \
      --extra-cflags="-O2 -fPIC -I${PREFIX}/include" \
      --extra-ldflags="-L${PREFIX}/lib" \
      --extra-libs="-lm -lz" \
      --enable-pic \
      --enable-static \
      --disable-shared \
      --disable-programs \
      --disable-doc \
      --disable-debug \
      --disable-avdevice \
      --disable-avfilter \
      --disable-postproc \
      --disable-network \
      --disable-everything \
      --enable-avcodec \
      --enable-avformat \
      --enable-avutil \
      --enable-swscale \
      --enable-swresample \
      --enable-jni \
      --enable-mediacodec \
      --enable-libopus \
      --enable-decoder=h264,h264_mediacodec,hevc,hevc_mediacodec,mpeg4,mpeg4_mediacodec,vp8,vp8_mediacodec,vp9,vp9_mediacodec,gif,png,mjpeg,webp,mp3,aac,vorbis,opus,libopus,flac,pcm_s16le,pcm_s16be,pcm_u8,pcm_f32le,pcm_s24le,pcm_s32le \
      --enable-encoder=libopus,mpeg4,h264_mediacodec,png \
      --enable-muxer=ogg,opus,mp4,webm \
      --enable-demuxer=ogg,mp3,wav,mov,matroska,gif,image2,flac,aac,h264,mjpeg,webm_dash_manifest \
      --enable-parser=h264,hevc,mpeg4video,vp8,vp9,opus,vorbis,mpegaudio,aac,gif,png,mjpeg,webp \
      --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
      --enable-protocol=file,pipe \
      ${FFMPEG_EXTRA}
    make -j"${JOBS}"
    make install
  )
  touch "${STAMP_DIR}/ffmpeg"
fi

echo "==> Done! All Android dependencies for ${ABI} installed in ${PREFIX}"
