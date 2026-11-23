include(CCordDeps)
ccord_find_zlib()
find_package(Threads REQUIRED)
add_library(ccord_plugins STATIC
  core/plugins.c
  core/plugin_host_proc.c
  core/plugin_storage.c
  core/plugin_rpc_core.c
  package/plugin_package.c
  package/manifest.c
  package/zip.c
  api_v3/api_v3_system.c
  api_v3/api_v3_ui.c
  api_v3/api_v3_discord.c
  workers/worker_queue.c
  workers/worker_net.c
  workers/worker_fs_proc.c
  catalog/catalog.c
  catalog/catalog_parse.c
  catalog/catalog_store.c
)
add_library(ccord::plugins ALIAS ccord_plugins)
target_include_directories(ccord_plugins
  PUBLIC include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ccord_plugins PUBLIC ccord_core ccord_model ccord_contrib ccord_store ccord_session ccord_ipc ZLIB::ZLIB Threads::Threads)

set(_ccord_plugins_sodium "")
if(TARGET PkgConfig::CCORD_SODIUM)
  set(_ccord_plugins_sodium PkgConfig::CCORD_SODIUM)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(CCORD_PLUGINS_SODIUM QUIET IMPORTED_TARGET libsodium)
  endif()
  if(CCORD_PLUGINS_SODIUM_FOUND)
    set(_ccord_plugins_sodium PkgConfig::CCORD_PLUGINS_SODIUM)
  endif()
endif()
if(_ccord_plugins_sodium)
  target_link_libraries(ccord_plugins PRIVATE ${_ccord_plugins_sodium})
  target_compile_definitions(ccord_plugins PRIVATE CCORD_HAVE_SODIUM=1)
endif()

set(CCORD_CATALOG_PUBKEY "" CACHE STRING "Optional comma-separated 64-hex Ed25519 public key(s) for plugin catalog verification")
if(CCORD_CATALOG_PUBKEY)
  target_compile_definitions(ccord_plugins PRIVATE CCORD_CATALOG_PUBKEY="${CCORD_CATALOG_PUBKEY}")
endif()

ccord_strict_warnings(ccord_plugins)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_plugin_catalog SOURCES tests/test_plugin_catalog.c LIBS ccord_plugins LABELS unit)
  target_include_directories(test_plugin_catalog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/catalog)
  if(_ccord_plugins_sodium)
    target_link_libraries(test_plugin_catalog PRIVATE ${_ccord_plugins_sodium})
    target_compile_definitions(test_plugin_catalog PRIVATE CCORD_HAVE_SODIUM=1)
  endif()
endif()
