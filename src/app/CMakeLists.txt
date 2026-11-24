add_library(ccord_account STATIC storage/account.c)
target_include_directories(ccord_account PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/storage)
target_link_libraries(ccord_account PUBLIC ccord_ui_logic)
ccord_strict_warnings(ccord_account)

set(CCORD_APP_SOURCES
  main.c
  cli.c
  host_callbacks.c
  account_bridge.c
  plugins_bridge.c
  media_bridge.c
  desktop/app_desktop.c
  desktop/app_ipc.c
  desktop/earcons.c
  desktop/scheduled_messages.c
  desktop/updater.c
  media/app_media.c
  media/app_media_audio.c
  media/app_media_video.c
  media/voice_audio_thread.c
  storage/app_profile_rest.c
  storage/warm_cache.c
  storage/warm_cache_codec.c
  rpc/app_rpc.c
  rpc/game_detect_app.c
  rpc/rpc_app_lookup.c)

if(ANDROID)
  # SDL3's Android SDLActivity loads libmain.so and calls SDL_main().
  set(CCORD_APP_TARGET main)
  add_library(main SHARED ${CCORD_APP_SOURCES})
  # Static FFmpeg AArch64 NEON assembly references internal FFmpeg tables with PC-relative relocations;
  # -Bsymbolic binds internal symbols locally inside libmain.so without disabling PIC or textrels.
  # The version script hides static third-party symbols so dlopen() relocations stay minimal.
  set(CCORD_ANDROID_LIBMAIN_MAP "${CMAKE_SOURCE_DIR}/android/app/libmain.map")
  target_link_options(main PRIVATE
    -Wl,-Bsymbolic
    "-Wl,--version-script=${CCORD_ANDROID_LIBMAIN_MAP}")
  set_target_properties(main PROPERTIES LINK_DEPENDS "${CCORD_ANDROID_LIBMAIN_MAP}")
else()
  set(CCORD_APP_TARGET ccord)
  add_executable(ccord ${CCORD_APP_SOURCES})
endif()

