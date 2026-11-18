# M5 test fragment — included from src/app/CMakeLists.txt under CCORD_BUILD_TESTS; paths are
# relative to src/app.

# m5-app-media: runtime media coordinator (settings sync, PTT, device hot-plug, Go Live, soundboard).
# All host seams are faked in the test; only ccord_core (alloc/error/log) is real. ccord_ui is linked
# purely to propagate the model/platform/ui header include dirs app_media.h needs.
ccord_add_test(NAME test_app_media SOURCES tests/test_app_media.c app_media.c
               LIBS ccord_ui ccord_core LABELS integration voice)
target_include_directories(test_app_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# m5-verify: --verify-slice m5 end-to-end harness. Runs the REAL ccord binary offline (mock transport,
# headless, dummy audio) and asserts every M5 check line, the PASS summary, and the exit codes.
ccord_add_test(NAME test_m5_verify SOURCES tests/test_m5_verify.c LABELS integration voice)
target_compile_definitions(test_m5_verify PRIVATE CCORD_BIN="$<TARGET_FILE:ccord>")
add_dependencies(test_m5_verify ccord)
set_tests_properties(test_m5_verify PROPERTIES TIMEOUT 180)
