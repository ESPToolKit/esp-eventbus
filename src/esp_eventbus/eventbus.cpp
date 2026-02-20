#include "eventbus.h"

#include <algorithm>

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
    workerTask_.reset();
    worker_.deinit();
    ESPWorker::Config workerConfig{};
    workerConfig.maxWorkers = 1;
    workerConfig.stackSizeBytes = config_.stackSize;
    workerConfig.priority = config_.priority;
    workerConfig.coreId = config_.coreId;
    workerConfig.enableExternalStacks = true;
    worker_.init(workerConfig);
    resetKernelStorage();
    EventBusVector<Subscription> subStorage{ EventBusAllocator<Subscription>(config_.usePSRAMBuffers) };
    subs_.swap(subStorage);

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

    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }

    if (subMutex_) {
        vSemaphoreDelete(subMutex_);
        subMutex_ = nullptr;
    }

    resetKernelStorage();
    subs_.clear();
    EventBusVector<Subscription> emptySubs{ EventBusAllocator<Subscription>(false) };
    subs_.swap(emptySubs);
    nextSubId_ = 0;
    stopEventPending_ = false;
    config_ = EventBusConfig{};
    workerTask_.reset();
    worker_.deinit();
    task_ = nullptr;
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
    if (!queue_) {
        return nullptr;
    }

    if (task_ && currentTaskHandle() == task_) {
        return nullptr;
    }

    QueueHandle_t responseQueue = nullptr;
    void* responseQueueStorage = nullptr;
    void* responseQueueControlStorage = nullptr;
#if defined(configSUPPORT_STATIC_ALLOCATION) && (configSUPPORT_STATIC_ALLOCATION == 1)
    if (config_.usePSRAMBuffers) {
        responseQueueStorage = allocateKernelStorage(sizeof(void*), true);
        responseQueueControlStorage = allocateKernelStorage(sizeof(StaticQueue_t), true);
        if (responseQueueStorage && responseQueueControlStorage) {
            responseQueue = xQueueCreateStatic(
                1,
                sizeof(void*),
                static_cast<uint8_t*>(responseQueueStorage),
                static_cast<StaticQueue_t*>(responseQueueControlStorage));
        }
        if (!responseQueue) {
            freeKernelStorage(responseQueueStorage);
            freeKernelStorage(responseQueueControlStorage);
            responseQueueStorage = nullptr;
            responseQueueControlStorage = nullptr;
        }
    }
#endif
    if (!responseQueue) {
        responseQueue = xQueueCreate(1, sizeof(void*));
    }
    if (!responseQueue) {
        return nullptr;
    }

    EventBusSub sid = subscribe(id, &ESPEventBus::waiterCallback, responseQueue, true);
    if (!sid) {
        vQueueDelete(responseQueue);
        freeKernelStorage(responseQueueStorage);
        freeKernelStorage(responseQueueControlStorage);
        return nullptr;
    }

    void* payload = nullptr;
    BaseType_t res = xQueueReceive(responseQueue, &payload, timeout);

    unsubscribe(sid);
    vQueueDelete(responseQueue);
    freeKernelStorage(responseQueueStorage);
    freeKernelStorage(responseQueueControlStorage);

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
}

void ESPEventBus::stopTask() {
    if (!workerTask_) {
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

    if (!workerTask_->wait(pdMS_TO_TICKS(3000))) {
        (void)workerTask_->destroy();
    }
    workerTask_.reset();
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

void ESPEventBus::waiterCallback(void* payload, void* userArg) {
    QueueHandle_t queue = reinterpret_cast<QueueHandle_t>(userArg);
    if (!queue) {
        return;
    }
    (void)xQueueSend(queue, &payload, 0);
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
    WorkerConfig taskConfig{};
    taskConfig.stackSizeBytes = config_.stackSize;
    taskConfig.priority = config_.priority;
    taskConfig.coreId = config_.coreId;
    taskConfig.name = (taskName && taskName[0] != '\0') ? taskName : "ESPEventBus";
    // Keep the event-bus worker task on the default FreeRTOS allocation path.
    // Some ESP32 ports assert on caps/static task creation for TCB memory.
    WorkerResult result = worker_.spawn([this]() { taskLoop(); }, taskConfig);
    if (!result) {
        workerTask_.reset();
        task_ = nullptr;
        return false;
    }
    workerTask_ = result.handler;
    task_ = nullptr;
    return true;
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
