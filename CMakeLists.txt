cmake_minimum_required(VERSION 3.25)

set(CCORD_VERSION "0.1.0" CACHE STRING "C-Cord version string (e.g. 0.2.0, v0.2.0-rc.1, or 0.1.0+git)")
string(REGEX REPLACE "^v([0-9])" "\\1" CCORD_VERSION_CLEAN "${CCORD_VERSION}")
if(CCORD_VERSION_CLEAN STREQUAL "" OR CCORD_VERSION_CLEAN MATCHES "[\"\\\\\r\n]")
  message(FATAL_ERROR "Invalid CCORD_VERSION: '${CCORD_VERSION}'")
endif()
if(CCORD_VERSION_CLEAN MATCHES "^([0-9]+(\\.[0-9]+){0,3})")
  set(CCORD_CMAKE_VERSION "${CMAKE_MATCH_1}")
else()
  set(CCORD_CMAKE_VERSION "0.1.0")
endif()

project(ccord VERSION ${CCORD_CMAKE_VERSION} LANGUAGES C)
if(APPLE)
  enable_language(OBJC)
  add_compile_definitions(_DARWIN_C_SOURCE)
endif()
if(MSVC)
  set(CCORD_MATH_LIB "")
else()
  set(CCORD_MATH_LIB m)
endif()
message(STATUS "C-Cord: version ${CCORD_VERSION_CLEAN}")

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CCordC23)
include(Pins)
include(CCordWarnings)
include(CCordSanitize)
include(CCordTest)
include(CCordFuzz)

option(CCORD_WERROR "Treat warnings as errors" ON)
option(CCORD_BUILD_TESTS "Build unit tests" ON)
option(CCORD_LIVE_ACCOUNT "Build live account transport (P4+); OFF keeps the build offline" ON)
option(CCORD_STRICT_PINS "Require exact pinned system versions of SDL3 and SDL3_ttf" OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

ccord_check_c23()
ccord_sanitize_setup()
ccord_fuzz_setup()

if(UNIX AND NOT APPLE AND NOT CCORD_SANITIZE)
  add_link_options(-Wl,-z,relro,-z,now -Wl,-z,noexecstack)
endif()
if(NOT CCORD_BUILD_TESTS)
  add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
endif()

if(CCORD_BUILD_TESTS)
  enable_testing()
endif()

set(CCORD_ASSET_DIR "${CMAKE_SOURCE_DIR}/assets")

# Third-party code: official Lua 5.5.1, pinned by sha256 in cmake/Pins.cmake.
add_subdirectory(third_party/lua)
# Discord DAVE voice E2EE (discord/libdave + cisco/mlspp, C++17, pinned in cmake/Pins.cmake). Required by
# Discord voice since 2026-03-01; only the live transport links it.
option(CCORD_DAVE "Build Discord DAVE voice E2EE (libdave + mlspp) into the live transport" ON)
if(CCORD_LIVE_ACCOUNT AND CCORD_DAVE)
  add_subdirectory(third_party/libdave)
endif()

# Every module is built unconditionally, in dependency order (layering is enforced by target_link_libraries).
add_subdirectory(src/core)
add_subdirectory(src/model)
add_subdirectory(src/emoji)
add_subdirectory(src/store)
add_subdirectory(src/net)
add_subdirectory(src/session)
add_subdirectory(src/theme)
add_subdirectory(src/ipc)
add_subdirectory(src/rpc)
add_subdirectory(src/plugins)
add_subdirectory(src/plugin_host)
add_subdirectory(src/platform)
add_subdirectory(src/render)
add_subdirectory(src/ui)
add_subdirectory(src/app)

if(CCORD_BUILD_TESTS OR CCORD_FUZZ)
  add_subdirectory(tests/fuzz)
endif()

if(CCORD_BUILD_TESTS)
  add_test(NAME test_hygiene
    COMMAND ${CMAKE_COMMAND} -DCCORD_SOURCE_DIR=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/cmake/CheckHygiene.cmake)
  set_tests_properties(test_hygiene PROPERTIES LABELS unit)
endif()
