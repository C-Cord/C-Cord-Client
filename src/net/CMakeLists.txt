# Layered networking & transport targets.
# Acyclic DAG:
#   ccord_net_contract (pure offline contract/admin/proto/policy/audio/rtp/dsp)
#     ├── ccord_net_mock (deterministic offline simulation)
#     ├── ccord_net_tls (LIVE: curl global init + strict TLS + SSRF opensocket)
#     │     ├── ccord_net_discord (LIVE: REST v9, Gateway v9, JSON codecs, token store)
#     │     ├── ccord_net_voice (DAVE E2EE + LIVE: Voice GW v8, Opus, AEAD, Go Live vstream)
#     │     └── ccord_net_services (dual-mode: profile_edit, http_fetch, video_resolve, detectable, app_lookup)
#     └── ccord_net_live (LIVE: live Discord transport orchestrator)
#   ccord_net (INTERFACE umbrella linking all active sub-targets)

include(CCordDeps)
find_package(PkgConfig QUIET)
ccord_find_speexdsp()
if(CCORD_LIVE_ACCOUNT)
  ccord_find_curl()
  ccord_find_zlib()
  find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CCORD_OPUS REQUIRED IMPORTED_TARGET opus)
  pkg_check_modules(CCORD_SODIUM REQUIRED IMPORTED_TARGET libsodium)
endif()

# 1. Pure offline contract, admin builders, rate governor, gateway pace, protobuf, RTP/audio/DSP.
add_library(ccord_net_contract STATIC
  common/net_util.c
  common/url_policy.c
  admin/admin_core.c
  admin/admin_channels.c
  admin/admin_guild.c
  admin/admin_moderation.c
  admin/admin_expressions.c
  admin/admin_social.c
  policy/discord_proto.c
  policy/governor.c
  policy/gateway_pace.c
  policy/voice_reconnect.c
  voice/rtp_h264.c
  voice/voice_audio.c
  voice/voice_dsp.c)
add_library(ccord::net_contract ALIAS ccord_net_contract)
target_include_directories(ccord_net_contract PUBLIC include)
target_include_directories(ccord_net_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccord_net_contract PUBLIC ccord_core ccord_model ${CCORD_MATH_LIB})
if(CCORD_SPEEXDSP_FOUND)
  target_link_libraries(ccord_net_contract PRIVATE PkgConfig::CCORD_SPEEXDSP)
  target_compile_definitions(ccord_net_contract PUBLIC CCORD_HAVE_SPEEXDSP=1)
else()
  target_compile_definitions(ccord_net_contract PUBLIC CCORD_HAVE_SPEEXDSP=0)
endif()
ccord_strict_warnings(ccord_net_contract)

# 2. Offline deterministic mock transport (compiler-enforced: cannot depend on live/curl/voice).
add_library(ccord_net_mock STATIC
  mock/mock_core.c
  mock/mock_dataset.c
  mock/mock_messages.c
  mock/mock_media.c
  mock/mock_expressions.c
  mock/mock_voice.c
  mock/mock_admin_helpers.c
  mock/mock_admin_guild.c
  mock/mock_admin_ext.c
  mock/mock_admin_ops.c
  mock/mock_interactions.c)
add_library(ccord::net_mock ALIAS ccord_net_mock)
target_include_directories(ccord_net_mock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccord_net_mock PUBLIC ccord_net_contract)
ccord_strict_warnings(ccord_net_mock)

# 3. Shared curl/TLS bootstrap + SSRF opensocket filter (LIVE only).
if(CCORD_LIVE_ACCOUNT)
  add_library(ccord_net_tls STATIC common/curl_tls.c)
  add_library(ccord::net_tls ALIAS ccord_net_tls)
  target_include_directories(ccord_net_tls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ccord_net_tls PUBLIC ccord_core CURL::libcurl)
  ccord_strict_warnings(ccord_net_tls)

  # 4. Discord REST v9, Main Gateway v9, JSON codecs, and token keyring (no voice/Opus/DAVE deps).
  add_library(ccord_net_discord STATIC
    discord/token_store.c
    discord/json_core.c
    discord/json_message.c
    discord/json_guild.c
    discord/json_guild_data.c
    discord/json_ready.c
    discord/json_dispatch.c
    discord/json_rest_profile.c
    discord/json_rest_admin.c
    discord/json_rest_community.c
    discord/discord_rest.c
    discord/gateway_wire.c
    discord/discord_gateway.c)
  add_library(ccord::net_discord ALIAS ccord_net_discord)
  target_include_directories(ccord_net_discord PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ccord_net_discord
    PUBLIC ccord_net_contract
    PRIVATE ccord_net_tls ZLIB::ZLIB OpenSSL::Crypto)
  ccord_strict_warnings(ccord_net_discord)
