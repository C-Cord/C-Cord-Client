add_library(ccord_store STATIC
  store.c
  store_index.c
  store_guilds.c
  store_users.c
  store_messages.c
  store_roles.c
  store_voice.c
  store_activities.c
  store_events.c
  store_slash.c
  store_expr_guild.c
  store_expr_picker.c
  store_admin.c
  store_forum.c
  store_notify.c
  store_user.c
  store_listsize.c
  store_unread.c)
add_library(ccord::store ALIAS ccord_store)
target_include_directories(ccord_store PUBLIC include)
target_link_libraries(ccord_store PUBLIC ccord_core ccord_model)
ccord_strict_warnings(ccord_store)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_store SOURCES tests/test_store.c LIBS ccord_store LABELS unit)
  ccord_add_test(NAME test_store_stale SOURCES tests/test_store_stale.c LIBS ccord_store LABELS unit)
  ccord_add_test(NAME test_store_voice SOURCES tests/test_store_voice.c LIBS ccord_store LABELS unit)
  ccord_add_test(NAME test_store_user SOURCES tests/test_store_user.c LIBS ccord_store LABELS unit)
endif()
