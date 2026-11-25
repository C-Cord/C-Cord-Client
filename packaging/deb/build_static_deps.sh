#!/usr/bin/env bash
# Build minimal static Linux dependencies for the cross-distro C-Cord .deb package.
#
# Bundles ONLY the libraries whose SONAMEs or required features diverge across
# Debian 12/13 and Ubuntu 24.04/26.04 (FFmpeg, libjpeg-turbo, libcurl with WebSockets),
# while keeping core system libraries (OpenSSL 3, zlib, nghttp2, Opus, libsodium,
# SpeexDSP, libpng, libwebp, PipeWire, GLib, DBus, X11, VA-API, FreeType/HarfBuzz)
# dynamically linked to the host distribution.
#
# Usage:
#   ./packaging/deb/build_static_deps.sh [prefix]
set -euo pipefail

PREFIX="${1:-${HOME}/.cache/ccord-deb-deps}"
CC="${CC:-clang-19}"
CXX="${CXX:-clang++-19}"
JOBS="${JOBS:-$(nproc)}"

WORK_DIR="/tmp/ccord-deb-deps-work"
DL_DIR="${WORK_DIR}/downloads"
SRC_DIR="${WORK_DIR}/src"
BUILD_DIR="${WORK_DIR}/build"
STAMP_DIR="${PREFIX}/stamps"

export CC CXX
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

mkdir -p "${DL_DIR}" "${SRC_DIR}" "${BUILD_DIR}" "${PREFIX}/lib/pkgconfig" "${PREFIX}/include" "${STAMP_DIR}"

fetch() {
  local name="$1" url="$2" sha256="$3"
  local out="${DL_DIR}/${name}"
  if [[ ! -s "${out}" ]]; then
    echo "==> Downloading ${name}"
    curl -fsSL --retry 3 --retry-delay 5 -o "${out}.tmp" "${url}"
    mv "${out}.tmp" "${out}"
  fi
  echo "${sha256}  ${out}" | sha256sum -c -
}

extract() {
  local archive="$1" dir="$2"
  if [[ ! -d "${SRC_DIR}/${dir}" ]]; then
    echo "==> Extracting ${archive}"
    tar -xf "${DL_DIR}/${archive}" -C "${SRC_DIR}"
  fi
}

echo "============================================================"
echo "C-Cord Linux Static .deb Dependencies"
echo "  Prefix: ${PREFIX}"
echo "  CC:     ${CC}"
echo "  CXX:    ${CXX}"
echo "  Jobs:   ${JOBS}"
echo "============================================================"

# 1. libcurl 8.14.1 (static, WebSockets enabled, linked to system OpenSSL 3 + zlib + nghttp2)
if [[ ! -f "${STAMP_DIR}/curl-8.14.1" ]]; then
  fetch "curl-8.14.1.tar.xz" \
    "https://github.com/curl/curl/releases/download/curl-8_14_1/curl-8.14.1.tar.xz" \
    "f4619a1e2474c4bbfedc88a7c2191209c8334b48fa1f4e53fd584cc12e9120dd"
  extract "curl-8.14.1.tar.xz" "curl-8.14.1"
  rm -rf "${BUILD_DIR}/curl"
  cmake -S "${SRC_DIR}/curl-8.14.1" -B "${BUILD_DIR}/curl" -G Ninja \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_STATIC_LIBS=ON \
    -DBUILD_CURL_EXE=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF \
    -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_PKGCONFIG=OFF \
    -DCURL_USE_OPENSSL=ON \
    -DCURL_ZLIB=ON \
    -DUSE_NGHTTP2=ON \
    -DCURL_USE_LIBPSL=OFF \
    -DCURL_USE_LIBSSH2=OFF \
    -DUSE_LIBIDN2=OFF \
    -DCURL_BROTLI=OFF \
    -DCURL_ZSTD=OFF \
    -DCURL_DISABLE_WEBSOCKETS=OFF \
    -DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt \
    -DCURL_CA_PATH=/etc/ssl/certs \
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
    -DCURL_DISABLE_MQTT=ON
  cmake --build "${BUILD_DIR}/curl" --parallel "${JOBS}"
  cmake --install "${BUILD_DIR}/curl"
  touch "${STAMP_DIR}/curl-8.14.1"
