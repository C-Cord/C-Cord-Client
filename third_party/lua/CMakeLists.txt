# Official Lua 5.5.1 (sha256 pinned in cmake/Pins.cmake). Only the core and standard
# libraries without io/os/package/debug are compiled into the static library ccord_lua.
set(CCORD_LUA_TARBALL "${CMAKE_CURRENT_SOURCE_DIR}/lua-${CCORD_PIN_LUA_VERSION}.tar.gz")
if(NOT EXISTS "${CCORD_LUA_TARBALL}")
  file(DOWNLOAD "${CCORD_PIN_LUA_URL}" "${CCORD_LUA_TARBALL}"
       EXPECTED_HASH SHA256=${CCORD_PIN_LUA_SHA256} TLS_VERIFY ON STATUS ccord_dl_status)
endif()
file(SHA256 "${CCORD_LUA_TARBALL}" ccord_lua_hash)
if(NOT ccord_lua_hash STREQUAL CCORD_PIN_LUA_SHA256)
  message(FATAL_ERROR "Lua tarball sha256 mismatch: got ${ccord_lua_hash}, pinned ${CCORD_PIN_LUA_SHA256}")
endif()

set(CCORD_LUA_EXTRACT "${CMAKE_BINARY_DIR}/third_party/lua-src")
file(ARCHIVE_EXTRACT INPUT "${CCORD_LUA_TARBALL}" DESTINATION "${CCORD_LUA_EXTRACT}")
set(CCORD_LUA_SRC "${CCORD_LUA_EXTRACT}/lua-${CCORD_PIN_LUA_VERSION}/src")

set(CCORD_LUA_SOURCES
  lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex lmathlib
  lmem lobject lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
set(ccord_lua_files)
foreach(f IN LISTS CCORD_LUA_SOURCES)
  list(APPEND ccord_lua_files "${CCORD_LUA_SRC}/${f}.c")
endforeach()

add_library(ccord_lua STATIC ${ccord_lua_files})
add_library(ccord::lua ALIAS ccord_lua)
target_include_directories(ccord_lua PUBLIC "${CCORD_LUA_SRC}")
target_compile_options(ccord_lua PRIVATE -w)
target_link_libraries(ccord_lua PUBLIC m)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_lua_version
    SOURCES tests/test_lua_version.c
    LIBS ccord_lua
    LABELS unit)
endif()
