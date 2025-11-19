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

## Example

The `examples/basic_usage` sketch shows a minimal setup with two FreeRTOS tasks: one posting network events and one waiting for them via `waitFor`.

## License

MIT — see [LICENSE.md](LICENSE.md) for details.

## ESPToolKit

- Check out other libraries: <https://github.com/orgs/ESPToolKit/repositories>
- Hang out on Discord: <https://discord.gg/WG8sSqAy>
- Support the project: <https://ko-fi.com/esptoolkit>
