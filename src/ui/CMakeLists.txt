# Pure logic, no SDL: layout, virtual list, text edit, time formatting.
add_library(ccord_ui_logic STATIC
  logic/layout.c
  logic/virtual_list.c
  logic/text_edit.c
  logic/timefmt.c
  logic/account_view.c
  logic/account_identity.c
  logic/anim.c
)
add_library(ccord::ui_logic ALIAS ccord_ui_logic)
target_include_directories(ccord_ui_logic PUBLIC include)
target_link_libraries(ccord_ui_logic PUBLIC ccord_core ccord_theme)
ccord_strict_warnings(ccord_ui_logic)

# Internal include-path set for ccord_ui, UI white-box tests, and ccord_verify (never linked by production ccord).
add_library(ccord_ui_internal_inc INTERFACE)
target_include_directories(ccord_ui_internal_inc INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/core
  ${CMAKE_CURRENT_SOURCE_DIR}/nav
  ${CMAKE_CURRENT_SOURCE_DIR}/chat
  ${CMAKE_CURRENT_SOURCE_DIR}/voice
  ${CMAKE_CURRENT_SOURCE_DIR}/overlays
  ${CMAKE_CURRENT_SOURCE_DIR}/expressions
  ${CMAKE_CURRENT_SOURCE_DIR}/settings
  ${CMAKE_CURRENT_SOURCE_DIR}/plugins
)

add_library(ccord_ui STATIC
  # Core
  core/ui_core.c
  core/ui_layout_geom.c
  core/ui_caches.c
  core/ui_sync.c
  core/ui_media.c
  core/ui_hover.c
  core/ui_input_nav.c
  core/ui_input.c
  core/ui_drag_context.c
  core/ui_input_mouse.c
  core/ui_frame.c
  core/ui_debug.c
  core/motion.c
  core/touch.c
  core/icons.c
  core/ui_style.c
  core/scroll.c
  # Navigation
  nav/draw_nav.c
  nav/draw_members.c
  nav/dm_views.c
  # Chat
  chat/md_inline.c
  chat/md_render.c
  chat/md_ansi.c
  chat/chat_header.c
  chat/chat_measure.c
  chat/chat_timeline.c
  chat/chat_cards.c
  chat/chat_embeds.c
  chat/chat_components.c
  chat/composer.c
  chat/composer_parse.c
  chat/composer_autocomplete.c
  chat/draw_forum.c
  chat/composer_format.c
  chat/composer_plus_menu.c
  chat/new_messages_bar.c
  chat/ui_voice_message.c
  # Voice
  voice/voice_stage.c
  voice/voice_activities.c
  voice/stream_viewer.c
  voice/ui_call.c
  voice/ui_dave.c
  voice/ui_scheduled_events.c
  # Overlays
  overlays/overlay_dispatch.c
  overlays/context_menu.c
  overlays/context_menu_actions.c
  overlays/header_popovers.c
  overlays/discovery.c
  overlays/member_verification.c
  overlays/profile_surfaces.c
  overlays/modals_draw.c
  overlays/modals_input.c
  overlays/lightbox_viewer.c
  overlays/ui_saved_messages.c
  overlays/status_card.c
  overlays/poll_composer.c
  overlays/schedule_dialog.c
  overlays/profile_view.c
  overlays/profile_roles.c
  overlays/clan_popover.c
  overlays/report_flow.c
  # Expressions
  expressions/expr_picker.c
  expressions/expr_tabs.c
  # Settings
  settings/settings_core.c
  settings/settings_ini.c
  settings/settings_input.c
  settings/settings_pages.c
  settings/settings_plugins.c
  settings/settings_account.c
  settings/settings_account_modals.c
  settings/settings_m6_devices.c
  settings/settings_m6_prefs.c
  settings/streamer_mask.c
  settings/settings_profile.c
  settings/settings_profile_pickers.c
  settings/server_settings_core.c
  settings/server_settings_widgets.c
  settings/server_settings_pages_a.c
  settings/server_settings_pages_b.c
  settings/server_settings_pages_c.c
  settings/server_settings_actions_a.c
  settings/server_settings_actions_b.c
  settings/channel_settings.c
  settings/channel_settings_pages.c
  # Plugins UI
  plugins/plugin_panel_compile.c
  plugins/plugin_panel_render.c
  plugins/plugin_v3_surfaces.c
  plugins/plugin_v3_modals.c
)
add_library(ccord::ui ALIAS ccord_ui)
target_include_directories(ccord_ui PUBLIC include)
target_link_libraries(ccord_ui
  PUBLIC ccord_ui_logic ccord_render ccord_session ccord_model ccord_contrib ccord_store ccord_emoji ccord_theme ccord_platform ccord_core ${CCORD_MATH_LIB}
  PRIVATE ccord_ui_internal_inc ccord_ipc SDL3::SDL3)
ccord_strict_warnings(ccord_ui)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_ui_logic SOURCES tests/test_ui_logic.c LIBS ccord_ui_logic LABELS unit)
  ccord_add_test(NAME test_ui_account_view SOURCES tests/test_ui_account_view.c LIBS ccord_ui_logic LABELS unit)
  ccord_add_test(NAME test_ui_anim SOURCES tests/test_ui_anim.c LIBS ccord_ui_logic LABELS unit)

  ccord_add_test(NAME test_ui_scroll SOURCES tests/test_ui_scroll.c LIBS ccord_ui ccord_ui_internal_inc LABELS unit)
  ccord_add_test(NAME test_ui_style_lint SOURCES tests/test_ui_style_lint.c LABELS unit)

  foreach(ui_inter_test IN ITEMS
    test_ui_interaction_core
    test_ui_interaction_chat
    test_ui_interaction_media
    test_ui_interaction_admin
    test_ui_interaction_ext)
    ccord_add_test(NAME ${ui_inter_test} SOURCES tests/${ui_inter_test}.c tests/ui_interaction_harness.c
                   LIBS ccord_ui ccord_ui_internal_inc ccord_platform ccord_net_mock SDL3::SDL3 LABELS ui)
  endforeach()

  foreach(ui_test IN ITEMS
    test_ui_mobile
    test_ui_components_v2
    test_ui_voice_message
    test_ui_style_c
    test_ui_style_x
    test_ui_style_helpers
    test_ui_style_s2_channel
    test_ui_style_n
    test_ui_style_s2
    test_ui_style_o
    test_cold_start_budget
    test_ui_profile_view
    test_ui_md_ansi
    test_ui_jumbo_emoji
    test_ui_composer_format
    test_ui_style_s1
    test_ui_voice_settings
    test_ui_voice_surfaces
    test_ui_ime
    test_ui_settings_devices
    test_ui_user_settings
    test_ui_account_profile)
    ccord_add_test(NAME ${ui_test} SOURCES tests/${ui_test}.c
                   LIBS ccord_ui ccord_ui_internal_inc ccord_platform ccord_net_mock SDL3::SDL3 LABELS ui)
  endforeach()
endif()
