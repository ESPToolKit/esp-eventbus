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

    EventBusConfig sanitized = config;
    if (sanitized.queueLength == 0 || sanitized.taskStackWords == 0) {
        return false;
    }

    if (sanitized.pressureThresholdPercent > 100) {
        sanitized.pressureThresholdPercent = 100;
    }

    config_ = sanitized;
    stopEventPending_ = false;

    subMutex_ = xSemaphoreCreateMutex();
    if (!subMutex_) {
        return false;
    }

    queue_ = xQueueCreate(config_.queueLength, sizeof(QueuedEvent));
    if (!queue_) {
        vSemaphoreDelete(subMutex_);
        subMutex_ = nullptr;
        return false;
    }

    running_ = true;
    const char* taskName = (config_.taskName && config_.taskName[0] != '\0') ? config_.taskName : "EventBus";
    BaseType_t res = xTaskCreatePinnedToCore(
        &EventBus::taskEntry,
        taskName,
        config_.taskStackWords,
        this,
        config_.taskPriority,
        &task_,
        config_.coreId);

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
    stopEventPending_ = false;
    config_ = EventBusConfig{};
}

bool EventBus::post(EventBusId id, void* payload, TickType_t timeout) {
    if (!queue_) {
        return false;
    }

    if (!validatePayload(id, payload)) {
        return false;
    }

    QueuedEvent ev{ id, payload, false };
    return enqueueFromTask(ev, timeout);
}

