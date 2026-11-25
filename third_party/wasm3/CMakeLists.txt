# wasm3 (sha256 pinned in cmake/Pins.cmake). Lightweight pure C WebAssembly interpreter.
set(CCORD_WASM3_TARBALL "${CMAKE_CURRENT_SOURCE_DIR}/wasm3-${CCORD_PIN_WASM3_COMMIT}.tar.gz")
if(NOT EXISTS "${CCORD_WASM3_TARBALL}")
  file(DOWNLOAD "${CCORD_PIN_WASM3_URL}" "${CCORD_WASM3_TARBALL}"
       EXPECTED_HASH SHA256=${CCORD_PIN_WASM3_SHA256} TLS_VERIFY ON STATUS ccord_dl_status)
endif()
file(SHA256 "${CCORD_WASM3_TARBALL}" ccord_wasm3_hash)
if(NOT ccord_wasm3_hash STREQUAL CCORD_PIN_WASM3_SHA256)
  message(FATAL_ERROR "wasm3 tarball sha256 mismatch: got ${ccord_wasm3_hash}, pinned ${CCORD_PIN_WASM3_SHA256}")
endif()

set(CCORD_WASM3_EXTRACT "${CMAKE_BINARY_DIR}/third_party/wasm3-src")
set(CCORD_WASM3_SRC "${CCORD_WASM3_EXTRACT}/wasm3-${CCORD_PIN_WASM3_COMMIT}/source")
if(NOT EXISTS "${CCORD_WASM3_SRC}/m3_core.c")
  file(ARCHIVE_EXTRACT INPUT "${CCORD_WASM3_TARBALL}" DESTINATION "${CCORD_WASM3_EXTRACT}")
endif()

set(CCORD_WASM3_SOURCES
  m3_bind.c m3_code.c m3_compile.c m3_core.c m3_deterministic.c
  m3_env.c m3_exec.c m3_function.c m3_info.c m3_module.c
  m3_parse.c m3_snapshot.c m3_validate.c m3_xxh64.c)
set(ccord_wasm3_files)
foreach(f IN LISTS CCORD_WASM3_SOURCES)
  list(APPEND ccord_wasm3_files "${CCORD_WASM3_SRC}/${f}")
endforeach()

add_library(ccord_wasm3 STATIC ${ccord_wasm3_files})
add_library(ccord::wasm3 ALIAS ccord_wasm3)
target_include_directories(ccord_wasm3 SYSTEM PUBLIC "${CCORD_WASM3_SRC}")
set_target_properties(ccord_wasm3 PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
if(NOT MSVC)
  target_compile_options(ccord_wasm3 PRIVATE -w)
endif()
target_link_libraries(ccord_wasm3 PUBLIC ${CCORD_MATH_LIB})
