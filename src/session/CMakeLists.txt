find_package(Threads REQUIRED)
add_library(ccord_session STATIC
  session.c
  session_events.c
  session_events_ext.c
  session_media.c
  media_stream.c
  session_messages.c
  session_voice.c
  session_admin.c
  session_presence.c
  session_social.c
  session_features.c)
add_library(ccord::session ALIAS ccord_session)
target_include_directories(ccord_session PUBLIC include)
target_link_libraries(ccord_session
  PUBLIC ccord_core ccord_model ccord_store ccord_net_contract Threads::Threads
  PRIVATE ccord_net_services)
ccord_strict_warnings(ccord_session)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_session_core SOURCES tests/test_session_core.c LIBS ccord_session ccord_net_mock LABELS unit)
  ccord_add_test(NAME test_session_messages SOURCES tests/test_session_messages.c LIBS ccord_session ccord_net_mock LABELS unit)
  ccord_add_test(NAME test_session_admin SOURCES tests/test_session_admin.c LIBS ccord_session ccord_net_mock LABELS unit)
  ccord_add_test(NAME test_session_features SOURCES tests/test_session_features.c LIBS ccord_session ccord_net_mock LABELS unit)
  ccord_add_test(NAME test_media_stream SOURCES tests/test_media_stream.c LIBS ccord_session ccord_net_mock LABELS unit)
  if(CCORD_LIVE_ACCOUNT)
    ccord_add_test(NAME test_session_profile_edit SOURCES tests/test_session_profile_edit.c LIBS ccord_session ccord_net_mock ccord_net_services LABELS unit)
  endif()
  ccord_add_test(NAME test_session_stale SOURCES tests/test_session_stale.c LIBS ccord_session ccord_net_mock LABELS unit)
  if(CCORD_LIVE_ACCOUNT)
    ccord_add_test(NAME test_session_user_settings SOURCES tests/test_session_user_settings.c LIBS ccord_session ccord_net LABELS unit)
    target_compile_definitions(test_session_user_settings PRIVATE CCORD_LIVE_ACCOUNT=1)
  else()
    ccord_add_test(NAME test_session_user_settings SOURCES tests/test_session_user_settings.c LIBS ccord_session ccord_net_mock LABELS unit)
    target_compile_definitions(test_session_user_settings PRIVATE CCORD_LIVE_ACCOUNT=0)
  endif()
endif()
