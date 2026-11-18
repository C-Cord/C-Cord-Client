# m5-portal: ScreenCast DBus negotiation (portal_dbus) + PipeWire frame capture (portal_pw).
# Owned by m5-portal
# registered in the production CMakeLists (capture_portal.c links them in every build);
# this fragment only registers the tests.
ccord_add_test(NAME test_portal_dbus
  SOURCES tests/test_portal_dbus.c
  LIBS ccord_platform
  LABELS unit)
# White-box access to portal_dbus.h (internal seam header, lives next to the sources).
target_include_directories(test_portal_dbus PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
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
# White-box access to portal_dbus.h / portal_pw.h (internal seam headers).
target_include_directories(test_capture_portal PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
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
