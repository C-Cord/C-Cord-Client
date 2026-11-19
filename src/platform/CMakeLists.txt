include(CCordDeps)
ccord_find_sdl()
find_package(PkgConfig REQUIRED)
pkg_check_modules(CCORD_AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(CCORD_AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(CCORD_SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(CCORD_SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
pkg_check_modules(CCORD_AVFORMAT REQUIRED IMPORTED_TARGET libavformat)

set(CCORD_PLATFORM_SOURCES
  platform.c
  audio_devices.c
  camera.c
  capture.c
  video_decode.c
  sound_decode.c
  sound_encode.c
  app_icon.c
  app_icon_data.c
  tray_icon.c
  link_watch.c)

if(WIN32)
  list(APPEND CCORD_PLATFORM_SOURCES desktop_win32.c capture_win32.c)
elseif(APPLE)
  list(APPEND CCORD_PLATFORM_SOURCES desktop_macos.m capture_macos.m)
else()
  # PipeWire is optional: portal video capture and the self-excluding Go Live audio
  # loopback use it when present; without it both fall back (portal reports unavailable,
  # loopback uses the parec/SDL whole-sink path).
  pkg_check_modules(CCORD_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
  pkg_check_modules(CCORD_XCAP QUIET IMPORTED_TARGET x11 xext xcomposite xrandr xfixes)
  pkg_check_modules(CCORD_GIO QUIET IMPORTED_TARGET gio-2.0)
  pkg_check_modules(CCORD_DBUS QUIET IMPORTED_TARGET dbus-1)
  list(APPEND CCORD_PLATFORM_SOURCES
    capture_x11.c
    capture_portal.c
    desktop_dbus.c
    portal_dbus.c
    portal_pw.c
    loopback_pw.c)
endif()

add_library(ccord_platform STATIC ${CCORD_PLATFORM_SOURCES})
add_library(ccord::platform ALIAS ccord_platform)
target_include_directories(ccord_platform PUBLIC include)
target_link_libraries(ccord_platform
  PUBLIC ccord_core ${CCORD_MATH_LIB}
  PRIVATE SDL3::SDL3 PkgConfig::CCORD_AVCODEC PkgConfig::CCORD_AVUTIL PkgConfig::CCORD_SWSCALE PkgConfig::CCORD_SWRESAMPLE PkgConfig::CCORD_AVFORMAT)

if(WIN32)
  target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_PIPEWIRE=0 CCORD_HAVE_X11=0 CCORD_HAVE_PORTAL=0 CCORD_HAVE_DBUS=0)
  target_link_libraries(ccord_platform PRIVATE shell32 user32 gdi32 dwmapi ole32 psapi)
elseif(APPLE)
  target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_PIPEWIRE=0 CCORD_HAVE_X11=0 CCORD_HAVE_PORTAL=0 CCORD_HAVE_DBUS=0)
  target_link_libraries(ccord_platform PRIVATE
    "-framework Cocoa"
    "-framework UserNotifications"
    "-framework CoreGraphics"
    "-framework CoreVideo"
    "-framework CoreMedia"
    "-framework ScreenCaptureKit")
else()
  if(CCORD_PIPEWIRE_FOUND)
    target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_PIPEWIRE=1)
    target_link_libraries(ccord_platform PRIVATE PkgConfig::CCORD_PIPEWIRE)
  else()
    target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_PIPEWIRE=0)
  endif()
  if(CCORD_XCAP_FOUND)
    target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_X11=1)
    target_link_libraries(ccord_platform PRIVATE PkgConfig::CCORD_XCAP)
  else()
    target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_X11=0)
  endif()
  if(CCORD_GIO_FOUND)
    target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_PORTAL=1)
    target_link_libraries(ccord_platform PRIVATE PkgConfig::CCORD_GIO)
  else()
    target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_PORTAL=0)
  endif()
  if(CCORD_DBUS_FOUND)
    target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_DBUS=1)
    target_link_libraries(ccord_platform PRIVATE PkgConfig::CCORD_DBUS)
  else()
    target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_DBUS=0)
  endif()
endif()
ccord_strict_warnings(ccord_platform)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_platform_desktop
    SOURCES tests/test_platform_desktop.c
    LIBS ccord_platform SDL3::SDL3
    LABELS unit)
  if(CCORD_GIO_FOUND)
    target_compile_definitions(test_platform_desktop PRIVATE CCORD_HAVE_PORTAL=1)
    target_link_libraries(test_platform_desktop PRIVATE PkgConfig::CCORD_GIO)
  else()
    target_compile_definitions(test_platform_desktop PRIVATE CCORD_HAVE_PORTAL=0)
  endif()
  # Resume-from-sleep / default-route change detection: pure feeds, no socket, no suspend.
  ccord_add_test(NAME test_link_watch
    SOURCES tests/test_link_watch.c
    LIBS ccord_platform SDL3::SDL3
    LABELS unit)
  if(UNIX AND NOT APPLE)
    # Self-excluding PipeWire loopback: pure node filter + fallback coverage.
    ccord_add_test(NAME test_loopback_pw
      SOURCES tests/test_loopback_pw.c
      LIBS ccord_platform SDL3::SDL3
      LABELS unit)
    # White-box access to loopback_pw.h (pure node filter).
    target_include_directories(test_loopback_pw PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(CCORD_PIPEWIRE_FOUND)
      target_compile_definitions(test_loopback_pw PRIVATE CCORD_HAVE_PIPEWIRE=1)
    else()
      target_compile_definitions(test_loopback_pw PRIVATE CCORD_HAVE_PIPEWIRE=0)
    endif()
  endif()
  # Voice-message Ogg/Opus encoder (round-trips through the soundboard decoder).
  ccord_add_test(NAME test_sound_encode
    SOURCES tests/test_sound_encode.c
    LIBS ccord_platform ${CCORD_MATH_LIB}
    LABELS unit)
  # M5 test fragments.
  include(tests/m5_devices.cmake)
  include(tests/m5_codecs.cmake)
  if(UNIX AND NOT APPLE)
    include(tests/m5_portal.cmake)
  endif()
endif()
