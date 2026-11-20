add_library(ccord_account STATIC account.c)
target_link_libraries(ccord_account PUBLIC ccord_ui_logic)
ccord_strict_warnings(ccord_account)

if(ANDROID)
  # SDL3's Android SDLActivity loads libmain.so and calls SDL_main().
  set(CCORD_APP_TARGET main)
  add_library(main SHARED main.c)
  # Static FFmpeg AArch64 NEON assembly references internal FFmpeg tables with PC-relative relocations;
  # -Bsymbolic binds internal symbols locally inside libmain.so without disabling PIC or textrels.
  target_link_options(main PRIVATE -Wl,-Bsymbolic)
else()
  set(CCORD_APP_TARGET ccord)
  add_executable(ccord main.c)
endif()
if(WIN32)
  target_sources(${CCORD_APP_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/assets/windows/ccord.rc)
  target_link_libraries(${CCORD_APP_TARGET} PRIVATE advapi32)
endif()
target_link_libraries(${CCORD_APP_TARGET} PRIVATE ccord_core ccord_model ccord_store ccord_net ccord_session ccord_theme ccord_platform ccord_render ccord_ui
                               ccord_account ccord_rpc SDL3::SDL3)
ccord_strict_warnings(${CCORD_APP_TARGET})
target_compile_definitions(${CCORD_APP_TARGET} PRIVATE CCORD_BUILD_VERIFY=$<BOOL:${CCORD_BUILD_TESTS}>)
if(CCORD_LIVE_ACCOUNT)
  target_compile_definitions(${CCORD_APP_TARGET} PRIVATE CCORD_LIVE_ACCOUNT=1)
else()
  target_compile_definitions(${CCORD_APP_TARGET} PRIVATE CCORD_LIVE_ACCOUNT=0)
endif()

# Core runtime app modules (built unconditionally).
target_sources(${CCORD_APP_TARGET} PRIVATE
  voice_audio_thread.c
  app_media.c
  earcons.c
  rpc_app_lookup.c
  app_rpc.c
  game_detect_app.c
  app_profile_rest.c
  app_desktop.c
  warm_cache.c
  scheduled_messages.c
  updater.c)

target_include_directories(${CCORD_APP_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src/ui)

if(CCORD_BUILD_TESTS)
  if(CCORD_LIVE_ACCOUNT)
    target_sources(${CCORD_APP_TARGET} PRIVATE verify_live_stream.c verify_stream_audio.c verify_expressions.c verify_messaging_events.c verify_rich_presence.c
                                  verify_membership_screening.c verify_components_v2.c verify_activities.c verify_voice_messages.c)
    target_include_directories(${CCORD_APP_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
  endif()
  target_sources(${CCORD_APP_TARGET} PRIVATE
    verify_m5.c
    verify_gif_favorites.c
    verify_embed_video.c
    verify_video_stream.c
    verify_call_bar.c verify_search_view.c verify_picker_layout.c verify_voice_text_chat.c verify_earcons.c
    verify_reply_style.c verify_ui_style.c verify_panel_toggles.c verify_smooth_scroll.c
    verify_motion.c verify_motion_overlays.c verify_motion_nav.c verify_motion_chat.c verify_motion_controls.c
    verify_media_load.c verify_hover_edit.c verify_status_card.c verify_jump_present.c
    verify_ccord_settings.c verify_game_detect.c verify_profile_edit.c verify_reaction_once.c
    verify_rpc_app_name.c
    verify_m6.c
    verify_streamer_mode.c
    verify_warm_start.c verify_plus_menu.c verify_poll_create.c verify_schedule_message.c
    verify_fav_gif_order.c verify_tray_unread.c verify_close_background.c verify_app_icon.c
    verify_status_sync.c verify_blocked_hide.c verify_ctx_react.c verify_new_messages_bar.c
    verify_report_flow.c verify_call_ring.c verify_global_ptt.c verify_voice_reconnect.c
    verify_multi_camera.c verify_notify_jump.c verify_resume_reconnect.c
    verify_profile_view.c verify_profile_roles.c verify_rail_scroll.c
    verify_composer_format.c verify_settings_profile.c verify_glyph_fallback.c verify_ansi_block.c)
endif()

# Extensions: the plugin manager and the sandboxed host are built directly into the app
# (and standalone ccord-plugin-host on desktop; Android runs the sandboxed host on in-process worker threads).
target_sources(${CCORD_APP_TARGET} PRIVATE ../plugin_host/main.c ../plugin_host/sandbox.c)
set_source_files_properties(../plugin_host/main.c PROPERTIES COMPILE_DEFINITIONS "CCORD_EMBEDDED_PLUGIN_HOST=1")
target_include_directories(${CCORD_APP_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src/plugin_host)
target_link_libraries(${CCORD_APP_TARGET} PRIVATE ccord_plugins ccord_ipc ccord_lua)
if(ANDROID)
  target_compile_definitions(${CCORD_APP_TARGET} PRIVATE CCORD_WITH_PLUGINS=1 CCORD_PLUGIN_HOST_PATH="" CCORD_EXTENSIONS_DIR="")
else()
  target_compile_definitions(${CCORD_APP_TARGET} PRIVATE CCORD_WITH_PLUGINS=1 CCORD_PLUGIN_HOST_PATH="$<IF:$<BOOL:${CCORD_BUILD_TESTS}>,$<TARGET_FILE:ccord-plugin-host>,${CMAKE_INSTALL_FULL_BINDIR}/ccord-plugin-host>" CCORD_EXTENSIONS_DIR="$<IF:$<BOOL:${CCORD_BUILD_TESTS}>,${CMAKE_SOURCE_DIR}/extensions,${CMAKE_INSTALL_FULL_DATADIR}/ccord/extensions>")
  add_dependencies(${CCORD_APP_TARGET} ccord-plugin-host)
endif()
if(NOT ANDROID)
  if(UNIX)
    add_custom_command(TARGET ccord POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ccord>" "${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:ccord>"
      COMMAND ${CMAKE_COMMAND} -E create_symlink ccord "${CMAKE_CURRENT_BINARY_DIR}/c-cord"
      COMMAND ${CMAKE_COMMAND} -E create_symlink ccord "${CMAKE_BINARY_DIR}/c-cord"
      BYPRODUCTS "${CMAKE_BINARY_DIR}/ccord" "${CMAKE_BINARY_DIR}/c-cord" "${CMAKE_CURRENT_BINARY_DIR}/c-cord"
      COMMENT "Copying self-contained ccord binary (and c-cord alias) to ${CMAKE_BINARY_DIR}")
  else()
    add_custom_command(TARGET ccord POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ccord>" "${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:ccord>"
      BYPRODUCTS "${CMAKE_BINARY_DIR}/ccord${CMAKE_EXECUTABLE_SUFFIX}"
      COMMENT "Copying self-contained ccord binary to ${CMAKE_BINARY_DIR}")
  endif()
endif()

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_app_smoke SOURCES tests/test_app_smoke.c LIBS SDL3::SDL3 LABELS integration ui)
  target_compile_definitions(test_app_smoke PRIVATE
    CCORD_BIN="$<TARGET_FILE:ccord>"
    CCORD_VERSION_STRING="${CCORD_VERSION_CLEAN}")
  if(CCORD_LIVE_ACCOUNT)
    target_compile_definitions(test_app_smoke PRIVATE CCORD_LIVE_ACCOUNT=1)
  else()
    target_compile_definitions(test_app_smoke PRIVATE CCORD_LIVE_ACCOUNT=0)
  endif()
  add_dependencies(test_app_smoke ccord)

  # M0-7: --net-stats + --verify-slice m0 (runs the real binary offline).
  ccord_add_test(NAME test_cli_stats SOURCES tests/test_cli_stats.c LABELS integration)
  target_compile_definitions(test_cli_stats PRIVATE CCORD_BIN="$<TARGET_FILE:ccord>")
  add_dependencies(test_cli_stats ccord)

  ccord_add_test(NAME test_app_account SOURCES tests/test_app_account.c
                 LIBS ccord_account ccord_session LABELS integration)
  target_compile_definitions(test_app_account PRIVATE CCORD_BIN="$<TARGET_FILE:ccord>")
  target_include_directories(test_app_account PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_dependencies(test_app_account ccord)

  # Rich Presence app wiring: per-connection slots, trailing-edge coalescing and clear paths over a real
  # AF_UNIX socket against a mock-transport session (hermetic; scratch dir under $TMPDIR).
  ccord_add_test(NAME test_app_rpc SOURCES tests/test_app_rpc.c app_rpc.c game_detect_app.c rpc_app_lookup.c
                 LIBS ccord_rpc ccord_session ccord_store ccord_net LABELS integration)
  target_include_directories(test_app_rpc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # Application-name lookup: cache, fallback and backoff rules over a scripted source (no network).
  ccord_add_test(NAME test_rpc_app_lookup SOURCES tests/test_rpc_app_lookup.c rpc_app_lookup.c
                 LIBS ccord_rpc ccord_net LABELS unit)
  target_include_directories(test_rpc_app_lookup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

  # Own-profile REST backend wiring: --mock edit through the session, live token follows account switch/sign-out
  # (loopback fake Discord; token files under $TMPDIR).
  ccord_add_test(NAME test_app_profile_rest SOURCES tests/test_app_profile_rest.c app_profile_rest.c
                 LIBS ccord_session ccord_store ccord_net LABELS integration)
  target_include_directories(test_app_profile_rest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(CCORD_LIVE_ACCOUNT)
    target_compile_definitions(test_app_profile_rest PRIVATE CCORD_LIVE_ACCOUNT=1)
  else()
    target_compile_definitions(test_app_profile_rest PRIVATE CCORD_LIVE_ACCOUNT=0)
  endif()

  # Warm-start disk cache: round trip, modes, integrity rejection, account isolation, forget/prune/cap (under $TMPDIR).
  ccord_add_test(NAME test_warm_cache SOURCES tests/test_warm_cache.c warm_cache.c
                 LIBS ccord_session ccord_store ccord_net LABELS unit)
  target_include_directories(test_warm_cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  ccord_add_test(NAME test_updater SOURCES tests/test_updater.c updater.c
                 LIBS ccord_core ccord_net ccord_ui LABELS unit)
  target_include_directories(test_updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(CCORD_LIVE_ACCOUNT)
    target_compile_definitions(test_warm_cache PRIVATE CCORD_LIVE_ACCOUNT=1)
  else()
    target_compile_definitions(test_warm_cache PRIVATE CCORD_LIVE_ACCOUNT=0)
  endif()

  # M0-6: dedicated audio pthread + voice_gateway PCM locking. Needs the live-only voice gateway.
  # The test stubs ccord_audio_* itself (no SDL/platform link), so it runs headless anywhere.
  if(CCORD_LIVE_ACCOUNT)
    ccord_add_test(NAME test_voice_audio_thread SOURCES tests/test_voice_audio_thread.c voice_audio_thread.c
                   LIBS ccord_net LABELS integration voice)
    target_include_directories(test_voice_audio_thread PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/platform/include)
    set_tests_properties(test_voice_audio_thread PROPERTIES TIMEOUT 60)

    # End-to-end --verify-slice live-stream: runs the REAL ccord binary headless against forked
    # loopback HTTP/WebSocket gateways and UDP SFU sockets (no Discord account or network needed).
    add_test(NAME live_stream_verify COMMAND ccord --headless --verify-slice live-stream)
    set_tests_properties(live_stream_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")

    # End-to-end --verify-slice stream-audio: Go Live audio (fake 440 Hz desktop-loopback tone ->
    # Opus PT 120 on the stream's READY audio SSRC) over the same forked loopback servers.
    add_test(NAME stream_audio_verify COMMAND ccord --headless --verify-slice stream-audio)
    set_tests_properties(stream_audio_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")

    # End-to-end --verify-slice rich-presence: local discord-ipc client -> app_rpc -> session -> live
    # transport -> loopback Main Gateway op 3 payloads (written to a scratch dir under $TMPDIR).
    add_test(NAME rich_presence_verify COMMAND ccord --headless --mock --verify-slice rich-presence)
    set_tests_properties(rich_presence_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")

    # End-to-end --verify-slice expressions: GIF catalog/search/favorites + Emoji rail over forked
    # loopback HTTP + Main Gateway, through the real live_transport/session/store/ui/renderer.
    add_test(NAME expressions_verify COMMAND ccord --headless --verify-slice expressions)
    set_tests_properties(expressions_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")

    # End-to-end --verify-slice messaging / settings-proto / events-stage (Results 1-3): outgoing
    # messaging + app commands, lossless favorite-GIF settings-proto writeback (live + mock), and
    # scheduled events + stage instances, over forked loopback HTTP + Main Gateway servers.
    add_test(NAME messaging_verify COMMAND ccord --headless --verify-slice messaging)
    set_tests_properties(messaging_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME settings_proto_verify COMMAND ccord --headless --verify-slice settings-proto)
    set_tests_properties(settings_proto_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME events_stage_verify COMMAND ccord --headless --verify-slice events-stage)
    set_tests_properties(events_stage_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")

    # End-to-end --verify-slice membership-screening: invite join -> pending composer lock + session send gate ->
    # rules modal (GET member-verification) -> gated Submit (PUT requests/@me) -> gateway pending=false -> send,
    # over forked loopback HTTP + Main Gateway servers, plus the mock transport's 'mv-test' flow.
    add_test(NAME membership_screening_verify COMMAND ccord --headless --verify-slice membership-screening)
    set_tests_properties(membership_screening_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")

    # End-to-end --verify-slice components-v2: a gateway MESSAGE_CREATE with a flag-32768 Components v2 tree renders
    # (all seven kinds, no duplicate message-level attachments) and its nested button/select POST /interactions; a legacy
    # v1 message is unchanged. Forked loopback HTTP + Main Gateway, the real parser/session/store/ui/renderer.
    add_test(NAME components_v2_verify COMMAND ccord --headless --mock --verify-slice components-v2)
    set_tests_properties(components_v2_verify PROPERTIES TIMEOUT 120 LABELS "integration;live"
                         ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    # End-to-end --verify-slice activities: the Activities chip lists the shelf in shelf_rank order, a click launches
    # (POST /activities/<channel>/<app> with the READY session_id), gateway EMBEDDED_ACTIVITY_UPDATE_V2 updates drive
    # the instance cards (join, second user, Leave, ended), voice disconnect leaves, plus the mock transport leg.
    add_test(NAME activities_verify COMMAND ccord --headless --mock --verify-slice activities)
    set_tests_properties(activities_verify PROPERTIES TIMEOUT 120 LABELS "integration;live"
                         ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

    # End-to-end --verify-slice voice-messages: UI recorder PCM seam -> real Ogg/Opus encoder -> UI Send ->
    # 3-step upload (slot, PUT, message POST with flags 8192 + duration_secs + waveform) over forked loopback HTTP,
    # a received flag-8192 message drawn by the waveform player, mock parity and the no-ATTACH_FILES refusal.
    add_test(NAME voice_messages_verify COMMAND ccord --headless --mock --verify-slice voice-messages)
    set_tests_properties(voice_messages_verify PROPERTIES TIMEOUT 120 LABELS "integration;live"
                         ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
  endif()

  # M5 test fragment.
  include(tests/m5_verify.cmake)
  # M6 desktop glue: startup INI prefs, host callbacks, tray badge, notification gating, hotkeys.
  include(tests/m6_app.cmake)
  # M6 end-to-end --verify-slice m6 harness (runs the real binary offline).
  include(tests/m6_verify.cmake)
endif()

if(NOT ANDROID)
  # Desktop integration, runtime assets, and bundled extensions (written under the install prefix).
  install(TARGETS ccord RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets/fonts ${CMAKE_SOURCE_DIR}/assets/themes
          DESTINATION ${CMAKE_INSTALL_DATADIR}/ccord)
  install(DIRECTORY ${CMAKE_SOURCE_DIR}/extensions
          DESTINATION ${CMAKE_INSTALL_DATADIR}/ccord)
  if(UNIX AND NOT APPLE)
    install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/C-Cord.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    foreach(sz 16 22 24 32 48 64 128 256 512)
      install(FILES ${CMAKE_SOURCE_DIR}/assets/icons/ccord-${sz}.png
              DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${sz}x${sz}/apps RENAME C-Cord.png)
    endforeach()
  elseif(APPLE)
    configure_file(${CMAKE_SOURCE_DIR}/assets/macos/Info.plist.in
                   ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/Info.plist DESTINATION ${CMAKE_INSTALL_DATADIR}/ccord)
  endif()
endif()