if(WIN32)
  target_sources(${CCORD_APP_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/assets/windows/ccord.rc)
  target_link_libraries(${CCORD_APP_TARGET} PRIVATE advapi32)
endif()

set(CCORD_APP_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/desktop
  ${CMAKE_CURRENT_SOURCE_DIR}/media
  ${CMAKE_CURRENT_SOURCE_DIR}/storage
  ${CMAKE_CURRENT_SOURCE_DIR}/rpc)

target_include_directories(${CCORD_APP_TARGET} PRIVATE ${CCORD_APP_INCLUDE_DIRS})
target_link_libraries(${CCORD_APP_TARGET} PRIVATE
  ccord_core ccord_assets ccord_model ccord_contrib ccord_store ccord_net ccord_session
  ccord_theme ccord_platform ccord_render ccord_ui ccord_account ccord_rpc
  ccord_plugin_host_core ccord_plugins ccord_ipc ccord_lua SDL3::SDL3)
ccord_strict_warnings(${CCORD_APP_TARGET})

target_compile_definitions(${CCORD_APP_TARGET} PRIVATE
  CCORD_BUILD_VERIFY=$<BOOL:${CCORD_BUILD_TESTS}>
  CCORD_LIVE_ACCOUNT=$<BOOL:${CCORD_LIVE_ACCOUNT}>
  CCORD_WITH_PLUGINS=1)

if(CCORD_BUILD_TESTS)
  add_subdirectory(verify)
  target_link_libraries(${CCORD_APP_TARGET} PRIVATE ccord_verify)
endif()

if(ANDROID)
  target_compile_definitions(${CCORD_APP_TARGET} PRIVATE CCORD_PLUGIN_HOST_PATH="" CCORD_EXTENSIONS_DIR="")
else()
  target_compile_definitions(${CCORD_APP_TARGET} PRIVATE
    CCORD_PLUGIN_HOST_PATH="$<IF:$<BOOL:${CCORD_BUILD_TESTS}>,$<TARGET_FILE:ccord-plugin-host>,${CMAKE_INSTALL_FULL_BINDIR}/ccord-plugin-host>"
    CCORD_EXTENSIONS_DIR="$<IF:$<BOOL:${CCORD_BUILD_TESTS}>,${CMAKE_SOURCE_DIR}/extensions,${CMAKE_INSTALL_FULL_DATADIR}/ccord/extensions>")
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
    CCORD_VERSION_STRING="${CCORD_VERSION_CLEAN}"
    CCORD_LIVE_ACCOUNT=$<BOOL:${CCORD_LIVE_ACCOUNT}>)
  add_dependencies(test_app_smoke ccord)

  # M0-7: --net-stats + --verify-slice m0 (runs the real binary offline).
  ccord_add_test(NAME test_cli_stats SOURCES tests/test_cli_stats.c LABELS integration)
  target_compile_definitions(test_cli_stats PRIVATE CCORD_BIN="$<TARGET_FILE:ccord>")
  add_dependencies(test_cli_stats ccord)

  ccord_add_test(NAME test_app_account SOURCES tests/test_app_account.c
                 LIBS ccord_account ccord_session LABELS integration)
  target_compile_definitions(test_app_account PRIVATE CCORD_BIN="$<TARGET_FILE:ccord>")
  target_include_directories(test_app_account PRIVATE ${CCORD_APP_INCLUDE_DIRS})
  add_dependencies(test_app_account ccord)

  # Rich Presence app wiring: per-connection slots, trailing-edge coalescing and clear paths over a real
  # AF_UNIX socket against a mock-transport session (hermetic; scratch dir under $TMPDIR).
  ccord_add_test(NAME test_app_rpc SOURCES tests/test_app_rpc.c rpc/app_rpc.c rpc/game_detect_app.c rpc/rpc_app_lookup.c
                 LIBS ccord_rpc ccord_session ccord_store ccord_net LABELS integration)
  target_include_directories(test_app_rpc PRIVATE ${CCORD_APP_INCLUDE_DIRS})

  # Application-name lookup: cache, fallback and backoff rules over a scripted source (no network).
  ccord_add_test(NAME test_rpc_app_lookup SOURCES tests/test_rpc_app_lookup.c rpc/rpc_app_lookup.c
                 LIBS ccord_rpc ccord_net LABELS unit)
  target_include_directories(test_rpc_app_lookup PRIVATE ${CCORD_APP_INCLUDE_DIRS})

  # Own-profile REST backend wiring: --mock edit through the session, live token follows account switch/sign-out.
  ccord_add_test(NAME test_app_profile_rest SOURCES tests/test_app_profile_rest.c storage/app_profile_rest.c
                 LIBS ccord_session ccord_store ccord_net LABELS integration)
  target_include_directories(test_app_profile_rest PRIVATE ${CCORD_APP_INCLUDE_DIRS})
  target_compile_definitions(test_app_profile_rest PRIVATE CCORD_LIVE_ACCOUNT=$<BOOL:${CCORD_LIVE_ACCOUNT}>)

  # Warm-start disk cache: round trip, modes, integrity rejection, account isolation, forget/prune/cap.
  ccord_add_test(NAME test_warm_cache SOURCES tests/test_warm_cache.c storage/warm_cache.c storage/warm_cache_codec.c
                 LIBS ccord_session ccord_store ccord_net LABELS unit)
  target_include_directories(test_warm_cache PRIVATE ${CCORD_APP_INCLUDE_DIRS})
  target_compile_definitions(test_warm_cache PRIVATE CCORD_LIVE_ACCOUNT=$<BOOL:${CCORD_LIVE_ACCOUNT}>)

  # Scheduled messages: lazy dir creation, O(1) has_entries fast-path, offline next_due_ms.
  ccord_add_test(NAME test_scheduled_messages SOURCES tests/test_scheduled_messages.c desktop/scheduled_messages.c
                 LIBS ccord_session ccord_store ccord_net ccord_ui LABELS unit)
  target_include_directories(test_scheduled_messages PRIVATE ${CCORD_APP_INCLUDE_DIRS})

  ccord_add_test(NAME test_updater SOURCES tests/test_updater.c desktop/updater.c
                 LIBS ccord_core ccord_net ccord_ui LABELS unit)
  target_include_directories(test_updater PRIVATE ${CCORD_APP_INCLUDE_DIRS})

  if(CCORD_LIVE_ACCOUNT)
    ccord_add_test(NAME test_voice_audio_thread SOURCES tests/test_voice_audio_thread.c media/voice_audio_thread.c
                   LIBS ccord_net LABELS integration voice)
    target_include_directories(test_voice_audio_thread PRIVATE
      ${CCORD_APP_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src/platform/include)
    set_tests_properties(test_voice_audio_thread PROPERTIES TIMEOUT 60)

    add_test(NAME live_stream_verify COMMAND ccord --headless --verify-slice live-stream)
    set_tests_properties(live_stream_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME stream_audio_verify COMMAND ccord --headless --verify-slice stream-audio)
    set_tests_properties(stream_audio_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME rich_presence_verify COMMAND ccord --headless --mock --verify-slice rich-presence)
    set_tests_properties(rich_presence_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME expressions_verify COMMAND ccord --headless --verify-slice expressions)
    set_tests_properties(expressions_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME messaging_verify COMMAND ccord --headless --verify-slice messaging)
    set_tests_properties(messaging_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME settings_proto_verify COMMAND ccord --headless --verify-slice settings-proto)
    set_tests_properties(settings_proto_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME events_stage_verify COMMAND ccord --headless --verify-slice events-stage)
    set_tests_properties(events_stage_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME membership_screening_verify COMMAND ccord --headless --verify-slice membership-screening)
    set_tests_properties(membership_screening_verify PROPERTIES TIMEOUT 120 LABELS "integration;live")
    add_test(NAME components_v2_verify COMMAND ccord --headless --mock --verify-slice components-v2)
    set_tests_properties(components_v2_verify PROPERTIES TIMEOUT 120 LABELS "integration;live"
                         ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    add_test(NAME activities_verify COMMAND ccord --headless --mock --verify-slice activities)
    set_tests_properties(activities_verify PROPERTIES TIMEOUT 120 LABELS "integration;live"
                         ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    add_test(NAME voice_messages_verify COMMAND ccord --headless --mock --verify-slice voice-messages)
    set_tests_properties(voice_messages_verify PROPERTIES TIMEOUT 120 LABELS "integration;live"
                         ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
  endif()

  ccord_add_test(NAME test_app_media
                 SOURCES tests/test_app_media.c media/app_media.c media/app_media_audio.c media/app_media_video.c
                 LIBS ccord_ui ccord_core LABELS integration voice)
  target_include_directories(test_app_media PRIVATE ${CCORD_APP_INCLUDE_DIRS})

  ccord_add_test(NAME test_m5_verify SOURCES tests/test_m5_verify.c LABELS integration voice)
  target_compile_definitions(test_m5_verify PRIVATE CCORD_BIN="$<TARGET_FILE:ccord>")
  add_dependencies(test_m5_verify ccord)
  set_tests_properties(test_m5_verify PROPERTIES TIMEOUT 180)

  ccord_add_test(NAME test_app_desktop
                 SOURCES tests/test_app_desktop.c desktop/app_desktop.c desktop/app_ipc.c
                 LIBS ccord_ui ccord_ui_internal_inc ccord_platform ccord_net SDL3::SDL3 LABELS integration ui)
  target_include_directories(test_app_desktop PRIVATE ${CCORD_APP_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/src/ui)

  ccord_add_test(NAME test_verify_m6 SOURCES tests/test_verify_m6.c LABELS integration ui)
  target_compile_definitions(test_verify_m6 PRIVATE CCORD_BIN="$<TARGET_FILE:ccord>")
  add_dependencies(test_verify_m6 ccord)
  set_tests_properties(test_verify_m6 PROPERTIES TIMEOUT 180)
endif()

if(NOT ANDROID)
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
