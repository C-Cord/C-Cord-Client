# M5 test fragment — owned by m5-ui-settings.
# Included from src/ui/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/ui.
ccord_add_test(NAME test_ui_voice_settings SOURCES tests/test_ui_voice_settings.c
               LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
