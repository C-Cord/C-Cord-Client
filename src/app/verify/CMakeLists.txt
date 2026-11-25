# Verification slice library (built ONLY when CCORD_BUILD_TESTS=ON).
set(CCORD_VERIFY_SOURCES
  verify_dispatch.c
  verify_m0.c
  verify_m4c.c
  verify_m4d.c
  verify_m5.c
  verify_m6.c
  verify_call_bar.c
  verify_search_view.c
  verify_picker_layout.c
  verify_voice_text_chat.c
  verify_earcons.c
  verify_reply_style.c
  verify_ui_style.c
  verify_panel_toggles.c
  verify_smooth_scroll.c
  verify_motion.c
  verify_motion_overlays.c
  verify_motion_nav.c
  verify_motion_chat.c
  verify_motion_controls.c
  verify_hover_edit.c
  verify_status_card.c
  verify_jump_present.c
  verify_ccord_settings.c
  verify_game_detect.c
  verify_profile_edit.c
  verify_reaction_once.c
  verify_rpc_app_name.c
  verify_streamer_mode.c
  verify_warm_start.c
  verify_plus_menu.c
  verify_poll_create.c
  verify_schedule_message.c
  verify_fav_gif_order.c
  verify_tray_unread.c
  verify_close_background.c
  verify_app_icon.c
  verify_status_sync.c
  verify_blocked_hide.c
  verify_ctx_react.c
  verify_new_messages_bar.c
  verify_report_flow.c
  verify_call_ring.c
  verify_global_ptt.c
  verify_voice_reconnect.c
  verify_multi_camera.c
  verify_notify_jump.c
  verify_resume_reconnect.c
  verify_profile_view.c
  verify_profile_roles.c
  verify_rail_scroll.c
  verify_composer_format.c
  verify_settings_profile.c
  verify_glyph_fallback.c
  verify_ansi_block.c)

if(CCORD_LIVE_ACCOUNT)
  list(APPEND CCORD_VERIFY_SOURCES
    verify_live_stream.c
    verify_stream_audio.c
    verify_expressions.c
    verify_messaging_events.c
    verify_rich_presence.c
    verify_membership_screening.c
    verify_components_v2.c
    verify_activities.c
    verify_voice_messages.c
    verify_gif_favorites.c
    verify_embed_video.c
    verify_video_stream.c
    verify_media_load.c)
endif()

add_library(ccord_verify STATIC ${CCORD_VERIFY_SOURCES})
target_include_directories(ccord_verify PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ccord_verify PRIVATE
  ${CMAKE_SOURCE_DIR}/src/app
  ${CMAKE_SOURCE_DIR}/src/app/desktop
  ${CMAKE_SOURCE_DIR}/src/app/media
  ${CMAKE_SOURCE_DIR}/src/app/storage
  ${CMAKE_SOURCE_DIR}/src/app/rpc
  ${CMAKE_SOURCE_DIR}/src/ui)
if(CCORD_LIVE_ACCOUNT)
  target_include_directories(ccord_verify PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)
endif()
target_link_libraries(ccord_verify PRIVATE
  ccord_core ccord_assets ccord_model ccord_store ccord_net ccord_session
  ccord_theme ccord_platform ccord_render ccord_ui ccord_ui_internal_inc ccord_account ccord_rpc ccord_plugins SDL3::SDL3)
target_compile_definitions(ccord_verify PRIVATE
  CCORD_BUILD_VERIFY=1
  CCORD_WITH_PLUGINS=1
  CCORD_LIVE_ACCOUNT=$<BOOL:${CCORD_LIVE_ACCOUNT}>
  CCORD_WEB_CAPTCHA=$<BOOL:${CCORD_WEB_CAPTCHA}>)
ccord_strict_warnings(ccord_verify)
