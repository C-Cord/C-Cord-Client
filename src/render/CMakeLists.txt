find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CCORD_PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(CCORD_JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(CCORD_WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(CCORD_AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(CCORD_AVCODEC_R REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(CCORD_AVUTIL_R REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(CCORD_SWSCALE_R REQUIRED IMPORTED_TARGET libswscale)

# fontconfig finds the installed font for a code point the bundled fonts lack. Optional: without it (or with
# -DCCORD_FONTCONFIG=OFF) text.c scans the standard font directories instead.
option(CCORD_FONTCONFIG "Use fontconfig for run-time font fallback when available" ON)
set(CCORD_RENDER_HAVE_FONTCONFIG 0)
if(CCORD_FONTCONFIG)
  pkg_check_modules(CCORD_FONTCONFIG_PKG QUIET IMPORTED_TARGET fontconfig)
  if(CCORD_FONTCONFIG_PKG_FOUND)
    set(CCORD_RENDER_HAVE_FONTCONFIG 1)
  endif()
endif()

add_library(ccord_render STATIC render.c text.c anim.c av_stream.c)
add_library(ccord::render ALIAS ccord_render)
target_include_directories(ccord_render PUBLIC include)
target_include_directories(ccord_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccord_render
  PUBLIC ccord_core ccord_platform ccord_theme m
  PRIVATE Threads::Threads SDL3::SDL3 SDL3_ttf::SDL3_ttf PkgConfig::CCORD_PNG PkgConfig::CCORD_JPEG PkgConfig::CCORD_WEBP
  PkgConfig::CCORD_AVFORMAT PkgConfig::CCORD_AVCODEC_R PkgConfig::CCORD_AVUTIL_R PkgConfig::CCORD_SWSCALE_R)
target_compile_definitions(ccord_render PRIVATE CCORD_HAVE_FONTCONFIG=${CCORD_RENDER_HAVE_FONTCONFIG})
if(CCORD_RENDER_HAVE_FONTCONFIG)
  target_link_libraries(ccord_render PRIVATE PkgConfig::CCORD_FONTCONFIG_PKG)
endif()
ccord_strict_warnings(ccord_render)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_render
    SOURCES tests/test_render.c
    LIBS ccord_render ccord_theme SDL3::SDL3 PkgConfig::CCORD_PNG PkgConfig::CCORD_JPEG PkgConfig::CCORD_WEBP
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
