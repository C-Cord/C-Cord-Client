add_library(ccord_store STATIC store.c store_index.c expressions.c store_forum.c store_roles.c store_admin.c store_notify.c store_unread.c store_listsize.c store_voice.c store_user.c)
add_library(ccord::store ALIAS ccord_store)
target_include_directories(ccord_store PUBLIC include)
target_link_libraries(ccord_store PUBLIC ccord_core ccord_model)
ccord_strict_warnings(ccord_store)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_store SOURCES tests/test_store.c LIBS ccord_store LABELS unit)
  ccord_add_test(NAME test_store_stale SOURCES tests/test_store_stale.c LIBS ccord_store LABELS unit)
  # M5 test fragment.
  include(tests/m5_store.cmake)
  # M6 account-settings fragment.
  include(tests/m6_store.cmake)
endif()
