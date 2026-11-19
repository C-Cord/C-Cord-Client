find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CCORD_EMBEDDED_ASSETS_C "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.c")
set(CCORD_EMBEDDED_INPUTS
  "${CMAKE_SOURCE_DIR}/assets/themes/c-cord-dark.theme"
  "${CMAKE_SOURCE_DIR}/assets/themes/c-cord-light.theme"
  "${CMAKE_SOURCE_DIR}/assets/themes/c-cord-amoled.theme"
  "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf"
  "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Bold.ttf"
  "${CMAKE_SOURCE_DIR}/extensions/copy-message-id/plugin.json"
  "${CMAKE_SOURCE_DIR}/extensions/copy-message-id/main.lua")

add_custom_command(
  OUTPUT "${CCORD_EMBEDDED_ASSETS_C}"
  COMMAND "${Python3_EXECUTABLE}" -c
    "import sys, pathlib; root = pathlib.Path(sys.argv[1]); out = pathlib.Path(sys.argv[2]); \
     f = lambda p: root.joinpath(p).read_bytes(); \
     arr = lambda b: ','.join(str(x) for x in b); \
     t = f('assets/themes/c-cord-dark.theme'); \
     tl = f('assets/themes/c-cord-light.theme'); \
     ta = f('assets/themes/c-cord-amoled.theme'); \
     fr = f('assets/fonts/NotoSans-Regular.ttf'); \
     fb = f('assets/fonts/NotoSans-Bold.ttf'); \
     pj = f('extensions/copy-message-id/plugin.json'); \
     ml = f('extensions/copy-message-id/main.lua'); \
     code = '#include <ccord/core/atomic_file.h>\\n#include <ccord/core/paths.h>\\n#include <stdio.h>\\n#include <string.h>\\n' + \
            f'static const unsigned char k_theme[] = {{{arr(t)}, 0}};\\n' + \
            f'static const unsigned char k_theme_light[] = {{{arr(tl)}, 0}};\\n' + \
            f'static const unsigned char k_theme_amoled[] = {{{arr(ta)}, 0}};\\n' + \
            f'static const unsigned char k_font_reg[] = {{{arr(fr)}}};\\n' + \
            f'static const unsigned char k_font_bold[] = {{{arr(fb)}}};\\n' + \
            f'static const unsigned char k_ext_json[] = {{{arr(pj)}}};\\n' + \
            f'static const unsigned char k_ext_lua[] = {{{arr(ml)}}};\\n' + \
            'const char *ccord_embedded_theme(size_t *out_len) { if (out_len) *out_len = sizeof(k_theme) - 1; return (const char *)k_theme; }\\n' + \
            'const char *ccord_embedded_theme_by_id(const char *id, size_t *out_len);\\n' + \
            'const char *ccord_embedded_theme_by_id(const char *id, size_t *out_len) {\\n' + \
            '  if (!id) return nullptr;\\n' + \
            '  if (strcmp(id, \"c-cord-dark\") == 0) { if (out_len) *out_len = sizeof(k_theme) - 1; return (const char *)k_theme; }\\n' + \
            '  if (strcmp(id, \"c-cord-light\") == 0) { if (out_len) *out_len = sizeof(k_theme_light) - 1; return (const char *)k_theme_light; }\\n' + \
            '  if (strcmp(id, \"c-cord-amoled\") == 0) { if (out_len) *out_len = sizeof(k_theme_amoled) - 1; return (const char *)k_theme_amoled; }\\n' + \
            '  return nullptr;\\n' + \
            '}\\n' + \
            'const unsigned char *ccord_embedded_font(const char *rel, size_t *out_len) {\\n' + \
            '  if (!rel) return nullptr;\\n' + \
            '  if (strstr(rel, \"Bold\") != nullptr) { if (out_len) *out_len = sizeof(k_font_bold); return k_font_bold; }\\n' + \
            '  if (out_len) *out_len = sizeof(k_font_reg);\\n' + \
            '  return k_font_reg;\\n' + \
            '}\\n' + \
            'static bool write_blob(const char *path, const unsigned char *data, size_t len) {\\n' + \
            '  FILE *fp = fopen(path, \"wb\"); if (!fp) return false;\\n' + \
            '  bool ok = fwrite(data, 1, len, fp) == len; fclose(fp); return ok;\\n' + \
            '}\\n' + \
            'bool ccord_embedded_materialize_extensions(const char *dest_dir) {\\n' + \
            '  if (!dest_dir || !dest_dir[0]) return false;\\n' + \
            '  ccord_error err;\\n' + \
            '  if (ccord_mkdir_p(dest_dir, 0700, &err) != CCORD_OK) return false;\\n' + \
            '  char sub[4096], p1[4096], p2[4096];\\n' + \
            '  if ((size_t)snprintf(sub, sizeof sub, \"%s/copy-message-id\", dest_dir) >= sizeof sub) return false;\\n' + \
            '  if (ccord_mkdir_p(sub, 0700, &err) != CCORD_OK) return false;\\n' + \
            '  if ((size_t)snprintf(p1, sizeof p1, \"%s/plugin.json\", sub) >= sizeof p1) return false;\\n' + \
            '  if ((size_t)snprintf(p2, sizeof p2, \"%s/main.lua\", sub) >= sizeof p2) return false;\\n' + \
            '  return write_blob(p1, k_ext_json, sizeof k_ext_json) && write_blob(p2, k_ext_lua, sizeof k_ext_lua);\\n' + \
            '}\\n'; \
     out.write_text(code)"
    "${CMAKE_SOURCE_DIR}" "${CCORD_EMBEDDED_ASSETS_C}"
  DEPENDS ${CCORD_EMBEDDED_INPUTS}
  VERBATIM
  COMMENT "Generating embedded assets for single-binary ccord")

