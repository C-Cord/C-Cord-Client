# QuickJS-ng (sha256 pinned in cmake/Pins.cmake). Pure C ES2023+ JS engine for the native web runtime.
set(CCORD_QJS_TARBALL "${CMAKE_CURRENT_SOURCE_DIR}/quickjs-ng-v${CCORD_PIN_QUICKJS_VERSION}.tar.gz")
if(NOT EXISTS "${CCORD_QJS_TARBALL}")
  file(DOWNLOAD "${CCORD_PIN_QUICKJS_URL}" "${CCORD_QJS_TARBALL}"
       EXPECTED_HASH SHA256=${CCORD_PIN_QUICKJS_SHA256} TLS_VERIFY ON STATUS ccord_dl_status)
endif()
file(SHA256 "${CCORD_QJS_TARBALL}" ccord_qjs_hash)
if(NOT ccord_qjs_hash STREQUAL CCORD_PIN_QUICKJS_SHA256)
  message(FATAL_ERROR "QuickJS-ng tarball sha256 mismatch: got ${ccord_qjs_hash}, pinned ${CCORD_PIN_QUICKJS_SHA256}")
endif()

set(CCORD_QJS_EXTRACT "${CMAKE_BINARY_DIR}/third_party/quickjs-src")
set(CCORD_QJS_SRC "${CCORD_QJS_EXTRACT}/quickjs-${CCORD_PIN_QUICKJS_VERSION}")
if(NOT EXISTS "${CCORD_QJS_SRC}/quickjs.c")
  file(ARCHIVE_EXTRACT INPUT "${CCORD_QJS_TARBALL}" DESTINATION "${CCORD_QJS_EXTRACT}")
endif()

add_library(ccord_quickjs STATIC
  "${CCORD_QJS_SRC}/dtoa.c"
  "${CCORD_QJS_SRC}/libregexp.c"
  "${CCORD_QJS_SRC}/libunicode.c"
  "${CCORD_QJS_SRC}/quickjs.c")
add_library(ccord::quickjs ALIAS ccord_quickjs)
target_include_directories(ccord_quickjs SYSTEM PUBLIC "${CCORD_QJS_SRC}")
set_target_properties(ccord_quickjs PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_definitions(ccord_quickjs PRIVATE QUICKJS_NG_BUILD _GNU_SOURCE)
if(WIN32)
  target_compile_definitions(ccord_quickjs PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()
if(NOT MSVC)
  target_compile_options(ccord_quickjs PRIVATE -w)
endif()
find_package(Threads REQUIRED)
target_link_libraries(ccord_quickjs PUBLIC ${CCORD_MATH_LIB} Threads::Threads)
