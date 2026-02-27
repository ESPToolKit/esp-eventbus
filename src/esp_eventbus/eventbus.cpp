#include "eventbus.h"

#include <algorithm>
#include <new>

ESPEventBus::ESPEventBus() = default;

ESPEventBus::~ESPEventBus() {
    deinit();
}

bool ESPEventBus::init(const EventBusConfig& config) {
    if (queue_ || task_ || subMutex_) {
        deinit();
    }

    EventBusConfig sanitized = config;
    if (sanitized.queueLength == 0 || sanitized.stackSize == 0) {
        return false;
    }

    if (sanitized.pressureThresholdPercent > 100) {
        sanitized.pressureThresholdPercent = 100;
    }

    config_ = sanitized;
    stopEventPending_ = false;
    resetKernelStorage();
    resetSubscriptions(config_.usePSRAMBuffers);
    resetWaiters(config_.usePSRAMBuffers);

    if (!createKernelMutex()) {
        return false;
    }

    if (!createKernelQueue()) {
        vSemaphoreDelete(subMutex_);
        subMutex_ = nullptr;
        resetKernelStorage();
        return false;
    }

    running_ = true;
    const char* taskName = (config_.taskName && config_.taskName[0] != '\0') ? config_.taskName : "ESPEventBus";
    if (!createWorkerTask(taskName)) {
        running_ = false;
        vQueueDelete(queue_);
        queue_ = nullptr;
        vSemaphoreDelete(subMutex_);
        subMutex_ = nullptr;
        resetKernelStorage();
        return false;
    }

    return true;
}

void ESPEventBus::deinit() {
    stopTask();

    if (subMutex_ && xSemaphoreTake(subMutex_, portMAX_DELAY) == pdTRUE) {
        clearWaiters();
        xSemaphoreGive(subMutex_);
    } else {
        clearWaiters();
    }

    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }

    if (subMutex_) {
        vSemaphoreDelete(subMutex_);
        subMutex_ = nullptr;
    }

    resetKernelStorage();
    resetSubscriptions(false);
    resetWaiters(false);
    nextSubId_ = 0;
    stopEventPending_ = false;
    config_ = EventBusConfig{};
    task_ = nullptr;
}

bool ESPEventBus::isInitialized() const {
    return queue_ != nullptr && subMutex_ != nullptr && task_ != nullptr && running_;
}

bool ESPEventBus::post(EventBusId id, void* payload, TickType_t timeout) {
    if (!queue_) {
        return false;
    }

    if (!validatePayload(id, payload)) {
        return false;
    }

    QueuedEvent ev{ id, payload, false };
    return enqueueFromTask(ev, timeout);
}

bool ESPEventBus::postFromISR(EventBusId id, void* payload, BaseType_t* higherPriorityTaskWoken) {
    if (!queue_) {
        return false;
    }

    if (!validatePayload(id, payload)) {
        return false;
    }

    QueuedEvent ev{ id, payload, false };
    return enqueueFromISR(ev, higherPriorityTaskWoken);
}

EventBusSub ESPEventBus::subscribe(EventBusId id,
                                   EventCallbackFn cb,
                                   void* userArg,
                                   bool oneshot) {
    if (!cb) {
        return 0;
    }

    return subscribe(id, EventCallback(cb), userArg, oneshot);
}

EventBusSub ESPEventBus::subscribe(EventBusId id,
                                   EventCallback cb,
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
    subs_.push_back(Subscription{ subId, id, std::move(cb), userArg, oneshot, true });
    xSemaphoreGive(subMutex_);
    return subId;
}

void ESPEventBus::unsubscribe(EventBusSub subId) {
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

void* ESPEventBus::waitFor(EventBusId id, TickType_t timeout) {
    if (!queue_ || !subMutex_) {
        return nullptr;
    }

    const TaskHandle_t callerTask = currentTaskHandle();
    if (!callerTask || (task_ && callerTask == task_)) {
        return nullptr;
    }

    QueueHandle_t responseQueue = nullptr;
    if (xSemaphoreTake(subMutex_, portMAX_DELAY) != pdTRUE) {
        return nullptr;
    }

    WaiterContext* waiter = findWaiterLocked(callerTask, id);
    if (!waiter) {
        waiters_.push_back(WaiterContext{ callerTask, id, nullptr, nullptr, nullptr });
        waiter = &waiters_.back();
        if (!createWaiterQueue(*waiter)) {
            waiters_.pop_back();
            xSemaphoreGive(subMutex_);
            return nullptr;
        }
    }

    responseQueue = waiter->queue;
    if (!responseQueue) {
        xSemaphoreGive(subMutex_);
        return nullptr;
    }

    (void)xQueueReset(responseQueue);
    xSemaphoreGive(subMutex_);

    void* payload = nullptr;
    BaseType_t res = xQueueReceive(responseQueue, &payload, timeout);

    if (res != pdTRUE) {
        return nullptr;
    }
    return payload;
}

void ESPEventBus::taskEntry(void* arg) {
    auto* instance = static_cast<ESPEventBus*>(arg);
    instance->taskLoop();
}

void ESPEventBus::taskLoop() {
    if (!queue_) {
        running_ = false;
        stopEventPending_ = false;
        task_ = nullptr;
        return;
    }
    task_ = currentTaskHandle();

    {
        // Scope dynamic containers so they release memory before the task self-deletes.
        EventBusVector<Subscription> snapshot{ EventBusAllocator<Subscription>(config_.usePSRAMBuffers) };
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

                for (auto& waiter : waiters_) {
                    if (waiter.eventId != ev.eventId || !waiter.queue) {
                        continue;
                    }
                    (void)xQueueSend(waiter.queue, &ev.payload, 0);
                }

                xSemaphoreGive(subMutex_);
            }

            for (auto& sub : snapshot) {
                if (sub.cb) {
                    sub.cb(ev.payload, sub.userArg);
                }
            }
        }
    }

    running_ = false;
    task_ = nullptr;
    vTaskDelete(nullptr);
}

