#include "eventbus.h"

#include <algorithm>

EventBus::EventBus() = default;

EventBus::~EventBus() {
    deinit();
}

bool EventBus::init(const EventBusConfig& config) {
    if (queue_ || task_ || subMutex_) {
        deinit();
    }

    if (config.queueLength == 0 || config.taskStackWords == 0) {
        return false;
    }

    subMutex_ = xSemaphoreCreateMutex();
    if (!subMutex_) {
        return false;
    }

    queue_ = xQueueCreate(config.queueLength, sizeof(QueuedEvent));
    if (!queue_) {
        vSemaphoreDelete(subMutex_);
        subMutex_ = nullptr;
        return false;
    }

    running_ = true;
    const char* taskName = (config.taskName && config.taskName[0] != '\0') ? config.taskName : "EventBus";
    BaseType_t res = xTaskCreatePinnedToCore(
        &EventBus::taskEntry,
        taskName,
        config.taskStackWords,
        this,
        config.taskPriority,
        &task_,
        config.coreId);

    if (res != pdPASS) {
        running_ = false;
        vQueueDelete(queue_);
        queue_ = nullptr;
        vSemaphoreDelete(subMutex_);
        subMutex_ = nullptr;
        return false;
    }

    return true;
}

void EventBus::deinit() {
    stopTask();

    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }

    if (subMutex_) {
        vSemaphoreDelete(subMutex_);
        subMutex_ = nullptr;
    }

    subs_.clear();
    nextSubId_ = 0;
}

bool EventBus::post(EventBusId id, void* payload, TickType_t timeout) {
    if (!queue_) {
        return false;
    }

    QueuedEvent ev{ id, payload, false };
    return xQueueSend(queue_, &ev, timeout) == pdTRUE;
}

bool EventBus::postFromISR(EventBusId id, void* payload, BaseType_t* higherPriorityTaskWoken) {
    if (!queue_) {
        return false;
    }

    QueuedEvent ev{ id, payload, false };
    BaseType_t localWoken = pdFALSE;
    BaseType_t res = xQueueSendFromISR(queue_, &ev, &localWoken);

    if (higherPriorityTaskWoken) {
        *higherPriorityTaskWoken = localWoken;
    } else if (localWoken == pdTRUE) {
#if defined(portYIELD_FROM_ISR)
        portYIELD_FROM_ISR();
#endif
    }

    return res == pdTRUE;
}

EventBusSub EventBus::subscribe(EventBusId id,
                                EventCallbackFn cb,
                                void* userArg,
                                bool oneshot) {
    if (!cb || !subMutex_) {
        return 0;
    }

    if (xSemaphoreTake(subMutex_, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    EventBusSub subId = ++nextSubId_;
    subs_.push_back(Subscription{ subId, id, cb, userArg, oneshot, true });
    xSemaphoreGive(subMutex_);
    return subId;
}

void EventBus::unsubscribe(EventBusSub subId) {
    if (!subId || !subMutex_) {
        return;
    }

    if (xSemaphoreTake(subMutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }

    for (auto& sub : subs_) {
        if (sub.subId == subId) {
            sub.active = false;
            break;
        }
    }

    compactSubscriptionsLocked();
    xSemaphoreGive(subMutex_);
}

void* EventBus::waitFor(EventBusId id, TickType_t timeout) {
    if (!queue_) {
        return nullptr;
    }

#if defined(INCLUDE_xTaskGetCurrentTaskHandle) && (INCLUDE_xTaskGetCurrentTaskHandle == 1)
    if (task_ && xTaskGetCurrentTaskHandle() == task_) {
        return nullptr;
    }
#endif

    QueueHandle_t responseQueue = xQueueCreate(1, sizeof(void*));
    if (!responseQueue) {
        return nullptr;
    }

    EventBusSub sid = subscribe(id, &EventBus::waiterCallback, responseQueue, true);
    if (!sid) {
        vQueueDelete(responseQueue);
        return nullptr;
    }

    void* payload = nullptr;
    BaseType_t res = xQueueReceive(responseQueue, &payload, timeout);

    unsubscribe(sid);
    vQueueDelete(responseQueue);

    if (res != pdTRUE) {
        return nullptr;
    }
    return payload;
}

void EventBus::taskEntry(void* arg) {
    auto* instance = static_cast<EventBus*>(arg);
    instance->taskLoop();
}

void EventBus::taskLoop() {
    if (!queue_) {
        task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    std::vector<Subscription> snapshot;
    snapshot.reserve(4);

    QueuedEvent ev;

    while (running_) {
        if (xQueueReceive(queue_, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (ev.stop) {
            break;
        }

        snapshot.clear();

        if (xSemaphoreTake(subMutex_, portMAX_DELAY) == pdTRUE) {
            bool needsCompact = false;
            for (auto& sub : subs_) {
                if (!sub.active) {
                    needsCompact = true;
                    continue;
                }

                if (sub.eventId == ev.eventId) {
                    snapshot.push_back(sub);
                    if (sub.oneshot) {
                        sub.active = false;
                        needsCompact = true;
                    }
                }
            }

            if (needsCompact) {
                compactSubscriptionsLocked();
            }

            xSemaphoreGive(subMutex_);
        }

        for (auto& sub : snapshot) {
            if (sub.cb) {
                sub.cb(ev.payload, sub.userArg);
            }
        }
    }

    running_ = false;
    task_ = nullptr;
    vTaskDelete(nullptr);
}

void EventBus::stopTask() {
    if (!task_) {
        running_ = false;
        return;
    }

    bool schedulerRunning = true;
#if defined(INCLUDE_xTaskGetSchedulerState) && (INCLUDE_xTaskGetSchedulerState == 1)
    schedulerRunning = (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
#endif

    if (!schedulerRunning) {
        vTaskDelete(task_);
        task_ = nullptr;
        running_ = false;
        return;
    }

    running_ = false;
    if (queue_) {
        QueuedEvent stopEvent{};
        stopEvent.stop = true;
        xQueueSend(queue_, &stopEvent, portMAX_DELAY);
    }

    while (task_) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void EventBus::compactSubscriptionsLocked() {
    subs_.erase(
        std::remove_if(subs_.begin(), subs_.end(),
                       [](const Subscription& sub) { return !sub.active; }),
        subs_.end());
}

void EventBus::waiterCallback(void* payload, void* userArg) {
    QueueHandle_t queue = reinterpret_cast<QueueHandle_t>(userArg);
    if (!queue) {
        return;
    }
    (void)xQueueSend(queue, &payload, 0);
}
