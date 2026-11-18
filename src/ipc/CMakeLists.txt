add_library(ccord_ipc STATIC ipc.c perm.c)
add_library(ccord::ipc ALIAS ccord_ipc)
target_include_directories(ccord_ipc PUBLIC include)
target_link_libraries(ccord_ipc PUBLIC ccord_core)
ccord_strict_warnings(ccord_ipc)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_ipc SOURCES tests/test_ipc.c LIBS ccord_ipc LABELS unit)
endif()