void ESPEventBus::stopTask() {
    if (!task_) {
        running_ = false;
        stopEventPending_ = false;
        task_ = nullptr;
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

    TickType_t start = xTaskGetTickCount();
    while (task_ && (xTaskGetTickCount() - start) <= pdMS_TO_TICKS(3000)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (task_) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    task_ = nullptr;
    running_ = false;
    stopEventPending_ = false;
}

void ESPEventBus::compactSubscriptionsLocked() {
    subs_.erase(
        std::remove_if(subs_.begin(), subs_.end(),
                       [](const Subscription& sub) { return !sub.active; }),
        subs_.end());
}

bool ESPEventBus::enqueueFromTask(const QueuedEvent& ev, TickType_t timeout) {
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

bool ESPEventBus::enqueueFromISR(const QueuedEvent& ev, BaseType_t* higherPriorityTaskWoken) {
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

bool ESPEventBus::handleOverflowFromTask(const QueuedEvent& ev) {
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

bool ESPEventBus::handleOverflowFromISR(const QueuedEvent& ev, BaseType_t* localWokenAggregate) {
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

void ESPEventBus::emitPressureMetricFromTask() {
    if (!config_.pressureCallback || !queue_ || config_.queueLength == 0 || config_.pressureThresholdPercent == 0) {
        return;
    }

    UBaseType_t queued = uxQueueMessagesWaiting(queue_);
    uint32_t percent = (queued * 100U) / config_.queueLength;
    if (percent >= config_.pressureThresholdPercent) {
        config_.pressureCallback(queued, config_.queueLength, config_.pressureUserArg);
    }
}

void ESPEventBus::notifyDrop(EventBusId id, void* payload) {
    if (config_.dropCallback) {
        config_.dropCallback(id, payload, config_.dropUserArg);
    }
}

bool ESPEventBus::validatePayload(EventBusId id, void* payload) const {
    if (!config_.payloadValidator) {
        return true;
    }
    return config_.payloadValidator(id, payload, config_.payloadValidatorArg);
}

void ESPEventBus::propagateYieldFromISR(BaseType_t localWoken, BaseType_t* higherPriorityTaskWoken) {
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
TaskHandle_t ESPEventBus::currentTaskHandle() {
    return xTaskGetCurrentTaskHandle();
}
#else
extern "C" void* volatile pxCurrentTCB;
TaskHandle_t ESPEventBus::currentTaskHandle() {
    return reinterpret_cast<TaskHandle_t>(pxCurrentTCB);
}
#endif

bool ESPEventBus::createKernelMutex() {
#if defined(configSUPPORT_STATIC_ALLOCATION) && (configSUPPORT_STATIC_ALLOCATION == 1)
    if (config_.usePSRAMBuffers) {
        mutexStorage_ = allocateKernelStorage(sizeof(StaticSemaphore_t), true);
        if (mutexStorage_) {
            subMutex_ = xSemaphoreCreateMutexStatic(static_cast<StaticSemaphore_t*>(mutexStorage_));
            if (subMutex_) {
                return true;
            }
            freeKernelStorage(mutexStorage_);
            mutexStorage_ = nullptr;
        }
    }
#endif
    subMutex_ = xSemaphoreCreateMutex();
    return subMutex_ != nullptr;
}

bool ESPEventBus::createKernelQueue() {
#if defined(configSUPPORT_STATIC_ALLOCATION) && (configSUPPORT_STATIC_ALLOCATION == 1)
    if (config_.usePSRAMBuffers) {
        queueStorage_ = allocateKernelStorage(static_cast<size_t>(config_.queueLength) * sizeof(QueuedEvent), true);
        queueControlStorage_ = allocateKernelStorage(sizeof(StaticQueue_t), true);
        if (queueStorage_ && queueControlStorage_) {
            queue_ = xQueueCreateStatic(
                config_.queueLength,
                sizeof(QueuedEvent),
                static_cast<uint8_t*>(queueStorage_),
                static_cast<StaticQueue_t*>(queueControlStorage_));
            if (queue_) {
                return true;
            }
        }
        freeKernelStorage(queueStorage_);
        freeKernelStorage(queueControlStorage_);
        queueStorage_ = nullptr;
        queueControlStorage_ = nullptr;
    }
#endif
    queue_ = xQueueCreate(config_.queueLength, sizeof(QueuedEvent));
    return queue_ != nullptr;
}

bool ESPEventBus::createWorkerTask(const char* taskName) {
    const char* resolvedTaskName = (taskName && taskName[0] != '\0') ? taskName : "ESPEventBus";
    task_ = nullptr;
    const BaseType_t created = xTaskCreatePinnedToCore(
        &ESPEventBus::taskEntry, resolvedTaskName, config_.stackSize, this, config_.priority, &task_, config_.coreId);
    return created == pdPASS && task_ != nullptr;
}

void ESPEventBus::resetSubscriptions(bool usePSRAMBuffers) {
    using SubscriptionVector = EventBusVector<Subscription>;
    subs_.~SubscriptionVector();
    new (&subs_) SubscriptionVector{ EventBusAllocator<Subscription>(usePSRAMBuffers) };
}

void ESPEventBus::clearWaiters() {
    for (auto& waiter : waiters_) {
        if (waiter.queue) {
            void* nullPayload = nullptr;
            (void)xQueueReset(waiter.queue);
            (void)xQueueSend(waiter.queue, &nullPayload, 0);
            vQueueDelete(waiter.queue);
            waiter.queue = nullptr;
        }
        freeKernelStorage(waiter.queueStorage);
        freeKernelStorage(waiter.queueControlStorage);
        waiter.queueStorage = nullptr;
        waiter.queueControlStorage = nullptr;
    }
    waiters_.clear();
}

void ESPEventBus::resetWaiters(bool usePSRAMBuffers) {
    using WaiterVector = EventBusVector<WaiterContext>;
    waiters_.~WaiterVector();
    new (&waiters_) WaiterVector{ EventBusAllocator<WaiterContext>(usePSRAMBuffers) };
}

ESPEventBus::WaiterContext* ESPEventBus::findWaiterLocked(TaskHandle_t ownerTask, EventBusId eventId) {
    for (auto& waiter : waiters_) {
        if (waiter.ownerTask == ownerTask && waiter.eventId == eventId) {
            return &waiter;
        }
    }
    return nullptr;
}

bool ESPEventBus::createWaiterQueue(WaiterContext& waiter) {
#if defined(configSUPPORT_STATIC_ALLOCATION) && (configSUPPORT_STATIC_ALLOCATION == 1)
    if (config_.usePSRAMBuffers) {
        waiter.queueStorage = allocateKernelStorage(sizeof(void*), true);
        waiter.queueControlStorage = allocateKernelStorage(sizeof(StaticQueue_t), true);
        if (waiter.queueStorage && waiter.queueControlStorage) {
            waiter.queue = xQueueCreateStatic(
                1,
                sizeof(void*),
                static_cast<uint8_t*>(waiter.queueStorage),
                static_cast<StaticQueue_t*>(waiter.queueControlStorage));
        }
        if (waiter.queue) {
            return true;
        }
        freeKernelStorage(waiter.queueStorage);
        freeKernelStorage(waiter.queueControlStorage);
        waiter.queueStorage = nullptr;
        waiter.queueControlStorage = nullptr;
    }
#endif
    waiter.queue = xQueueCreate(1, sizeof(void*));
    return waiter.queue != nullptr;
}

void ESPEventBus::resetKernelStorage() {
    freeKernelStorage(mutexStorage_);
    freeKernelStorage(queueStorage_);
    freeKernelStorage(queueControlStorage_);
    freeKernelStorage(taskStackStorage_);
    freeKernelStorage(taskControlStorage_);
    mutexStorage_ = nullptr;
    queueStorage_ = nullptr;
    queueControlStorage_ = nullptr;
    taskStackStorage_ = nullptr;
    taskControlStorage_ = nullptr;
}

size_t ESPEventBus::taskStackWords(uint32_t stackSizeBytes) {
    const size_t bytes = static_cast<size_t>(stackSizeBytes);
    const size_t wordSize = sizeof(StackType_t);
    return (bytes + (wordSize - 1U)) / wordSize;
}

void* ESPEventBus::allocateKernelStorage(size_t bytes, bool usePSRAMBuffers) {
    return eventbus_allocator_detail::allocate(bytes, usePSRAMBuffers);
}

void ESPEventBus::freeKernelStorage(void* ptr) {
    if (ptr) {
        eventbus_allocator_detail::deallocate(ptr);
    }
}
