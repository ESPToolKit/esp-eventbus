# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]
### Added
- FreeRTOS-backed `EventBus` with async posting, ISR-safe APIs, subscription management, and blocking `waitFor` helpers.
- Configurable worker task (queue length, stack size, priority, core affinity, name).
- `waitFor` helper implemented with per-call queues so any task can synchronously wait for the next event payload.

### Maintenance
- Replaced the placeholder logger code with the actual event bus implementation and refreshed the example, README, and metadata to reflect the new library focus.
