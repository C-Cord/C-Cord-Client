include(CCordDeps)
ccord_find_curl()
ccord_find_zlib()
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

add_library(ccord_web STATIC
  web_view.c
  web_net.c
  web_wasm.c
  web_canvas.c
  web_dom.c
  web_js_env.c
  web_layout_paint.c)
add_library(ccord::web ALIAS ccord_web)

target_include_directories(ccord_web
  PUBLIC include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ccord_web
  PUBLIC ccord_core ccord_render ccord_platform
  PRIVATE ccord_quickjs ccord_lexbor ccord_wasm3 CURL::libcurl ZLIB::ZLIB OpenSSL::Crypto ${CCORD_MATH_LIB})

ccord_strict_warnings(ccord_web)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_web_engine
    SOURCES tests/test_web_engine.c
    LIBS ccord_web ccord_theme
    LABELS unit)
endif()
