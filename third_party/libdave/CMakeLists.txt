# Discord's official DAVE (Audio/Video E2EE) library, discord/libdave, plus its MLS dependency cisco/mlspp.
# Both are pinned to exact commits by sha256 in cmake/Pins.cmake and the tarballs are vendored next to this
# file (third_party/libdave/, third_party/mlspp/), mirroring how Lua is handled. Only the C API
# (<dave/dave.h>) is used by C-Cord; the C++ sources are compiled here into one static library, ccord_dave,
# with their own warnings silenced. mlspp is configured like libdave's vcpkg overlay port:
# MLS_CXX_NAMESPACE=mlspp, DISABLE_GREASE, OpenSSL 3, mls_ds/mls_vectors omitted.
enable_language(CXX)

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(nlohmann_json REQUIRED)  # header-only; mlspp's hpke uses it internally

function(ccord_vendor_extract name tarball sha url out_dir)
  if(NOT EXISTS "${tarball}")
    file(DOWNLOAD "${url}" "${tarball}" EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON)
  endif()
  file(SHA256 "${tarball}" got)
  if(NOT got STREQUAL sha)
    message(FATAL_ERROR "${name} tarball sha256 mismatch: got ${got}, pinned ${sha}")
  endif()
  set(dest "${CMAKE_BINARY_DIR}/third_party/${name}-src")
  if(NOT EXISTS "${dest}/.ccord-${sha}")
    file(REMOVE_RECURSE "${dest}")
    file(ARCHIVE_EXTRACT INPUT "${tarball}" DESTINATION "${dest}")
    file(WRITE "${dest}/.ccord-${sha}" "")
  endif()
  set(${out_dir} "${dest}" PARENT_SCOPE)
endfunction()

ccord_vendor_extract(mlspp
  "${CMAKE_SOURCE_DIR}/third_party/mlspp/mlspp-${CCORD_PIN_MLSPP_COMMIT}.tar.gz"
  ${CCORD_PIN_MLSPP_SHA256} ${CCORD_PIN_MLSPP_URL} mlspp_root)
ccord_vendor_extract(libdave
  "${CMAKE_CURRENT_SOURCE_DIR}/libdave-${CCORD_PIN_LIBDAVE_COMMIT}.tar.gz"
  ${CCORD_PIN_LIBDAVE_SHA256} ${CCORD_PIN_LIBDAVE_URL} dave_root)
set(MLSPP_SRC "${mlspp_root}/mlspp-${CCORD_PIN_MLSPP_COMMIT}")
set(DAVE_SRC "${dave_root}/libdave-${CCORD_PIN_LIBDAVE_COMMIT}/cpp")

# mlspp/cmake/namespace.h.in -> include/namespace.h
set(MLSPP_GEN "${CMAKE_CURRENT_BINARY_DIR}/mlspp-gen/include")
set(MLS_CXX_NAMESPACE mlspp)
configure_file("${MLSPP_SRC}/cmake/namespace.h.in" "${MLSPP_GEN}/namespace.h" @ONLY)

file(GLOB mlspp_sources
  "${MLSPP_SRC}/src/*.cpp"
  "${MLSPP_SRC}/lib/bytes/src/*.cpp"
  "${MLSPP_SRC}/lib/tls_syntax/src/*.cpp"
  "${MLSPP_SRC}/lib/hpke/src/*.cpp")
set(mlspp_includes
  "${MLSPP_SRC}/include" "${MLSPP_GEN}" "${MLSPP_SRC}/lib/bytes/include" "${MLSPP_SRC}/lib/tls_syntax/include"
  "${MLSPP_SRC}/lib/hpke/include" "${MLSPP_SRC}/third_party")

file(GLOB_RECURSE dave_sources "${DAVE_SRC}/src/*.cpp")
list(FILTER dave_sources EXCLUDE REGEX ".*persisted_key.*")
list(FILTER dave_sources EXCLUDE REGEX ".*_(win|apple|wasm)\\.cpp$")
list(FILTER dave_sources EXCLUDE REGEX ".*boringssl_cryptor.*")
list(APPEND dave_sources
  "${DAVE_SRC}/src/mls/persisted_key_pair_null.cpp"
  "${DAVE_SRC}/test/external_sender.cpp"
  "${DAVE_SRC}/test/capi/external_sender_wrapper.cpp")

add_library(ccord_mlspp STATIC ${mlspp_sources})
set_target_properties(ccord_mlspp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                                             POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(ccord_mlspp PUBLIC WITH_OPENSSL3 DISABLE_GREASE)
target_include_directories(ccord_mlspp SYSTEM PUBLIC ${mlspp_includes})
target_compile_options(ccord_mlspp PRIVATE -w -include ctime)
target_link_libraries(ccord_mlspp PRIVATE nlohmann_json::nlohmann_json PUBLIC OpenSSL::Crypto)

# libdave's private src/ tree has its own mls/session.h, so it must not leak into mlspp's include path.
add_library(ccord_dave STATIC ${dave_sources})
add_library(ccord::dave ALIAS ccord_dave)
set_target_properties(ccord_dave PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
                                            POSITION_INDEPENDENT_CODE ON)
target_include_directories(ccord_dave SYSTEM PUBLIC "${DAVE_SRC}/includes" "${DAVE_SRC}/test/capi")
target_include_directories(ccord_dave SYSTEM PRIVATE "${DAVE_SRC}/src" "${DAVE_SRC}/test")
target_compile_options(ccord_dave PRIVATE -w)
target_link_libraries(ccord_dave PRIVATE ccord_mlspp PUBLIC OpenSSL::Crypto)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_link_libraries(ccord_dave PUBLIC stdc++)
endif()
# C targets that link this static library are linked with the C++ driver automatically (CMake propagates the
# CXX link language of static libraries), so libstdc++ comes along without extra flags.
