# M6 test fragment — owned by m6-session-mock.
# store_user.c is a production source of ccord_store (listed in src/store/CMakeLists.txt) because
# the session links against it with CCORD_BUILD_TESTS=OFF too. This fragment registers its tests.
# Included from src/store/CMakeLists.txt under CCORD_BUILD_TESTS; paths are relative to src/store.
ccord_add_test(NAME test_store_user SOURCES tests/test_store_user.c LIBS ccord_store LABELS unit)
