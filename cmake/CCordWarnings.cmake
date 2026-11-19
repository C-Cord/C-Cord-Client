# Applies strict warnings and hardening flags to one of our own targets.
function(ccord_strict_warnings tgt)
  set(common_flags
    -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Wformat=2
    -Wcast-qual -Wcast-align -Wredundant-decls -Wundef -Wvla -Wwrite-strings -Wdouble-promotion)
  set(c_only_flags -Wpedantic -Wstrict-prototypes -Wmissing-prototypes)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND c_only_flags -Wduplicated-cond -Wlogical-op -Wnull-dereference)
  endif()
  if(NOT CCORD_SANITIZE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND common_flags -fstack-protector-strong)
    if(NOT CCORD_BUILD_TESTS)
      target_compile_options(${tgt} PRIVATE
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-U_FORTIFY_SOURCE>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-D_FORTIFY_SOURCE=3>")
    endif()
  endif()
  if(CCORD_WERROR)
    list(APPEND common_flags -Werror)
  endif()
  target_compile_options(${tgt} PRIVATE
    ${common_flags}
    "$<$<COMPILE_LANGUAGE:C>:${c_only_flags}>")
endfunction()