fi

# 2. libjpeg-turbo 3.1.0 (static, SIMD-accelerated; resolves Debian libjpeg.so.62 vs Ubuntu libjpeg.so.8 split)
if [[ ! -f "${STAMP_DIR}/libjpeg-turbo-3.1.0" ]]; then
  fetch "libjpeg-turbo-3.1.0.tar.gz" \
    "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz" \
    "9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93"
  extract "libjpeg-turbo-3.1.0.tar.gz" "libjpeg-turbo-3.1.0"
  rm -rf "${BUILD_DIR}/libjpeg-turbo"
  cmake -S "${SRC_DIR}/libjpeg-turbo-3.1.0" -B "${BUILD_DIR}/libjpeg-turbo" -G Ninja \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DENABLE_SHARED=OFF \
    -DENABLE_STATIC=ON \
    -DWITH_TURBOJPEG=OFF \
    -DWITH_TOOLS=OFF \
    -DWITH_TESTS=OFF
  cmake --build "${BUILD_DIR}/libjpeg-turbo" --parallel "${JOBS}"
  cmake --install "${BUILD_DIR}/libjpeg-turbo"
  touch "${STAMP_DIR}/libjpeg-turbo-3.1.0"
fi

# 3. nv-codec-headers 12.2.72.0 (header-only; enables runtime-dlopen h264_nvenc / av1_nvenc with zero link-time NVIDIA dep)
if [[ ! -f "${STAMP_DIR}/nv-codec-headers-12.2.72.0" ]]; then
  fetch "nv-codec-headers-12.2.72.0.tar.gz" \
    "https://github.com/FFmpeg/nv-codec-headers/releases/download/n12.2.72.0/nv-codec-headers-12.2.72.0.tar.gz" \
    "c295a2ba8a06434d4bdc5c2208f8a825285210d71d91d572329b2c51fd0d4d03"
  extract "nv-codec-headers-12.2.72.0.tar.gz" "nv-codec-headers-12.2.72.0"
  make -C "${SRC_DIR}/nv-codec-headers-12.2.72.0" PREFIX="${PREFIX}" LIBDIR=lib install
  touch "${STAMP_DIR}/nv-codec-headers-12.2.72.0"
fi

# 4. libvpx 1.15.0 (static, decoder-only; required for transparent WebM VP8/VP9 alpha stickers & animations)
if [[ ! -f "${STAMP_DIR}/libvpx-1.15.0" ]]; then
  fetch "libvpx-1.15.0.tar.gz" \
    "https://github.com/webmproject/libvpx/archive/refs/tags/v1.15.0.tar.gz" \
    "e935eded7d81631a538bfae703fd1e293aad1c7fd3407ba00440c95105d2011e"
  extract "libvpx-1.15.0.tar.gz" "libvpx-1.15.0"
  rm -rf "${BUILD_DIR}/libvpx"
  mkdir -p "${BUILD_DIR}/libvpx"
  (
    cd "${BUILD_DIR}/libvpx"
    LD="${CC}" "${SRC_DIR}/libvpx-1.15.0/configure" \
      --prefix="${PREFIX}" \
      --libdir="${PREFIX}/lib" \
      --target=x86_64-linux-gcc \
      --as=nasm \
      --enable-pic \
      --enable-static \
      --disable-shared \
      --disable-examples \
      --disable-tools \
      --disable-docs \
      --disable-unit-tests \
      --disable-vp8-encoder \
      --disable-vp9-encoder \
      --enable-vp8-decoder \
      --enable-vp9-decoder
    make -j"${JOBS}"
    make install
  )
  touch "${STAMP_DIR}/libvpx-1.15.0"
fi

