option(CCORD_SANITIZE "Build everything with ASan and UBSan" OFF)

# Sets up sanitizer flags for all targets (including third-party Lua) when CCORD_SANITIZE is ON.
macro(ccord_sanitize_setup)
  if(CCORD_SANITIZE)
    set(CCORD_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_compile_options(${CCORD_SAN_FLAGS})
    add_link_options(${CCORD_SAN_FLAGS})
    add_compile_definitions(CCORD_SANITIZED=1)
    message(STATUS "C-Cord: sanitizers ON (ASan + UBSan)")
  endif()
endmacro()
