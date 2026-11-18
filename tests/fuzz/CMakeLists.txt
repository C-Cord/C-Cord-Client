# Fuzz harnesses for the untrusted-input parsers. See cmake/CCordFuzz.cmake for the two modes:
#  - default (GCC/Clang, normal or CCORD_SANITIZE): deterministic corpus driver, registered as CTest targets.
#  - CCORD_FUZZ=ON (Clang): libFuzzer + a bounded -runs=2000 smoke test.
# Only the harness translation units are listed here; the driver is added by ccord_add_fuzz.

ccord_add_fuzz(NAME fuzz_discord_proto SOURCES fuzz_discord_proto.c LIBS ccord_net)
ccord_add_fuzz(NAME fuzz_rtp_h264 SOURCES fuzz_rtp_h264.c LIBS ccord_net)
ccord_add_fuzz(NAME fuzz_parsers SOURCES fuzz_parsers.c LIBS ccord_core ccord_model ccord_theme ccord_emoji)

# discord_json.c only compiles when the live account transport is enabled.
if(CCORD_LIVE_ACCOUNT)
  ccord_add_fuzz(NAME fuzz_discord_json SOURCES fuzz_discord_json.c LIBS ccord_net)
endif()
