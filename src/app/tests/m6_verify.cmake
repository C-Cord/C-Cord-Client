# M6 test fragment — included from src/app/CMakeLists.txt under CCORD_BUILD_TESTS; paths are
# relative to src/app.

# m6-verify: --verify-slice m6 end-to-end harness. Runs the REAL ccord binary offline (mock transport,
# headless, dummy video+audio) and asserts every M6 check line, the 12/12 + PASS summary, and exit codes.
ccord_add_test(NAME test_verify_m6 SOURCES tests/test_verify_m6.c LABELS integration ui)
target_compile_definitions(test_verify_m6 PRIVATE CCORD_BIN="$<TARGET_FILE:ccord>")
add_dependencies(test_verify_m6 ccord)
set_tests_properties(test_verify_m6 PROPERTIES TIMEOUT 180)
