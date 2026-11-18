# M6 UI/IME test fragment — owned by m6-ui-ime.
# Included from src/ui/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/ui.
ccord_add_test(NAME test_ui_ime SOURCES tests/test_ui_ime.c
               LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
