find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CCORD_EMBEDDED_ASSETS_C "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.c")
set(CCORD_EMBEDDED_INPUTS
  "${CMAKE_SOURCE_DIR}/scripts/gen_embedded_assets.py"
  "${CMAKE_SOURCE_DIR}/assets/themes/c-cord-dark.theme"
  "${CMAKE_SOURCE_DIR}/assets/themes/c-cord-light.theme"
  "${CMAKE_SOURCE_DIR}/assets/themes/c-cord-amoled.theme"
  "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf"
  "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Bold.ttf"
  "${CMAKE_SOURCE_DIR}/extensions/copy-message-id/plugin.json"
  "${CMAKE_SOURCE_DIR}/extensions/copy-message-id/main.lua")

add_custom_command(
  OUTPUT "${CCORD_EMBEDDED_ASSETS_C}"
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/scripts/gen_embedded_assets.py"
    "${CMAKE_SOURCE_DIR}" "${CCORD_EMBEDDED_ASSETS_C}"
  DEPENDS ${CCORD_EMBEDDED_INPUTS}
  VERBATIM
  COMMENT "Generating embedded assets for single-binary ccord")

add_library(ccord_assets STATIC "${CCORD_EMBEDDED_ASSETS_C}")
add_library(ccord::assets ALIAS ccord_assets)
target_include_directories(ccord_assets PUBLIC include)
target_link_libraries(ccord_assets PUBLIC ccord_core)
ccord_strict_warnings(ccord_assets)
