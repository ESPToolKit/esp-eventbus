# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]
### Added
- Added `std::function` subscription overloads so callers can bind member methods or use capturing lambdas.

### Fixed
- Disambiguated the README and `examples/basic_usage` subscription callback to avoid overload ambiguity on Arduino.

## [1.0.0] - 2025-11-19
### Added
- FreeRTOS-backed `ESPEventBus` with async posting, ISR-safe APIs, subscription management, and blocking `waitFor` helpers.
- Configurable worker task (queue length, stack size, priority, core affinity, name).
- `waitFor` helper implemented with per-call queues so any task can synchronously wait for the next event payload.

### Maintenance
- Replaced the placeholder logger code with the actual event bus implementation and refreshed the example, README, and metadata to reflect the new library focus.

[Unreleased]: https://github.com/ESPToolKit/esp-eventbus/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/ESPToolKit/esp-eventbus/releases/tag/v1.0.0