# 5. dav1d 1.5.1 (static, fast software AV1 decoder)
if [[ ! -f "${STAMP_DIR}/dav1d-1.5.1" ]]; then
  fetch "dav1d-1.5.1.tar.gz" \
    "https://code.videolan.org/videolan/dav1d/-/archive/1.5.1/dav1d-1.5.1.tar.gz" \
    "fa635e2bdb25147b1384007c83e15de44c589582bb3b9a53fc1579cb9d74b695"
  extract "dav1d-1.5.1.tar.gz" "dav1d-1.5.1"
  rm -rf "${BUILD_DIR}/dav1d"
  meson setup "${BUILD_DIR}/dav1d" "${SRC_DIR}/dav1d-1.5.1" \
    --prefix="${PREFIX}" \
    --libdir=lib \
    --default-library=static \
    --buildtype=release \
    -Denable_tools=false \
    -Denable_tests=false \
    -Denable_examples=false \
    -Denable_docs=false
  ninja -C "${BUILD_DIR}/dav1d" -j"${JOBS}"
  ninja -C "${BUILD_DIR}/dav1d" install
  touch "${STAMP_DIR}/dav1d-1.5.1"
fi

# 6. x264 stable (static, H.264 software encoder for Go Live screen sharing when HW encode isn't used)
X264_COMMIT="31e19f92f00c7003fa115047ce50978bc98c3a0d"
if [[ ! -f "${STAMP_DIR}/x264-${X264_COMMIT}" ]]; then
  fetch "x264-${X264_COMMIT}.tar.bz2" \
    "https://code.videolan.org/videolan/x264/-/archive/${X264_COMMIT}/x264-${X264_COMMIT}.tar.bz2" \
    "01a4acb74eea1118c3aa96c3e18ee3384bc1f2bc670f31fb0f63be853e4d9d08"
  extract "x264-${X264_COMMIT}.tar.bz2" "x264-${X264_COMMIT}"
  rm -rf "${BUILD_DIR}/x264"
  mkdir -p "${BUILD_DIR}/x264"
  (
    cd "${BUILD_DIR}/x264"
    "${SRC_DIR}/x264-${X264_COMMIT}/configure" \
      --prefix="${PREFIX}" \
      --libdir="${PREFIX}/lib" \
      --enable-static \
      --disable-shared \
      --enable-pic \
      --disable-cli \
      --disable-opencl \
      --disable-avs \
      --disable-swscale \
      --disable-lavf \
      --disable-ffms \
      --disable-gpac \
      --disable-lsmash
    make -j"${JOBS}"
    make install
  )
  touch "${STAMP_DIR}/x264-${X264_COMMIT}"
fi

# 7. FFmpeg 7.1.1 (minimal static build containing only the codecs, demuxers, muxers, and HW accel C-Cord uses)
if [[ ! -f "${STAMP_DIR}/ffmpeg-7.1.1" ]]; then
  fetch "ffmpeg-7.1.1.tar.xz" \
    "https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz" \
    "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1"
  extract "ffmpeg-7.1.1.tar.xz" "ffmpeg-7.1.1"
  rm -rf "${BUILD_DIR}/ffmpeg"
  mkdir -p "${BUILD_DIR}/ffmpeg"
  (
    cd "${BUILD_DIR}/ffmpeg"
    "${SRC_DIR}/ffmpeg-7.1.1/configure" \
      --prefix="${PREFIX}" \
      --libdir="${PREFIX}/lib" \
      --cc="${CC}" \
      --cxx="${CXX}" \
      --pkg-config=pkg-config \
      --pkg-config-flags="--static" \
      --extra-cflags="-O2 -fPIC -I${PREFIX}/include" \
      --extra-ldflags="-L${PREFIX}/lib" \
      --enable-pic \
      --enable-static \
      --disable-shared \
      --enable-gpl \
      --enable-version3 \
      --disable-programs \
      --disable-doc \
      --disable-debug \
      --disable-avdevice \
      --disable-avfilter \
      --disable-postproc \
      --disable-network \
      --disable-xlib \
      --disable-vdpau \
      --disable-vulkan \
      --disable-cuda-llvm \
      --disable-sdl2 \
      --disable-sndio \
      --disable-alsa \
      --disable-lzma \
      --disable-bzlib \
      --disable-everything \
      --enable-avcodec \
      --enable-avformat \
      --enable-avutil \
      --enable-swscale \
      --enable-swresample \
      --enable-zlib \
      --enable-libopus \
      --enable-libx264 \
      --enable-libvpx \
      --enable-libdav1d \
      --enable-ffnvcodec \
      --enable-nvenc \
      --enable-vaapi \
      --enable-decoder=h264,hevc,vp8,vp9,libvpx_vp8,libvpx_vp9,libdav1d,av1,mpeg4,gif,apng,png,webp,mjpeg,bmp,tiff,mp3,mp3float,aac,aac_latm,vorbis,opus,libopus,flac,pcm_s16le,pcm_s16be,pcm_u8,pcm_f32le,pcm_f32be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_alaw,pcm_mulaw \
      --enable-encoder=libopus,libx264,h264_nvenc,av1_nvenc,h264_vaapi,av1_vaapi,mpeg4,png \
      --enable-muxer=ogg,opus,mp4,webm \
      --enable-demuxer=gif,apng,image_webp_pipe,image_png_pipe,image_jpeg_pipe,image_bmp_pipe,image_tiff_pipe,image2,wav,ogg,mp3,flac,mov,matroska,aac,h264,hevc,mjpeg \
      --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,opus,vorbis,mpegaudio,aac,aac_latm,flac,gif,png,webp,mjpeg,bmp \
      --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,vp9_superframe \
      --enable-protocol=file,pipe
    make -j"${JOBS}"
    make install
  )

  # Flatten Requires.private -> Requires and Libs.private -> Libs in the installed static .pc files
  # so CMake's pkg_check_modules() (which invokes pkg-config without --static) links the full static closure.
  python3 - "${PREFIX}/lib/pkgconfig" <<'PY'
