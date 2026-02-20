# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]
### Added
- Added `std::function` subscription overloads so callers can bind member methods or use capturing lambdas.
- Added `EventBusConfig::usePSRAMBuffers` and routed subscription/fan-out vectors through `ESPBufferManager` with automatic fallback when PSRAM is unavailable.
- Added a platform-gated static FreeRTOS allocation path (`configSUPPORT_STATIC_ALLOCATION == 1`) so `usePSRAMBuffers` also covers worker queue/mutex storage via `ESPBufferManager`, with fallback to dynamic FreeRTOS allocation when unavailable.
- Migrated worker task creation/lifecycle to `ESPWorker` and removed direct `xTaskCreate*`/`vTaskDelete` usage in runtime code.

### Fixed
- Disambiguated the README and `examples/basic_usage` subscription callback to avoid overload ambiguity on Arduino.
- Hardened worker task creation by always using `ESPWorker::spawn(...)` (non-caps path) to avoid ESP32 `xPortCheckValidTCBMem(...)` asserts observed with caps/static task creation on some ports.

## [1.0.0] - 2025-11-19
### Added
- FreeRTOS-backed `ESPEventBus` with async posting, ISR-safe APIs, subscription management, and blocking `waitFor` helpers.
- Configurable worker task (queue length, stack size, priority, core affinity, name).
- `waitFor` helper implemented with per-call queues so any task can synchronously wait for the next event payload.

### Maintenance
- Replaced the placeholder logger code with the actual event bus implementation and refreshed the example, README, and metadata to reflect the new library focus.

[Unreleased]: https://github.com/ESPToolKit/esp-eventbus/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/ESPToolKit/esp-eventbus/releases/tag/v1.0.0