endif()

# 5. Voice media plane (DAVE E2EE + LIVE Voice Gateway v8 / Opus / AEAD / Go Live vstream).
# Built unconditionally for voice_dave.c (passthrough stub when CCORD_HAVE_DAVE is unset).
set(CCORD_NET_VOICE_SOURCES voice/voice_dave.c)
if(CCORD_LIVE_ACCOUNT)
  list(APPEND CCORD_NET_VOICE_SOURCES
    voice/voice_wire.c
    voice/voice_json.c
    voice/voice_gateway.c
    voice/voice_media_pump.c
    voice/voice_camera.c
    voice/voice_stream.c
    voice/voice_codec.c
    voice/voice_crypto.c)
endif()
add_library(ccord_net_voice STATIC ${CCORD_NET_VOICE_SOURCES})
add_library(ccord::net_voice ALIAS ccord_net_voice)
target_include_directories(ccord_net_voice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccord_net_voice PUBLIC ccord_net_contract)
if(TARGET ccord::dave)
  target_link_libraries(ccord_net_voice PRIVATE ccord::dave)
  target_compile_definitions(ccord_net_voice PUBLIC CCORD_HAVE_DAVE=1)
endif()
if(CCORD_LIVE_ACCOUNT)
  target_link_libraries(ccord_net_voice
    PRIVATE ccord_net_tls OpenSSL::Crypto PkgConfig::CCORD_OPUS PkgConfig::CCORD_SODIUM)
endif()
ccord_strict_warnings(ccord_net_voice)

# 6. Dual-mode auxiliary services (profile_edit, http_fetch, video_resolve, detectable, app_lookup).
add_library(ccord_net_services STATIC
  services/profile_edit.c
  services/account_edit.c
  services/app_lookup.c
  services/detectable.c
  services/http_fetch.c
  services/video_resolve_parse.c
  services/video_resolve_worker.c)
add_library(ccord::net_services ALIAS ccord_net_services)
target_include_directories(ccord_net_services PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccord_net_services PUBLIC ccord_net_contract)
if(CCORD_LIVE_ACCOUNT)
  target_link_libraries(ccord_net_services PRIVATE ccord_net_discord ccord_net_tls)
  target_compile_definitions(ccord_net_services PRIVATE
    CCORD_PROFILE_LIVE=1
    CCORD_APP_LOOKUP_REST=1
    CCORD_DETECTABLE_LIVE=1
    CCORD_HTTP_FETCH_LIVE=1
    CCORD_VIDEO_RESOLVE_LIVE=1)
endif()
ccord_strict_warnings(ccord_net_services)

# 7. Live Discord transport orchestrator (LIVE only).
if(CCORD_LIVE_ACCOUNT)
  add_library(ccord_net_live STATIC
    live/live_wire.c
    live/live_core.c
    live/live_rest_dispatch.c
    live/live_admin_dispatch.c
    live/live_actions_msg.c
    live/live_actions_social.c
    live/live_voice.c
    live/live_streams.c)
  add_library(ccord::net_live ALIAS ccord_net_live)
  target_include_directories(ccord_net_live PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ccord_net_live
    PUBLIC ccord_net_discord ccord_net_voice ccord_net_services
    PRIVATE ccord_net_tls PkgConfig::CCORD_OPUS)
  ccord_strict_warnings(ccord_net_live)
endif()

# 8. Aggregate umbrella target (for app and full-stack integration tests).
add_library(ccord_net INTERFACE)
add_library(ccord::net ALIAS ccord_net)
target_link_libraries(ccord_net INTERFACE ccord_net_contract ccord_net_mock ccord_net_voice ccord_net_services)
if(CCORD_LIVE_ACCOUNT)
  target_link_libraries(ccord_net INTERFACE ccord_net_discord ccord_net_live OpenSSL::Crypto)
endif()

