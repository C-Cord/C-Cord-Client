# Applies strict warnings and hardening flags to one of our own targets.
function(ccord_strict_warnings tgt)
  set(flags
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wformat=2
    -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls
    -Wundef -Wvla -Wwrite-strings -Wdouble-promotion)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND flags -Wduplicated-cond -Wlogical-op -Wnull-dereference)
  endif()
  if(NOT CCORD_SANITIZE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND flags -fstack-protector-strong)
    if(NOT CCORD_BUILD_TESTS)
      target_compile_options(${tgt} PRIVATE
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-U_FORTIFY_SOURCE>"
        "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-D_FORTIFY_SOURCE=3>")
    endif()
  endif()
  if(CCORD_WERROR)
    list(APPEND flags -Werror)
  endif()
  target_compile_options(${tgt} PRIVATE ${flags})
endfunction()
