# M6 account settings pages test fragment — owned by m6-ui-devices.
# Included from src/ui/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/ui.
ccord_add_test(NAME test_ui_settings_devices SOURCES tests/test_ui_settings_devices.c
               LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
# M6 preference pages (Notifications/Accessibility/Streamer/Language/Advanced + Appearance theme/zoom).
ccord_add_test(NAME test_ui_user_settings SOURCES tests/test_ui_user_settings.c
               LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