import os, sys

pc_dir = sys.argv[1]
for name in ("libavutil.pc", "libswresample.pc", "libswscale.pc", "libavcodec.pc", "libavformat.pc"):
    path = os.path.join(pc_dir, name)
    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    reqs, reqs_priv = "", ""
    libs, libs_priv = "", ""
    out = []
    for line in lines:
        if line.startswith("Requires:"):
            reqs = line.split(":", 1)[1].strip()
        elif line.startswith("Requires.private:"):
            reqs_priv = line.split(":", 1)[1].strip()
        elif line.startswith("Libs:"):
            libs = line.split(":", 1)[1].strip()
        elif line.startswith("Libs.private:"):
            libs_priv = line.split(":", 1)[1].strip()
        else:
            out.append(line)
    merged_reqs = ", ".join(x for x in (reqs, reqs_priv) if x)
    merged_libs = " ".join(x for x in (libs, libs_priv) if x)
    if name == "libavutil.pc" and "-ldl" not in merged_libs:
        merged_libs += " -ldl -lm -lpthread"
    if merged_reqs:
        out.append(f"Requires: {merged_reqs}")
    out.append(f"Libs: {merged_libs}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print(f"Flattened {name}: Requires=[{merged_reqs}] Libs=[{merged_libs}]")
PY
  touch "${STAMP_DIR}/ffmpeg-7.1.1"
fi

# Verify static libcurl has WebSockets (wss) compiled in
cat <<'EOF' > "${WORK_DIR}/check_curl_ws.c"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
  if (!info || !info->protocols) return 1;
  int has_wss = 0;
  for (const char *const *p = info->protocols; *p; p++) {
    if (strcmp(*p, "wss") == 0) has_wss = 1;
  }
  printf("Static libcurl %s (wss=%s)\n", info->version, has_wss ? "yes" : "no");
  return has_wss ? 0 : 1;
}
EOF
"${CC}" -DCURL_STATICLIB -I"${PREFIX}/include" "${WORK_DIR}/check_curl_ws.c" \
  "${PREFIX}/lib/libcurl.a" -lssl -lcrypto -lz -lnghttp2 -lpthread -o "${WORK_DIR}/check_curl_ws"
"${WORK_DIR}/check_curl_ws"

# Verify pkg-config resolves our static FFmpeg + libjpeg cleanly
pkg-config --modversion libavformat libavcodec libavutil libswscale libswresample libjpeg
rm -rf "${WORK_DIR}"
echo "==> Done! Static .deb dependencies installed in ${PREFIX}"
