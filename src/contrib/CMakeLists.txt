add_library(ccord_contrib STATIC
  contrib_core.c
  contrib_panel_patch.c
  contrib_plugins.c
  contrib_ui_ext.c
  contrib_assets.c)
add_library(ccord::contrib ALIAS ccord_contrib)
target_include_directories(ccord_contrib PUBLIC include)
target_link_libraries(ccord_contrib PUBLIC ccord_core ccord_model)
ccord_strict_warnings(ccord_contrib)
