# ESPEventBus

An asynchronous, FreeRTOS-native event bus for ESP32 projects. Producers post payloads and continue running while the bus fan-outs the work on its own task. Consumers subscribe with callbacks or synchronously block for the next matching event with `waitFor`.

## CI / Release / License
[![CI](https://github.com/ESPToolKit/esp-eventbus/actions/workflows/ci.yml/badge.svg)](https://github.com/ESPToolKit/esp-eventbus/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ESPToolKit/esp-eventbus?sort=semver)](https://github.com/ESPToolKit/esp-eventbus/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Features
- Tiny API, single header include (`ESPEventBus.h`).
- Dedicated worker task managed with native FreeRTOS APIs with configurable queue depth, stack size, priority, and core affinity.
- Thread-safe and ISR-safe posting (ISRs can queue events without waking the worker unless needed).
- Unlimited subscriptions per event with optional user data and one-shot semantics.
- `std::function` callback support so you can bind private member methods or use capturing lambdas.
- Per-task/per-event `waitFor` helper with persistent waiter queues to avoid per-call heap churn in tight loops.
- Queue overflow policies, pressure callbacks, and payload validation hooks for defensive firmware.
- Optional `usePSRAMBuffers` toggle to route subscription/fan-out buffers and (when static FreeRTOS allocation is enabled) queue/mutex storage through `ESPBufferManager` with safe fallback.

## Examples
Define strongly typed event IDs (the bus only needs integral IDs internally):

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

Wire up subscriptions and post from any task:

```cpp
#include <ESPEventBus.h>
#include "app_events.h"

struct NetworkGotIpPayload {
    String ip;
};

ESPEventBus eventBus;

void onNetworkGotIp(void* payload, void*) {
    auto* info = static_cast<NetworkGotIpPayload*>(payload);
    Serial.printf("[ESPEventBus] Network IP %s\n", info->ip.c_str());
}

void setup() {
    eventBus.init();

    eventBus.subscribe(AppEvent::NetworkGotIP, onNetworkGotIp);
}

void loop() {
    static NetworkGotIpPayload payload{};
    payload.ip = WiFi.localIP().toString();
    eventBus.post(AppEvent::NetworkGotIP, &payload);
    delay(5000);
}

void stopEventBusBeforeSleepOrRestart() {
    if (eventBus.isInitialized()) {
        eventBus.deinit();
    }
}
```

Bind private class methods with `std::bind` when needed:

```cpp
eventBus.subscribe(AppEvent::NetworkGotIP,
    std::bind(&HostedFirmwareUpdater::onNetEvent,
    this,
    std::placeholders::_1,
    std::placeholders::_2)
);
```

Need to suspend the caller until a payload arrives? `waitFor` blocks on a reusable waiter queue owned by the calling task/event pair:

```cpp
auto* payload = static_cast<NetworkGotIpPayload*>(eventBus.waitFor(AppEvent::NetworkGotIP, pdMS_TO_TICKS(1000)));
if (payload) {
    Serial.printf("Received IP %s\n", payload->ip.c_str());
}
```

Explore the sketches under `examples/`:
- `examples/basic_usage` – minimal two-task sketch with posting and `waitFor`.
- `examples/request_response` – request/response pattern between tasks.
- `examples/overflow_monitor` – queue pressure instrumentation and overflow policies.

## Gotchas
- The bus only stores the pointer you supply; keep payloads alive for as long as subscribers need them or use pools/ref-counted buffers.
- `waitFor` refuses to run on the ESPEventBus worker task. Enable `INCLUDE_xTaskGetCurrentTaskHandle=1` (set by default on ESP-IDF/Arduino) so the guard can detect misuse.
- `waitFor` reuses one waiter queue per `(task, event)` pair. Multiple tasks can wait on the same event concurrently, but avoid overlapping waits for the same `(task, event)` pair.
- `postFromISR` behaves like `post` but still runs callbacks on the worker. Long callbacks block the worker and delay other subscribers.
- Overflow policies that drop events fire user callbacks in the posting context—keep those callbacks short and ISR-safe where applicable.

## API Reference
- `bool init(const EventBusConfig& cfg = EventBusConfig{})` – creates the subscription mutex, queue, and worker task with native FreeRTOS task creation.
- `void deinit()` – idempotent teardown that stops the worker task, releases queue/mutex resources, and clears subscriptions.
- `bool isInitialized() const` – reports whether runtime resources are currently active.
- `bool post(Id id, void* payload, TickType_t timeout = 0)` / `bool postFromISR(...)` – queue an event from tasks or interrupts.
- `EventBusSub subscribe(Id id, EventCallbackFn cb, void* userArg = nullptr, bool oneshot = false)` – register C-style callbacks; returns `0` on failure.
- `EventBusSub subscribe(Id id, EventCallback cb, void* userArg = nullptr, bool oneshot = false)` – register `std::function` callbacks (bind/captures).
- `void unsubscribe(EventBusSub subId)` – deactivate one subscription.
- `void* waitFor(Id id, TickType_t timeout = portMAX_DELAY)` – block the caller on a reusable task/event waiter queue and return the payload pointer or `nullptr` on timeout.

`EventBusConfig` exposes guardrails so multiple components can safely share one bus:

```cpp
enum class EventBusOverflowPolicy : uint8_t {
    Block,
    DropNewest,
    DropOldest,
};

struct EventBusConfig {
    uint16_t queueLength = 16;
    UBaseType_t priority = 5;
    uint32_t stackSize = 4096 * sizeof(StackType_t);
    BaseType_t coreId = tskNO_AFFINITY;
    const char* taskName = "ESPEventBus";
    uint16_t maxSubscriptions = 0;
    bool usePSRAMBuffers = false;
    EventBusOverflowPolicy overflowPolicy = EventBusOverflowPolicy::Block;
    uint8_t pressureThresholdPercent = 90;
    EventBusQueuePressureFn pressureCallback = nullptr;
    EventBusDropFn dropCallback = nullptr;
    EventBusPayloadValidatorFn payloadValidator = nullptr;
};

Stack sizes are expressed in bytes.

When `usePSRAMBuffers` is enabled, ESPEventBus routes its dynamic subscription/fan-out containers through `ESPBufferManager`. On builds where `configSUPPORT_STATIC_ALLOCATION == 1`, it also prefers PSRAM-backed static storage for the event queue storage/control block and synchronization objects. The worker task is created through standard FreeRTOS task creation (`xTaskCreatePinnedToCore(...)`) for broad ESP32 compatibility. If PSRAM (or static allocation support) is unavailable, ESPEventBus falls back automatically to normal FreeRTOS heap-backed creation paths.
```

Combine `pressureCallback` and `dropCallback` to monitor noisy publishers, and wire `payloadValidator` to enforce shared ownership rules before any payload reaches the queue.

## Restrictions
- Built and tested on ESP32 (Arduino-ESP32 and ESP-IDF) with FreeRTOS available; other MCUs/frameworks are unsupported.
- Requires C++17 support and a FreeRTOS configuration that enables `xTaskGetCurrentTaskHandle` (Arduino + ESP-IDF do this by default).
- Single ESPEventBus instance manages its own worker task; if you construct multiple buses they each allocate their own queue/task resources.
- Call `deinit()` before deep sleep, component shutdown, or application restart to release worker/task resources explicitly.

## Tests
Unity tests run under PlatformIO: plug in an ESP32 dev board and execute `pio test -e esp32dev` from the repo root. The suite covers overflow policies, subscription caps, payload validation, and graceful shutdown.

## License
MIT — see [LICENSE.md](LICENSE.md).

## ESPToolKit
- Check out other libraries: <https://github.com/orgs/ESPToolKit/repositories>
- Hang out on Discord: <https://discord.gg/WG8sSqAy>
- Support the project: <https://ko-fi.com/esptoolkit>
- Visit the website: <https://www.esptoolkit.hu/>