if(CCORD_BUILD_TESTS)
  # Pure offline contract unit tests (zero curl/OpenSSL/Opus/sodium/DAVE/mock relink!)
  ccord_add_test(NAME test_discord_proto SOURCES tests/test_discord_proto.c LIBS ccord_net_contract LABELS unit)
  ccord_add_test(NAME test_governor SOURCES tests/test_governor.c LIBS ccord_net_contract LABELS unit)
  ccord_add_test(NAME test_admin SOURCES tests/test_admin.c LIBS ccord_net_contract LABELS unit)
  ccord_add_test(NAME test_gateway_pace SOURCES tests/test_gateway_pace.c LIBS ccord_net_contract LABELS unit)
  ccord_add_test(NAME test_voice_reconnect SOURCES tests/test_voice_reconnect.c LIBS ccord_net_contract LABELS unit)
  ccord_add_test(NAME test_rtp_h264 SOURCES tests/test_rtp_h264.c LIBS ccord_net_contract LABELS unit)
  ccord_add_test(NAME test_voice_audio SOURCES tests/test_voice_audio.c LIBS ccord_net_contract LABELS unit)
  ccord_add_test(NAME test_voice_dsp SOURCES tests/test_voice_dsp.c LIBS ccord_net_contract LABELS unit)
  if(TARGET ccord::dave)
    ccord_add_test(NAME test_voice_dave SOURCES tests/test_voice_dave.c LIBS ccord_net_voice LABELS unit)
    target_link_libraries(test_voice_dave PRIVATE ccord::dave)
  endif()
  ccord_add_test(NAME test_video_resolve SOURCES tests/test_video_resolve.c LIBS ccord_net_services LABELS unit)

  # Offline mock & session-voice tests
  ccord_add_test(NAME test_mock_core SOURCES tests/test_mock_core.c LIBS ccord_net_mock LABELS unit)
  ccord_add_test(NAME test_mock_admin SOURCES tests/test_mock_admin.c LIBS ccord_net_mock LABELS unit)
  ccord_add_test(NAME test_mock_voice SOURCES tests/test_mock_voice.c LIBS ccord_net_mock LABELS unit)
  ccord_add_test(NAME test_session_voice SOURCES tests/test_session_voice.c LIBS ccord_session ccord_net_mock LABELS integration)

  if(CCORD_LIVE_ACCOUNT)
    ccord_add_test(NAME test_voice_crypto SOURCES tests/test_voice_crypto.c LIBS ccord_net_voice LABELS unit)
    ccord_add_test(NAME test_discord_json SOURCES tests/test_discord_json.c LIBS ccord_net_discord ccord_net_mock LABELS unit)
    ccord_add_test(NAME test_m6_json SOURCES tests/test_m6_json.c LIBS ccord_net_discord LABELS unit)
    ccord_add_test(NAME test_account_edit SOURCES tests/test_account_edit.c LIBS ccord_net LABELS unit)
    ccord_add_test(NAME test_profile_json SOURCES tests/test_profile_json.c LIBS ccord_net_discord ccord_net_mock ccord_store LABELS unit)

    ccord_add_test(NAME test_live_auth_policy SOURCES tests/test_live_auth_policy.c LIBS ccord_net LABELS unit)
    ccord_add_test(NAME test_live_transport_gw SOURCES tests/test_live_transport_gw.c LIBS ccord_net LABELS unit)
    ccord_add_test(NAME test_live_rest SOURCES tests/test_live_rest.c LIBS ccord_net LABELS unit)
    ccord_add_test(NAME test_live_features SOURCES tests/test_live_features.c LIBS ccord_net LABELS unit)
    ccord_add_test(NAME test_rest_queue SOURCES tests/test_rest_queue.c LIBS ccord_net LABELS integration)
    ccord_add_test(NAME test_rest_range SOURCES tests/test_rest_range.c LIBS ccord_net LABELS integration)
    ccord_add_test(NAME test_live_actions SOURCES tests/test_live_actions.c LIBS ccord_net LABELS integration)
    ccord_add_test(NAME test_live_admin SOURCES tests/test_live_admin.c LIBS ccord_net LABELS integration)
    ccord_add_test(NAME test_live_gateway SOURCES tests/test_live_gateway.c LIBS ccord_net LABELS integration)
    ccord_add_test(NAME test_discord_gateway SOURCES tests/test_discord_gateway.c LIBS ccord_net LABELS integration)
    ccord_add_test(NAME test_voice_core SOURCES tests/test_voice_core.c LIBS ccord_net LABELS integration)
    ccord_add_test(NAME test_voice_resume SOURCES tests/test_voice_resume.c LIBS ccord_net LABELS integration)
    ccord_add_test(NAME test_voice_loopback SOURCES tests/test_voice_loopback.c LIBS ccord_net CURL::libcurl PkgConfig::CCORD_OPUS LABELS integration)
    ccord_add_test(NAME test_live_calls_voice SOURCES tests/test_live_calls_voice.c LIBS ccord_session ccord_net LABELS integration)
    ccord_add_test(NAME test_live_calls_streams SOURCES tests/test_live_calls_streams.c LIBS ccord_session ccord_net LABELS integration)
    ccord_add_test(NAME test_m6_user_settings SOURCES tests/test_m6_user_settings.c LIBS ccord_net LABELS integration)
  endif()
endif()
