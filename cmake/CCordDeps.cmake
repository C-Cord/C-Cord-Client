# SDL3 + SDL3_ttf, pinned in Pins.cmake. Call ccord_find_sdl() from src/platform only; the imported targets are GLOBAL.
#   CCORD_DEPS=system (default): find_package(... EXACT), configure fails on any other version.
#   CCORD_DEPS=fetch: download the pinned release tarballs (URL_HASH SHA256 checked) and build them
#   in the build tree; the ccord binary runs against that copy (RUNPATH points at _deps/).
set(CCORD_DEPS "system" CACHE STRING "Where SDL3 and SDL3_ttf come from: system or fetch")
set_property(CACHE CCORD_DEPS PROPERTY STRINGS system fetch)

macro(ccord_find_sdl)
  if(NOT TARGET SDL3::SDL3)
    if(CCORD_DEPS STREQUAL "system")
      if(CCORD_STRICT_PINS)
        find_package(SDL3 ${CCORD_PIN_SDL3_VERSION} EXACT CONFIG REQUIRED GLOBAL)
        find_package(SDL3_ttf ${CCORD_PIN_SDL3_TTF_VERSION} EXACT CONFIG REQUIRED GLOBAL)
      else()
        find_package(SDL3 3.2 CONFIG REQUIRED GLOBAL)
        find_package(SDL3_ttf 3.1 CONFIG REQUIRED GLOBAL)
      endif()
    elseif(CCORD_DEPS STREQUAL "fetch")
      include(FetchContent)
      # The release tarballs do not ship the FreeType/HarfBuzz submodules, so SDL3_ttf uses the
      # system copies of those two only; SDL3 itself and SDL3_ttf come from the pinned tarballs.
      set(SDLTTF_VENDORED OFF CACHE BOOL "" FORCE)
      # SYSTEM keeps third-party headers out of our strict-warning/`-Werror` diagnostics.
      FetchContent_Declare(SDL3 SYSTEM
        URL ${CCORD_PIN_SDL3_URL}
        URL_HASH SHA256=${CCORD_PIN_SDL3_SHA256})
      FetchContent_Declare(SDL3_ttf SYSTEM
        URL ${CCORD_PIN_SDL3_TTF_URL}
        URL_HASH SHA256=${CCORD_PIN_SDL3_TTF_SHA256})
      FetchContent_MakeAvailable(SDL3 SDL3_ttf)
      # project() versions stay in the subdirectory scope; mirror the pins here for the status line.
      set(SDL3_VERSION "${CCORD_PIN_SDL3_VERSION}")
      set(SDL3_ttf_VERSION "${CCORD_PIN_SDL3_TTF_VERSION}")
    else()
      message(FATAL_ERROR "CCORD_DEPS must be 'system' or 'fetch'")
    endif()
    message(STATUS "C-Cord: SDL3 ${SDL3_VERSION}, SDL3_ttf ${SDL3_ttf_VERSION} (${CCORD_DEPS})")
  endif()
endmacro()

# libcurl for the live net transport (REST v9 + Gateway v9 WebSocket). Call
# ccord_find_curl() from src/net; consumers of ccord_net inherit it through the
# link interface. The system copy must be built with the WebSocket API
# (curl_ws_*), checked by the transport test rather than a version pin because
# distributions rarely expose the feature bit.
macro(ccord_find_curl)
  if(NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
  endif()
  if(NOT TARGET CURL::libcurl)
    message(FATAL_ERROR "C-Cord needs libcurl with the WebSocket API (curl_ws_send/curl_ws_recv)")
  endif()
endmacro()

# zlib for Gateway zlib-stream transport compression (inflate). Call
# ccord_find_zlib() from src/net; consumers of ccord_net inherit it through the
# link interface.
macro(ccord_find_zlib)
  if(NOT TARGET ZLIB::ZLIB)
    find_package(ZLIB REQUIRED)
  endif()
  if(NOT TARGET ZLIB::ZLIB)
    message(FATAL_ERROR "C-Cord needs zlib for Gateway zlib-stream compression")
  endif()
endmacro()

# speexdsp for voice input processing (NS/AEC/AGC, M5). Optional: when the
# package is missing, ccord_net builds with CCORD_HAVE_SPEEXDSP=0 and the DSP
# chain is a no-op. Call ccord_find_speexdsp() from src/net under
# CCORD_LIVE_ACCOUNT, after find_package(PkgConfig).
macro(ccord_find_speexdsp)
  if(NOT TARGET PkgConfig::CCORD_SPEEXDSP)
    pkg_check_modules(CCORD_SPEEXDSP QUIET IMPORTED_TARGET speexdsp)
  endif()
endmacro()
