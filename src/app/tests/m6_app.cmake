# M6 desktop glue test fragment — owned by m6-app.
# Included from src/app/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/app.
# app_desktop.c is compiled in directly; the test links the real ui/platform/renderer/store/session
# (headless SDL) so the callbacks, tray badge, notification sink and hotkey intents are observed
# through their public seams.
ccord_add_test(NAME test_app_desktop SOURCES tests/test_app_desktop.c app_desktop.c
               LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS integration ui)
target_include_directories(test_app_desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# The Result 5/6 checks read the Advanced-page seam (settings_m6_prefs.h), like verify_m6.c.
target_include_directories(test_app_desktop PRIVATE ${CMAKE_SOURCE_DIR}/src/ui)
