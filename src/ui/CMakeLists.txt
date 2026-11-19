# Pure logic, no SDL: layout, virtual list, text edit, time formatting.
add_library(ccord_ui_logic STATIC layout.c virtual_list.c text_edit.c timefmt.c account_view.c account_identity.c anim.c)
add_library(ccord::ui_logic ALIAS ccord_ui_logic)
target_include_directories(ccord_ui_logic PUBLIC include)
target_link_libraries(ccord_ui_logic PUBLIC ccord_core ccord_theme)
ccord_strict_warnings(ccord_ui_logic)

add_library(ccord_ui STATIC ui.c motion.c draw_nav.c draw_chat.c draw_forum.c draw_members.c context_menu.c settings.c settings_m6_devices.c settings_m6_prefs.c server_settings.c channel_settings.c icons.c expression_picker.c ui_call.c ui_dave.c ui_scheduled_events.c ui_saved_messages.c ui_voice_message.c ui_style.c scroll.c status_card.c settings_profile.c composer_plus_menu.c poll_composer.c schedule_dialog.c profile_view.c new_messages_bar.c profile_roles.c clan_popover.c report_flow.c md_ansi.c composer_format.c plugin_panels.c)
add_library(ccord::ui ALIAS ccord_ui)
target_include_directories(ccord_ui PUBLIC include)
target_include_directories(ccord_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccord_ui
  PUBLIC ccord_ui_logic ccord_render ccord_session ccord_model ccord_store ccord_emoji ccord_theme ccord_platform ccord_core ${CCORD_MATH_LIB}
  PRIVATE SDL3::SDL3)
ccord_strict_warnings(ccord_ui)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_ui_logic SOURCES tests/test_ui_logic.c LIBS ccord_ui_logic LABELS unit)
  ccord_add_test(NAME test_ui_account_view SOURCES tests/test_ui_account_view.c LIBS ccord_ui_logic LABELS unit)
  ccord_add_test(NAME test_ui_anim SOURCES tests/test_ui_anim.c LIBS ccord_ui_logic LABELS unit)
endif()

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_ui_scroll SOURCES tests/test_ui_scroll.c LIBS ccord_ui LABELS unit)
  ccord_add_test(NAME test_ui_interaction SOURCES tests/test_ui_interaction.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_components_v2 SOURCES tests/test_ui_components_v2.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_voice_message SOURCES tests/test_ui_voice_message.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_style_c SOURCES tests/test_ui_style_c.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_style_x SOURCES tests/test_ui_style_x.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_style_helpers SOURCES tests/test_ui_style_helpers.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_style_s2_channel SOURCES tests/test_ui_style_s2_channel.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_style_n SOURCES tests/test_ui_style_n.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_style_s2 SOURCES tests/test_ui_style_s2.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_style_o SOURCES tests/test_ui_style_o.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_cold_start_budget SOURCES tests/test_cold_start_budget.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_profile_view SOURCES tests/test_ui_profile_view.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_md_ansi SOURCES tests/test_ui_md_ansi.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_composer_format SOURCES tests/test_ui_composer_format.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  ccord_add_test(NAME test_ui_style_lint SOURCES tests/test_ui_style_lint.c LABELS unit)
  ccord_add_test(NAME test_ui_style_s1 SOURCES tests/test_ui_style_s1.c
                 LIBS ccord_ui ccord_platform ccord_net SDL3::SDL3 LABELS ui)
  # M5 test fragments.
  include(tests/m5_uisettings.cmake)
  include(tests/m5_uisurfaces.cmake)
  # M6 test fragments.
  include(tests/m6_uiime.cmake)
  include(tests/m6_uisettings.cmake)
endif()