bool EventBus::postFromISR(EventBusId id, void* payload, BaseType_t* higherPriorityTaskWoken) {
    if (!queue_) {
        return false;
    }

    if (!validatePayload(id, payload)) {
        return false;
    }

    QueuedEvent ev{ id, payload, false };
    return enqueueFromISR(ev, higherPriorityTaskWoken);
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

    if (config_.maxSubscriptions != 0) {
        size_t activeCount = std::count_if(subs_.begin(), subs_.end(), [](const Subscription& sub) { return sub.active; });
        if (activeCount >= config_.maxSubscriptions) {
            xSemaphoreGive(subMutex_);
            return 0;
        }
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

    if (task_ && currentTaskHandle() == task_) {
        return nullptr;
    }

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
        running_ = false;
        stopEventPending_ = false;
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
            stopEventPending_ = false;
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
        stopEventPending_ = false;
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
        stopEventPending_ = false;
        return;
    }

    if (queue_ && !stopEventPending_) {
        QueuedEvent stopEvent{};
        stopEvent.stop = true;
        while (xQueueSend(queue_, &stopEvent, pdMS_TO_TICKS(10)) != pdPASS) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        stopEventPending_ = true;
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

bool EventBus::enqueueFromTask(const QueuedEvent& ev, TickType_t timeout) {
    TickType_t waitTicks = timeout;
    if (config_.overflowPolicy != EventBusOverflowPolicy::Block) {
        waitTicks = 0;
    }

    if (xQueueSend(queue_, &ev, waitTicks) == pdTRUE) {
        emitPressureMetricFromTask();
        return true;
    }

    if (config_.overflowPolicy == EventBusOverflowPolicy::Block) {
        notifyDrop(ev.eventId, ev.payload);
        return false;
    }

    return handleOverflowFromTask(ev);
}

bool EventBus::enqueueFromISR(const QueuedEvent& ev, BaseType_t* higherPriorityTaskWoken) {
    BaseType_t localWoken = pdFALSE;
    BaseType_t res = xQueueSendFromISR(queue_, &ev, &localWoken);
    if (res == pdPASS) {
        propagateYieldFromISR(localWoken, higherPriorityTaskWoken);
        return true;
    }

    if (config_.overflowPolicy == EventBusOverflowPolicy::Block) {
        notifyDrop(ev.eventId, ev.payload);
        propagateYieldFromISR(localWoken, higherPriorityTaskWoken);
        return false;
    }

    bool ok = handleOverflowFromISR(ev, &localWoken);
    propagateYieldFromISR(localWoken, higherPriorityTaskWoken);
    return ok;
}

bool EventBus::handleOverflowFromTask(const QueuedEvent& ev) {
    switch (config_.overflowPolicy) {
        case EventBusOverflowPolicy::DropNewest:
            notifyDrop(ev.eventId, ev.payload);
            return false;
        case EventBusOverflowPolicy::DropOldest: {
            QueuedEvent dropped{};
            if (xQueueReceive(queue_, &dropped, 0) == pdTRUE) {
                notifyDrop(dropped.eventId, dropped.payload);
                if (xQueueSend(queue_, &ev, 0) == pdTRUE) {
                    emitPressureMetricFromTask();
                    return true;
                }
            }
            notifyDrop(ev.eventId, ev.payload);
            return false;
        }
        case EventBusOverflowPolicy::Block:
        default:
            notifyDrop(ev.eventId, ev.payload);
            return false;
    }
}

bool EventBus::handleOverflowFromISR(const QueuedEvent& ev, BaseType_t* localWokenAggregate) {
    switch (config_.overflowPolicy) {
        case EventBusOverflowPolicy::DropNewest:
            notifyDrop(ev.eventId, ev.payload);
            return false;
        case EventBusOverflowPolicy::DropOldest: {
            QueuedEvent dropped{};
            BaseType_t localWoken = pdFALSE;
            if (xQueueReceiveFromISR(queue_, &dropped, &localWoken) == pdTRUE) {
                if (localWokenAggregate) {
                    if (localWoken == pdTRUE) {
                        *localWokenAggregate = pdTRUE;
                    }
                }
                notifyDrop(dropped.eventId, dropped.payload);
                BaseType_t sendWoken = pdFALSE;
                if (xQueueSendFromISR(queue_, &ev, &sendWoken) == pdPASS) {
                    if (sendWoken == pdTRUE) {
                        localWoken = pdTRUE;
                    }
                    if (localWokenAggregate) {
                        if (localWoken == pdTRUE || sendWoken == pdTRUE) {
                            *localWokenAggregate = pdTRUE;
                        }
                    }
                    return true;
                }
            }
            notifyDrop(ev.eventId, ev.payload);
            return false;
        }
        case EventBusOverflowPolicy::Block:
        default:
            notifyDrop(ev.eventId, ev.payload);
            return false;
    }
}

void EventBus::emitPressureMetricFromTask() {
    if (!config_.pressureCallback || !queue_ || config_.queueLength == 0 || config_.pressureThresholdPercent == 0) {
        return;
    }

    UBaseType_t queued = uxQueueMessagesWaiting(queue_);
    uint32_t percent = (queued * 100U) / config_.queueLength;
    if (percent >= config_.pressureThresholdPercent) {
        config_.pressureCallback(queued, config_.queueLength, config_.pressureUserArg);
    }
}

void EventBus::notifyDrop(EventBusId id, void* payload) {
    if (config_.dropCallback) {
        config_.dropCallback(id, payload, config_.dropUserArg);
    }
}

bool EventBus::validatePayload(EventBusId id, void* payload) const {
    if (!config_.payloadValidator) {
        return true;
    }
    return config_.payloadValidator(id, payload, config_.payloadValidatorArg);
}

void EventBus::propagateYieldFromISR(BaseType_t localWoken, BaseType_t* higherPriorityTaskWoken) {
    if (higherPriorityTaskWoken) {
        if (localWoken == pdTRUE) {
            *higherPriorityTaskWoken = pdTRUE;
        }
    } else if (localWoken == pdTRUE) {
#if defined(portYIELD_FROM_ISR)
        portYIELD_FROM_ISR();
#endif
    }
}

#if defined(INCLUDE_xTaskGetCurrentTaskHandle) && (INCLUDE_xTaskGetCurrentTaskHandle == 1)
TaskHandle_t EventBus::currentTaskHandle() {
    return xTaskGetCurrentTaskHandle();
}
#else
extern "C" void* volatile pxCurrentTCB;
TaskHandle_t EventBus::currentTaskHandle() {
    return reinterpret_cast<TaskHandle_t>(pxCurrentTCB);
}
#endif
