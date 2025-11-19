#include <Arduino.h>
#include <ESPEventBus.h>

enum class SensorEvent : uint16_t {
    Sample,
};

struct SensorPayload {
    uint32_t sequence;
    float reading;
};

EventBus eventBus;
static SensorPayload pool[4];
static size_t nextSlot = 0;

struct PoolContext {
    SensorPayload* pool;
    size_t size;
};

PoolContext poolCtx{ pool, sizeof(pool) / sizeof(pool[0]) };

bool payloadValidator(EventBusId, void* payload, void* userArg) {
    auto* ctx = static_cast<PoolContext*>(userArg);
    for (size_t i = 0; i < ctx->size; ++i) {
        if (payload == &ctx->pool[i]) {
            return true;
        }
    }
    return false;
}

void pressureCallback(UBaseType_t queued, UBaseType_t capacity, void*) {
    Serial.printf("[EventBus] queue pressure: %u/%u entries in use\n", queued, capacity);
}

void dropCallback(EventBusId id, void*, void*) {
    Serial.printf("[EventBus] dropped payload for event %u (queue stayed full)\n", static_cast<unsigned>(id));
}

void sensorConsumer(void* payload, void*) {
    auto* info = static_cast<SensorPayload*>(payload);
    Serial.printf("[EventBus] seq=%u reading=%.3f\n", info->sequence, info->reading);
    vTaskDelay(pdMS_TO_TICKS(80));  // Pretend to do slow work so we can exercise pressure/drop callbacks
}

void setup() {
    Serial.begin(115200);
    while (!Serial) {
        delay(10);
    }

    EventBusConfig cfg{};
    cfg.queueLength = 4;
    cfg.overflowPolicy = EventBusOverflowPolicy::DropOldest;
    cfg.pressureThresholdPercent = 75;
    cfg.pressureCallback = pressureCallback;
    cfg.dropCallback = dropCallback;
    cfg.payloadValidator = payloadValidator;
    cfg.payloadValidatorArg = &poolCtx;

    if (!eventBus.init(cfg)) {
        Serial.println("Failed to init EventBus");
        return;
    }

    eventBus.subscribe(SensorEvent::Sample, sensorConsumer);
}

void loop() {
    auto& slot = pool[nextSlot];
    slot.sequence++;
    slot.reading = analogReadMilliVolts(34) / 1000.0f;

    if (!eventBus.post(SensorEvent::Sample, &slot, 0)) {
        Serial.println("[EventBus] queue is saturated; payload rejected");
    }

    nextSlot = (nextSlot + 1) % (sizeof(pool) / sizeof(pool[0]));
    delay(10);
}
