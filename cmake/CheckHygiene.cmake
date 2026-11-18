if(NOT DEFINED CCORD_SOURCE_DIR)
  get_filename_component(CCORD_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
set(CCORD_ROOT "${CCORD_SOURCE_DIR}")

function(ccord_hygiene_fail message_text)
  message(FATAL_ERROR "hygiene: ${message_text}")
endfunction()

# 1. Token, secret-assignment and long dotted base64url literals. REDACTED_TEST_* sentinels are the only allowed
# fixture values and do not match these shapes.
file(GLOB_RECURSE ccord_token_files LIST_DIRECTORIES false
  "${CCORD_ROOT}/src/*" "${CCORD_ROOT}/tests/*" "${CCORD_ROOT}/extensions/*" "${CCORD_ROOT}/assets/*"
  "${CCORD_ROOT}/README.md" "${CCORD_ROOT}/SECURITY.md" "${CCORD_ROOT}/docs/*")
foreach(path IN LISTS ccord_token_files)
  if(IS_DIRECTORY "${path}")
    continue()
  endif()
  if(path MATCHES "\\.png$")
    continue()
  endif()
  file(READ "${path}" content)
  if(content MATCHES "Bearer[ \t\r\n]+[A-Za-z0-9._~+/=-]{20,}")
    ccord_hygiene_fail("Bearer token literal in ${path}")
  endif()
  if(content MATCHES "Bot[ \t\r\n]+[A-Za-z0-9._~+/=-]{20,}")
    ccord_hygiene_fail("Bot token literal in ${path}")
  endif()
  if(content MATCHES "client_secret[ \t]*=[ \t]*\"[^\"]+\"")
    ccord_hygiene_fail("client_secret assignment in ${path}")
  endif()
  if(content MATCHES "[A-Za-z0-9_-]{16,}\\.[A-Za-z0-9_-]{16,}\\.[A-Za-z0-9_-]{16,}")
    ccord_hygiene_fail("long dotted base64url literal in ${path}")
  endif()
endforeach()

# 2. Existing layers may not include the secret buffer.
file(GLOB_RECURSE ccord_old_layer_files LIST_DIRECTORIES false
  "${CCORD_ROOT}/src/ipc/*" "${CCORD_ROOT}/src/plugins/*" "${CCORD_ROOT}/src/plugin_host/*"
  "${CCORD_ROOT}/src/store/*" "${CCORD_ROOT}/src/session/*" "${CCORD_ROOT}/src/net/*"
  "${CCORD_ROOT}/src/model/*")
foreach(path IN LISTS ccord_old_layer_files)
  if(IS_DIRECTORY "${path}")
    continue()
  endif()
  file(READ "${path}" content)
  foreach(forbidden "ccord/core/secret.h")
    string(FIND "${content}" "${forbidden}" found)
    if(NOT found EQUAL -1)
      ccord_hygiene_fail("${path} references ${forbidden}")
    endif()
  endforeach()
endforeach()

message(STATUS "hygiene checks passed")
