include(CCordDeps)
ccord_find_sdl()
find_package(PkgConfig REQUIRED)
pkg_check_modules(CCORD_AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(CCORD_AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(CCORD_SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(CCORD_SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
pkg_check_modules(CCORD_AVFORMAT REQUIRED IMPORTED_TARGET libavformat)

set(CCORD_PLATFORM_SOURCES
  window/platform.c
  window/platform_events.c
  window/link_watch.c
  audio/audio_stream.c
  audio/audio_devices.c
  audio/loopback.c
  audio/sound_decode.c
  audio/sound_encode.c
  video/video_stream.c
  video/video_decode.c
  video/camera.c
  capture/capture.c
  icons/app_icon.c
  icons/app_icon_data.c
  icons/tray_icon.c)

if(WIN32)
  list(APPEND CCORD_PLATFORM_SOURCES desktop/desktop_win32.c capture/capture_win32.c)
elseif(APPLE)
  list(APPEND CCORD_PLATFORM_SOURCES desktop/desktop_macos.m capture/capture_macos.m)
elseif(ANDROID)
  list(APPEND CCORD_PLATFORM_SOURCES desktop/desktop_android.c capture/capture_android.c)
else()
  pkg_check_modules(CCORD_PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
  pkg_check_modules(CCORD_XCAP QUIET IMPORTED_TARGET x11 xext xcomposite xrandr xfixes)
  pkg_check_modules(CCORD_GIO QUIET IMPORTED_TARGET gio-2.0)
  pkg_check_modules(CCORD_DBUS QUIET IMPORTED_TARGET dbus-1)
  list(APPEND CCORD_PLATFORM_SOURCES
    capture/capture_x11.c
    capture/capture_portal.c
    capture/portal_dbus.c
    capture/portal_pw.c
    desktop/desktop_dbus.c
    desktop/desktop_dbus_sni.c
    desktop/desktop_dbus_shortcuts.c
    audio/loopback_pw.c)
endif()

add_library(ccord_platform STATIC ${CCORD_PLATFORM_SOURCES})
add_library(ccord::platform ALIAS ccord_platform)
target_include_directories(ccord_platform PUBLIC include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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
elseif(ANDROID)
  target_compile_definitions(ccord_platform PRIVATE CCORD_HAVE_PIPEWIRE=0 CCORD_HAVE_X11=0 CCORD_HAVE_PORTAL=0 CCORD_HAVE_DBUS=0)
  target_link_libraries(ccord_platform PRIVATE android log)
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

  ccord_add_test(NAME test_link_watch
    SOURCES tests/test_link_watch.c
    LIBS ccord_platform SDL3::SDL3
    LABELS unit)

  if(UNIX AND NOT APPLE AND NOT ANDROID)
    ccord_add_test(NAME test_loopback_pw
      SOURCES tests/test_loopback_pw.c
      LIBS ccord_platform SDL3::SDL3
      LABELS unit)
    target_include_directories(test_loopback_pw PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/audio")
    if(CCORD_PIPEWIRE_FOUND)
      target_compile_definitions(test_loopback_pw PRIVATE CCORD_HAVE_PIPEWIRE=1)
    else()
      target_compile_definitions(test_loopback_pw PRIVATE CCORD_HAVE_PIPEWIRE=0)
    endif()
  endif()

  ccord_add_test(NAME test_sound_encode
    SOURCES tests/test_sound_encode.c
    LIBS ccord_platform ${CCORD_MATH_LIB}
    LABELS unit)

  ccord_add_test(NAME test_platform_devices
    SOURCES tests/test_platform_devices.c
    LIBS ccord_platform SDL3::SDL3
    LABELS unit)
  target_include_directories(test_platform_devices PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/audio")

  ccord_add_test(NAME test_video_decode
    SOURCES tests/test_video_decode.c
    LIBS ccord_platform
    LABELS unit)

  ccord_add_test(NAME test_sound_decode
    SOURCES tests/test_sound_decode.c
    LIBS ccord_platform
    LABELS unit)

  if(UNIX AND NOT APPLE AND NOT ANDROID)
    ccord_add_test(NAME test_portal_dbus
      SOURCES tests/test_portal_dbus.c
      LIBS ccord_platform
      LABELS unit)
    target_include_directories(test_portal_dbus PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/capture")
    if(CCORD_DBUS_FOUND)
      target_compile_definitions(test_portal_dbus PRIVATE CCORD_HAVE_DBUS=1)
      target_link_libraries(test_portal_dbus PRIVATE PkgConfig::CCORD_DBUS)
    else()
      target_compile_definitions(test_portal_dbus PRIVATE CCORD_HAVE_DBUS=0)
    endif()

    ccord_add_test(NAME test_capture_portal
      SOURCES tests/test_capture_portal.c
      LIBS ccord_platform
      LABELS unit)
    target_include_directories(test_capture_portal PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/capture")
    if(CCORD_DBUS_FOUND)
      target_compile_definitions(test_capture_portal PRIVATE CCORD_HAVE_DBUS=1)
      target_link_libraries(test_capture_portal PRIVATE PkgConfig::CCORD_DBUS)
    else()
      target_compile_definitions(test_capture_portal PRIVATE CCORD_HAVE_DBUS=0)
    endif()
    if(CCORD_PIPEWIRE_FOUND)
      target_compile_definitions(test_capture_portal PRIVATE CCORD_HAVE_PIPEWIRE=1)
    else()
      target_compile_definitions(test_capture_portal PRIVATE CCORD_HAVE_PIPEWIRE=0)
    endif()
  endif()
endif()
