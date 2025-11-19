#pragma once

#include <cstdint>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Internal identifier used to track events on the bus.
using EventBusId = uint16_t;
using EventBusSub = uint32_t;

using EventCallbackFn = void (*)(void* payload, void* userArg);

struct EventBusConfig {
    uint16_t queueLength = 16;
    UBaseType_t taskPriority = 5;
    uint32_t taskStackWords = 4096;
    BaseType_t coreId = tskNO_AFFINITY;
    const char* taskName = "EventBus";
};

class EventBus {
  public:
    EventBus();
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    bool init(const EventBusConfig& config = EventBusConfig{});
    void deinit();

    bool post(EventBusId id, void* payload, TickType_t timeout = 0);

    template <typename Id>
    bool post(Id id, void* payload, TickType_t timeout = 0) {
        return post(static_cast<EventBusId>(id), payload, timeout);
    }

    bool postFromISR(EventBusId id, void* payload, BaseType_t* higherPriorityTaskWoken = nullptr);

    template <typename Id>
    bool postFromISR(Id id, void* payload, BaseType_t* higherPriorityTaskWoken = nullptr) {
        return postFromISR(static_cast<EventBusId>(id), payload, higherPriorityTaskWoken);
    }

    EventBusSub subscribe(EventBusId id,
                          EventCallbackFn cb,
                          void* userArg = nullptr,
                          bool oneshot = false);

    template <typename Id>
    EventBusSub subscribe(Id id,
                          EventCallbackFn cb,
                          void* userArg = nullptr,
                          bool oneshot = false) {
        return subscribe(static_cast<EventBusId>(id), cb, userArg, oneshot);
    }

    void unsubscribe(EventBusSub subId);

    void* waitFor(EventBusId id, TickType_t timeout = portMAX_DELAY);

    template <typename Id>
    void* waitFor(Id id, TickType_t timeout = portMAX_DELAY) {
        return waitFor(static_cast<EventBusId>(id), timeout);
    }

  private:
    struct Subscription {
        EventBusSub subId = 0;
        EventBusId eventId = 0;
        EventCallbackFn cb = nullptr;
        void* userArg = nullptr;
        bool oneshot = false;
        bool active = false;
    };

    struct QueuedEvent {
        EventBusId eventId = 0;
        void* payload = nullptr;
        bool stop = false;
    };

    static void taskEntry(void* arg);
    void taskLoop();
    void stopTask();
    void compactSubscriptionsLocked();
    static void waiterCallback(void* payload, void* userArg);

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t subMutex_ = nullptr;
    std::vector<Subscription> subs_;
    EventBusSub nextSubId_ = 0;
    bool running_ = false;
};
