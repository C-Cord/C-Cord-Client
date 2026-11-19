# Host-private sandbox library: one Lua 5.5 state per plugin. Only the host and its tests use it.
add_library(ccord_host_sandbox STATIC sandbox.c)
target_link_libraries(ccord_host_sandbox PUBLIC ccord_lua ccord_core ccord_ipc)
ccord_strict_warnings(ccord_host_sandbox)

add_executable(ccord-plugin-host main.c)
target_link_libraries(ccord-plugin-host PRIVATE ccord_host_sandbox)
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

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_host_sandbox SOURCES tests/test_host_sandbox.c LIBS ccord_host_sandbox LABELS unit)
  target_include_directories(test_host_sandbox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  ccord_add_test(NAME test_plugins_integration SOURCES tests/test_plugins_integration.c LIBS ccord_plugins ccord_store ccord_model ccord_core LABELS integration)
  target_compile_definitions(test_plugins_integration PRIVATE CCORD_PLUGIN_HOST_BIN="$<TARGET_FILE:ccord-plugin-host>")
  add_dependencies(test_plugins_integration ccord-plugin-host)
endif()
