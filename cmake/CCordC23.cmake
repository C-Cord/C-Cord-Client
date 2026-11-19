# C23 is mandatory. No fallback to C17 or older.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(ccord_check_c23)
  # GCC 14 reports __STDC_VERSION__ == 202000L under -std=c23/-std=c2x (C17 is 201710L;
  # GCC 15+ and Clang 18+ report 202311L), so also probe C23 features we rely on.
  file(WRITE ${CMAKE_BINARY_DIR}/c23probe.c
    "#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202000L\n#error not C23\n#endif\n"
    "#include <stdckdint.h>\n"
    "int main(void){void *p=nullptr; auto x=(int)(p==nullptr); typeof(x) y=0; return ckd_add(&y,x,0)?1:0;}\n")
  try_compile(CCORD_C23_OK ${CMAKE_BINARY_DIR}/c23probe
    SOURCES ${CMAKE_BINARY_DIR}/c23probe.c
    C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    OUTPUT_VARIABLE CCORD_C23_OUT)
  if(NOT CCORD_C23_OK)
    message(FATAL_ERROR "C-Cord needs a C23 compiler (GCC >= 14 or Clang >= 18); no fallback to C17\n${CCORD_C23_OUT}")
  endif()
endfunction()
