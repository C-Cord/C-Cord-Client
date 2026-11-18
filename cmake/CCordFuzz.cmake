# Fuzz harness support (tests/fuzz). Two modes:
#  - CCORD_FUZZ=ON (Clang only): each harness links libFuzzer (-fsanitize=fuzzer,address,undefined)
#    and a bounded -runs=2000 smoke test is registered. All targets get ASan + UBSan and
#    fuzzer-no-link coverage instrumentation, so libFuzzer guides mutations by the branches inside
#    the libraries, not just the harness translation units.
#  - default (GCC or Clang, with or without CCORD_SANITIZE): each harness links fuzz_runner_main.c,
#    which drives LLVMFuzzerTestOneInput through a fixed seed + adversarial corpus, and is registered
#    as a normal deterministic CTest target.
option(CCORD_FUZZ "Build libFuzzer harnesses (requires Clang)" OFF)

# Call after ccord_sanitize_setup().
macro(ccord_fuzz_setup)
  if(CCORD_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "CCORD_FUZZ=ON requires Clang (libFuzzer); reconfigure with "
                          "-DCCORD_FUZZ=OFF for ${CMAKE_C_COMPILER_ID}")
    endif()
    # Coverage counters everywhere, but no libFuzzer main: only the harness targets link the
    # fuzzer runtime (-fsanitize=fuzzer in ccord_add_fuzz).
    add_compile_options(-fsanitize=fuzzer-no-link)
    if(NOT CCORD_SANITIZE)
      set(CCORD_FUZZ_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined
                               -fno-omit-frame-pointer)
      add_compile_options(${CCORD_FUZZ_SAN_FLAGS})
      add_link_options(${CCORD_FUZZ_SAN_FLAGS})
      # Same contract as ccord_sanitize_setup: the plugin host skips its 1 GiB RLIMIT_AS and forwards
      # sanitizer options to children only when CCORD_SANITIZED is set (src/plugin_host/main.c).
      add_compile_definitions(CCORD_SANITIZED=1)
      message(STATUS "C-Cord: fuzz build instruments all targets (ASan + UBSan + libFuzzer coverage)")
    endif()
  endif()
endmacro()

# ccord_add_fuzz(NAME <name> SOURCES <files...> LIBS <targets...>)
# SOURCES are the harness files only; the driver (fuzz_runner_main.c or libFuzzer) is added here.
function(ccord_add_fuzz)
  cmake_parse_arguments(ARG "" "NAME" "SOURCES;LIBS" ${ARGN})
  if(NOT ARG_NAME OR NOT ARG_SOURCES)
    message(FATAL_ERROR "ccord_add_fuzz needs NAME and SOURCES")
  endif()
  if(CCORD_FUZZ)
    add_executable(${ARG_NAME} ${ARG_SOURCES})
    target_link_libraries(${ARG_NAME} PRIVATE ${ARG_LIBS})
    set(CCORD_FUZZ_TARGET_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
                                -fno-omit-frame-pointer)
    target_compile_options(${ARG_NAME} PRIVATE ${CCORD_FUZZ_TARGET_FLAGS})
    target_link_options(${ARG_NAME} PRIVATE ${CCORD_FUZZ_TARGET_FLAGS})
    ccord_strict_warnings(${ARG_NAME})
    if(CCORD_BUILD_TESTS)
      # Crash artifacts stay inside the build tree, never the source tree.
      file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuzz-artifacts")
      add_test(NAME ${ARG_NAME}_smoke COMMAND ${ARG_NAME} -runs=2000 -max_len=8192 -timeout=10)
      set_tests_properties(${ARG_NAME}_smoke PROPERTIES LABELS fuzz
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuzz-artifacts")
    endif()
  else()
    if(NOT CCORD_BUILD_TESTS)
      return()
    endif()
    ccord_add_test(NAME ${ARG_NAME} SOURCES ${ARG_SOURCES} fuzz_runner_main.c
      LIBS ${ARG_LIBS} LABELS fuzz)
  endif()
endfunction()
