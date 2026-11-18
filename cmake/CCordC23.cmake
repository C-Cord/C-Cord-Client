# C23 is mandatory. No fallback to C17 or older.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(ccord_check_c23)
  file(WRITE ${CMAKE_BINARY_DIR}/c23probe.c
    "#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L\n#error not C23\n#endif\nint main(void){return 0;}\n")
  try_compile(CCORD_C23_OK ${CMAKE_BINARY_DIR}/c23probe
    SOURCES ${CMAKE_BINARY_DIR}/c23probe.c
    C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(NOT CCORD_C23_OK)
    message(FATAL_ERROR "C-Cord needs a C23 compiler (GCC >= 14 or Clang >= 18); no fallback to C17")
  endif()
endfunction()
