# Host-side tests are not available yet because the ESPEventBus depends on
# FreeRTOS primitives that are only present on the ESP32 toolchains. This
# placeholder keeps the build system happy until on-device tests are added.
message(STATUS "ESPEventBus: tests are disabled for the host build.")
