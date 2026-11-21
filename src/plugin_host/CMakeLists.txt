include(CCordDeps)
ccord_find_zlib()

# Host-private sandbox library: one Lua 5.5 state per plugin.
add_library(ccord_host_sandbox STATIC
  sandbox/sandbox_core.c
  sandbox/sandbox_api_v1.c
  sandbox/sandbox_api_v2.c
  sandbox/sandbox_api_sys.c
  sandbox/sandbox_libs.c
  sandbox/sandbox_v3_ui.c
  sandbox/sandbox_v3_discord.c
  sandbox/sandbox_v3_sys.c
)
target_include_directories(ccord_host_sandbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccord_host_sandbox PUBLIC ccord_lua ccord_core ccord_ipc ccord_emoji ZLIB::ZLIB)
if(NOT MSVC)
  target_link_libraries(ccord_host_sandbox PRIVATE m)
endif()
ccord_strict_warnings(ccord_host_sandbox)

# Reusable plugin-host runtime (IPC loop + OS containment + Lua sandbox).
# Linked by standalone ccord-plugin-host, embedded ccord (--plugin-host / Android), and mobile host tests.
add_library(ccord_plugin_host_core STATIC host/host_runtime.c host/os_sandbox.c)
add_library(ccord::plugin_host_core ALIAS ccord_plugin_host_core)
target_include_directories(ccord_plugin_host_core PUBLIC include)
target_link_libraries(ccord_plugin_host_core PUBLIC ccord_host_sandbox)
ccord_strict_warnings(ccord_plugin_host_core)

if(NOT ANDROID)
  add_executable(ccord-plugin-host main.c)
  target_link_libraries(ccord-plugin-host PRIVATE ccord_plugin_host_core)
  ccord_strict_warnings(ccord-plugin-host)

  add_custom_target(ccord_plugin_host ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ccord-plugin-host> $<TARGET_FILE_DIR:ccord-plugin-host>/ccord_plugin_host${CMAKE_EXECUTABLE_SUFFIX}
    DEPENDS ccord-plugin-host
    COMMENT "Creating ccord_plugin_host binary alias"
  )
  add_custom_command(TARGET ccord-plugin-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ccord-plugin-host> $<TARGET_FILE_DIR:ccord-plugin-host>/ccord_plugin_host${CMAKE_EXECUTABLE_SUFFIX}
    COMMENT "Creating ccord_plugin_host binary alias"
  )

  include(GNUInstallDirs)
  install(TARGETS ccord-plugin-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(CCORD_BUILD_TESTS AND NOT ANDROID)
  ccord_add_test(NAME test_host_sandbox SOURCES tests/test_host_sandbox.c LIBS ccord_host_sandbox LABELS unit)
  ccord_add_test(NAME test_plugins_integration SOURCES tests/test_plugins_integration.c LIBS ccord_plugins ccord_store ccord_model ccord_core LABELS integration)
  target_compile_definitions(test_plugins_integration PRIVATE CCORD_PLUGIN_HOST_BIN="$<TARGET_FILE:ccord-plugin-host>")
  add_dependencies(test_plugins_integration ccord-plugin-host)
  ccord_add_test(NAME test_plugins_mobile_api SOURCES tests/test_plugins_mobile_api.c LIBS ccord_plugin_host_core ccord_plugins ccord_store ccord_model ccord_core LABELS integration)
endif()
