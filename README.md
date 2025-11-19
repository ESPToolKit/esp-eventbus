# ESPEventBus

[![CI](https://github.com/ESPToolKit/esp-eventbus/actions/workflows/ci.yml/badge.svg)](https://github.com/ESPToolKit/esp-eventbus/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ESPToolKit/esp-eventbus?sort=semver)](https://github.com/ESPToolKit/esp-eventbus/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

An asynchronous, FreeRTOS-native event bus for ESP32 projects. Producers simply post events and keep running while the bus delivers payloads in its own worker task. Consumers can subscribe with callbacks or synchronously block on the next event with `waitFor`.

## Features

- Tiny API, single header include (`ESPEventBus.h`).
- Dedicated FreeRTOS worker task with configurable queue depth, stack size, priority, and core affinity.
- Thread-safe and ISR-safe posting (ISRs can queue events without waking the worker unless needed).
- Unlimited subscriptions per event with optional user data and one-shot semantics.
- Per-task `waitFor` helper implemented with short-lived queues so any task can await the next payload.

## Event IDs

The bus only needs a numeric ID internally, so the recommended pattern is to keep all application events in a shared header as a `enum class`:

```cpp
// app_events.h
#pragma once
#include <cstdint>

enum class AppEvent : uint16_t {
    NetworkGotIP,
    NetworkLostIP,
    LoggerFlush,
};
```

Every `EventBus` call is templated, so strongly typed enums work without casts:

```cpp
eventBus.post(AppEvent::NetworkGotIP, payload);
auto* payload = eventBus.waitFor(AppEvent::NetworkGotIP, portMAX_DELAY);
```

## Basic usage

```cpp
#include <Arduino.h>
#include <ESPEventBus.h>
#include "app_events.h"

struct NetworkGotIpPayload {
    String ip;
};

EventBus eventBus;

void setup() {
    Serial.begin(115200);

    if (!eventBus.init()) {
        Serial.println("Failed to start EventBus");
        return;
    }

    eventBus.subscribe(AppEvent::NetworkGotIP, [](void* payload, void*) {
        auto* info = static_cast<NetworkGotIpPayload*>(payload);
        Serial.printf("[EventBus] Network IP %s\n", info->ip.c_str());
    });
}

void loop() {
    static NetworkGotIpPayload payload{};
    payload.ip = WiFi.localIP().toString();
    eventBus.post(AppEvent::NetworkGotIP, &payload);
    delay(5000);
}
```

Need to block the caller until an event arrives? `waitFor` creates a tiny queue and uses a one-shot subscription to wake the waiting task:

```cpp
auto* payload = static_cast<NetworkGotIpPayload*>(
    eventBus.waitFor(AppEvent::NetworkGotIP, pdMS_TO_TICKS(1000))
);
if (payload) {
    Serial.printf("Received IP %s\n", payload->ip.c_str());
}
```

> ⚠️ The bus only stores the pointer you pass. Make sure the payload stays valid for as long as subscribers need it. Pools or reference-counted buffers work well for fire-and-forget events.

## API reference

- `bool init(const EventBusConfig& cfg = EventBusConfig{})`  
  Creates the subscription mutex, event queue, and worker task. `EventBusConfig` exposes queue length, stack size (in words), task priority, core affinity, and task name.

- `bool post(Id id, void* payload, TickType_t timeout = 0)`  
  Queues an event from any task context. Returns `false` when the queue is full for the whole timeout window.

- `bool postFromISR(Id id, void* payload, BaseType_t* higherPriorityTaskWoken = nullptr)`  
  Safe to call from ISRs. If `higherPriorityTaskWoken` is omitted and the worker should be rescheduled, the bus automatically yields at the end of the ISR (when `portYIELD_FROM_ISR` is available).

- `EventBusSub subscribe(Id id, EventCallbackFn cb, void* userArg = nullptr, bool oneshot = false)`  
  Adds a subscriber. The callback runs on the EventBus worker task. Use `userArg` to pass arbitrary context and `oneshot` to auto-remove the subscription after the first hit. Returns `0` on failure.

- `void unsubscribe(EventBusSub subId)`  
  Marks the subscription inactive; the worker periodically compacts the table.

- `void* waitFor(Id id, TickType_t timeout = portMAX_DELAY)`  
  Blocks the calling task using a temporary queue. Returns the payload pointer or `nullptr` on timeout.

### EventBus configuration & safety controls

`EventBusConfig` now includes extra fields so you can harden the bus when multiple components share it:

```cpp
enum class EventBusOverflowPolicy : uint8_t {
    Block,       // Default — callers block (up to their timeout) when the queue is full
    DropNewest,  // Drop the event that could not be queued
    DropOldest,  // Evict the stalest queued event to make room for the new one
};

struct EventBusConfig {
    uint16_t queueLength = 16;
    UBaseType_t taskPriority = 5;
    uint32_t taskStackWords = 4096;
    BaseType_t coreId = tskNO_AFFINITY;
    const char* taskName = "EventBus";
    uint16_t maxSubscriptions = 0;             // 0 => unlimited subscriptions
    EventBusOverflowPolicy overflowPolicy = EventBusOverflowPolicy::Block;
    uint8_t pressureThresholdPercent = 90;     // Trigger pressure callback once the queue reaches this fill level (0 disables)
    EventBusQueuePressureFn pressureCallback = nullptr;
    void* pressureUserArg = nullptr;
    EventBusDropFn dropCallback = nullptr;     // Called whenever a payload is dropped by the overflow policy
    void* dropUserArg = nullptr;
    EventBusPayloadValidatorFn payloadValidator = nullptr;
    void* payloadValidatorArg = nullptr;
};
```

- `maxSubscriptions` limits how many active subscriptions the bus will host. When the ceiling is reached `subscribe` returns `0` so a rogue task cannot exhaust heap. Leave it at `0` (the default) for the previous unlimited behaviour.
- `overflowPolicy` defines the queue back-pressure behaviour. The default (`Block`) preserves the original semantics. `DropNewest` fails quickly and `DropOldest` discards stale events to keep the newest payload flowing. When an event is dropped the optional `dropCallback` fires with the offending ID/payload (again, in the context of the poster/ISR) so you can log or recycle memory.
- `pressureCallback` provides basic instrumentation: whenever the queue fill percentage meets/exceeds `pressureThresholdPercent`, the callback is invoked with the queued and maximum depths. This runs in the context of the posting task (or ISR), so keep the callback short and ISR-safe if you post from interrupts. You can use it to slow noisy producers or collect metrics.
- `payloadValidator` is invoked before every post (task or ISR) in the caller's context. It lets you enforce ownership rules globally — for example, ensure every payload pointer belongs to a shared pool. Returning `false` rejects the event immediately, so keep the validator ISR-safe if you emit events from interrupts.

> ℹ️ `waitFor` now unconditionally checks that it is **not** running on the EventBus worker. Set `INCLUDE_xTaskGetCurrentTaskHandle=1` in your FreeRTOS config (this is enabled by default on ESP-IDF and Arduino) or the build will fail.

## Examples

- `examples/basic_usage` is a minimal two-task sketch showing posting and `waitFor`.
- `examples/request_response` demonstrates using the bus for request/response interactions between tasks (`waitFor` + worker callbacks).
- `examples/overflow_monitor` showcases payload validation, queue pressure instrumentation, and overflow policies in action.

## Tests

The repository ships with PlatformIO-based Unity tests that exercise overflow policies, subscription limits, payload validation, and shutdown behaviour. To run them on hardware:

```bash
pio test -e esp32dev
```

You can swap `esp32dev` with any ESP32 board definition you have connected.

## License

MIT — see [LICENSE.md](LICENSE.md) for details.

## ESPToolKit

- Check out other libraries: <https://github.com/orgs/ESPToolKit/repositories>
- Hang out on Discord: <https://discord.gg/WG8sSqAy>
- Support the project: <https://ko-fi.com/esptoolkit>
