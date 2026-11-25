find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CCORD_PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(CCORD_JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(CCORD_WEBP REQUIRED IMPORTED_TARGET libwebp)
# Static libwebp (e.g. Android cross-build) pulls SharpYuv* from libsharpyuv.a when encoding WebP;
# libwebp.pc lists libsharpyuv under Requires.private, which non-static pkg-config queries omit.
set(CCORD_WEBP_EXTRA_LIBS "")
if(ANDROID OR ("${CCORD_WEBP_LINK_LIBRARIES}" MATCHES "\\.a(;|$)" AND NOT "${CCORD_WEBP_LINK_LIBRARIES}" MATCHES "\\.dll\\.a(;|$)"))
  pkg_check_modules(CCORD_SHARPYUV QUIET IMPORTED_TARGET libsharpyuv)
  if(CCORD_SHARPYUV_FOUND)
    list(APPEND CCORD_WEBP_EXTRA_LIBS PkgConfig::CCORD_SHARPYUV)
  else()
    find_library(CCORD_SHARPYUV_LIB NAMES sharpyuv)
    if(NOT CCORD_SHARPYUV_LIB)
      find_library(CCORD_SHARPYUV_LIB NAMES sharpyuv HINTS ${CCORD_WEBP_LIBRARY_DIRS} NO_CMAKE_FIND_ROOT_PATH)
    endif()
    if(CCORD_SHARPYUV_LIB)
      list(APPEND CCORD_WEBP_EXTRA_LIBS ${CCORD_SHARPYUV_LIB})
    endif()
  endif()
endif()
pkg_check_modules(CCORD_AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(CCORD_AVCODEC_R REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(CCORD_AVUTIL_R REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(CCORD_SWSCALE_R REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(CCORD_SWRESAMPLE_R REQUIRED IMPORTED_TARGET libswresample)

# fontconfig finds the installed font for a code point the bundled fonts lack. Optional: without it (or with
# -DCCORD_FONTCONFIG=OFF) text.c scans the standard font directories instead.
option(CCORD_FONTCONFIG "Use fontconfig for run-time font fallback when available" ON)
set(CCORD_RENDER_HAVE_FONTCONFIG 0)
if(CCORD_FONTCONFIG AND NOT ANDROID)
  pkg_check_modules(CCORD_FONTCONFIG_PKG QUIET IMPORTED_TARGET fontconfig)
  if(CCORD_FONTCONFIG_PKG_FOUND)
    set(CCORD_RENDER_HAVE_FONTCONFIG 1)
  endif()
endif()

add_library(ccord_render STATIC render.c image_decode.c texture_cache.c text.c anim.c av_stream.c lottie.c)
add_library(ccord::render ALIAS ccord_render)
target_include_directories(ccord_render PUBLIC include)
target_include_directories(ccord_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccord_render
  PUBLIC ccord_core ccord_platform ccord_theme ${CCORD_MATH_LIB}
  PRIVATE Threads::Threads SDL3::SDL3 SDL3_ttf::SDL3_ttf PkgConfig::CCORD_PNG PkgConfig::CCORD_JPEG
  PkgConfig::CCORD_WEBP ${CCORD_WEBP_EXTRA_LIBS}
  PkgConfig::CCORD_AVFORMAT PkgConfig::CCORD_AVCODEC_R PkgConfig::CCORD_AVUTIL_R PkgConfig::CCORD_SWSCALE_R
  PkgConfig::CCORD_SWRESAMPLE_R)
target_compile_definitions(ccord_render PRIVATE CCORD_HAVE_FONTCONFIG=${CCORD_RENDER_HAVE_FONTCONFIG})
if(CCORD_RENDER_HAVE_FONTCONFIG)
  target_link_libraries(ccord_render PRIVATE PkgConfig::CCORD_FONTCONFIG_PKG)
endif()
ccord_strict_warnings(ccord_render)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_render
    SOURCES tests/test_render.c
    LIBS ccord_render ccord_theme SDL3::SDL3 PkgConfig::CCORD_PNG PkgConfig::CCORD_JPEG
    PkgConfig::CCORD_WEBP ${CCORD_WEBP_EXTRA_LIBS}
    LABELS ui)
  ccord_add_test(NAME test_anim
    SOURCES tests/test_anim.c
    LIBS ccord_render ccord_core
    LABELS unit)
  ccord_add_test(NAME test_av_source
    SOURCES tests/test_av_source.c
    LIBS ccord_render ccord_core Threads::Threads
    LABELS unit)
endif()