if(WIN32)
  set(CCORD_CORE_OS_SRC os_win32.c)
else()
  set(CCORD_CORE_OS_SRC os_posix.c)
  find_package(Threads REQUIRED)
endif()

add_library(ccord_core STATIC
  error.c log.c alloc.c arena.c buf.c utf8.c json.c ini.c clock.c paths.c event_bus.c idmap.c
  secret.c random.c sha256.c atomic_file.c ${CCORD_CORE_OS_SRC} "${CCORD_EMBEDDED_ASSETS_C}")
add_library(ccord::core ALIAS ccord_core)
target_include_directories(ccord_core PUBLIC include)
target_compile_definitions(ccord_core PUBLIC CCORD_VERSION_STRING="${CCORD_VERSION_CLEAN}")
target_include_directories(ccord_core PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_compile_definitions(ccord_core PRIVATE CCORD_DEFAULT_ASSET_DIR="$<IF:$<BOOL:${CCORD_BUILD_TESTS}>,${CCORD_ASSET_DIR},${CMAKE_INSTALL_FULL_DATADIR}/ccord>")
if(WIN32)
  target_link_libraries(ccord_core PUBLIC ws2_32 bcrypt shell32 ole32)
elseif(ANDROID)
  target_link_libraries(ccord_core PUBLIC Threads::Threads log)
else()
  target_link_libraries(ccord_core PUBLIC Threads::Threads)
endif()
ccord_strict_warnings(ccord_core)

if(CCORD_BUILD_TESTS)
  ccord_add_test(NAME test_event_bus SOURCES tests/test_event_bus.c LIBS ccord_core LABELS unit)
  ccord_add_test(NAME test_core SOURCES tests/test_core.c LIBS ccord_core LABELS unit)
  ccord_add_test(NAME test_core_json SOURCES tests/test_core_json.c LIBS ccord_core LABELS unit)
  ccord_add_test(NAME test_core_secret SOURCES tests/test_core_secret.c LIBS ccord_core LABELS unit)
  ccord_add_test(NAME test_core_sha256 SOURCES tests/test_core_sha256.c LIBS ccord_core LABELS unit)
  ccord_add_test(NAME test_core_idmap SOURCES tests/test_core_idmap.c LIBS ccord_core LABELS unit)
  ccord_add_test(NAME test_atomic_file SOURCES tests/test_atomic_file.c LIBS ccord_core LABELS unit)
endif()
