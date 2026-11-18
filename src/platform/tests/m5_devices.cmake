# m5-devices: audio device enumeration / device-id selection / hot-plug + camera plane.
ccord_add_test(NAME test_platform_devices
  SOURCES tests/test_platform_devices.c
  LIBS ccord_platform SDL3::SDL3
  LABELS unit)
# White-box access to devices_internal.h (struct ccord_audio, registry helpers).
target_include_directories(test_platform_devices PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
