#include <Arduino.h>
#include <ESPEventBus.h>
#include <unity.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class TestEvent : uint16_t {
    FastTick = 1,
    SlowTick,
};

static void noopCallback(void*, void*) {
}

struct DropContext {
    volatile int received = 0;
    volatile int dropped = 0;
};

struct CounterContext {
    volatile int callbacks = 0;
};

static void slowSubscriber(void*, void* userArg) {
    auto* ctx = static_cast<DropContext*>(userArg);
    if (ctx) {
        ctx->received++;
    }
    vTaskDelay(pdMS_TO_TICKS(25));
}

static void dropCallback(EventBusId, void*, void* userArg) {
    auto* ctx = static_cast<DropContext*>(userArg);
    if (ctx) {
        ctx->dropped++;
    }
}

static void counterCallback(void*, void* userArg) {
    auto* ctx = static_cast<CounterContext*>(userArg);
    if (ctx) {
        ctx->callbacks++;
    }
}

struct PressureContext {
    volatile bool triggered = false;
};

static void pressureCallback(UBaseType_t, UBaseType_t, void* userArg) {
    auto* ctx = static_cast<PressureContext*>(userArg);
    if (ctx) {
        ctx->triggered = true;
    }
}

struct TestPayload {
    int value;
};

struct PoolContext {
    TestPayload* pool;
    size_t size;
};

static bool poolValidator(EventBusId, void* payload, void* userArg) {
    auto* ctx = static_cast<PoolContext*>(userArg);
    if (!ctx) {
        return true;
    }
    for (size_t i = 0; i < ctx->size; ++i) {
        if (payload == &ctx->pool[i]) {
            return true;
        }
    }
    return false;
}

struct ProducerContext {
    ESPEventBus* bus = nullptr;
    TestPayload payload{};
    volatile bool stop = false;
    TaskHandle_t handle = nullptr;
};

static void producerTask(void* arg) {
    auto* ctx = static_cast<ProducerContext*>(arg);
    while (!ctx->stop) {
        if (ctx->bus) {
            ctx->bus->post(TestEvent::FastTick, &ctx->payload, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ctx->handle = nullptr;
    vTaskDelete(nullptr);
}

void test_subscription_cap_limits_handles() {
    ESPEventBus bus;
    EventBusConfig cfg{};
    cfg.maxSubscriptions = 2;
    TEST_ASSERT_TRUE(bus.init(cfg));

    auto subA = bus.subscribe(TestEvent::FastTick, noopCallback);
    auto subB = bus.subscribe(TestEvent::FastTick, noopCallback);
    auto subC = bus.subscribe(TestEvent::FastTick, noopCallback);

    TEST_ASSERT_NOT_EQUAL(0U, subA);
    TEST_ASSERT_NOT_EQUAL(0U, subB);
    TEST_ASSERT_EQUAL_UINT32(0, subC);

    bus.deinit();
}

void test_payload_validator_rejects_unknown_payloads() {
    ESPEventBus bus;
    TestPayload pool[2]{};
    PoolContext ctx{ pool, 2 };

    EventBusConfig cfg{};
    cfg.payloadValidator = poolValidator;
    cfg.payloadValidatorArg = &ctx;
    TEST_ASSERT_TRUE(bus.init(cfg));

    TEST_ASSERT_TRUE(bus.post(TestEvent::FastTick, &pool[0], portMAX_DELAY));
    TestPayload notOwned{};
    TEST_ASSERT_FALSE(bus.post(TestEvent::FastTick, &notOwned, 0));

    vTaskDelay(pdMS_TO_TICKS(20));
    bus.deinit();
}

void test_drop_oldest_policy_discards_backlog() {
    ESPEventBus bus;
    DropContext ctx{};
    TestPayload scratch[8]{};

    EventBusConfig cfg{};
    cfg.queueLength = 2;
    cfg.overflowPolicy = EventBusOverflowPolicy::DropOldest;
    cfg.dropCallback = dropCallback;
    cfg.dropUserArg = &ctx;
    TEST_ASSERT_TRUE(bus.init(cfg));

    bus.subscribe(TestEvent::FastTick, slowSubscriber, &ctx);
    for (int i = 0; i < 8; ++i) {
        bus.post(TestEvent::FastTick, &scratch[i], 0);
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_TRUE(ctx.dropped > 0);
    TEST_ASSERT_TRUE(ctx.received > 0);

    bus.deinit();
}

void test_drop_newest_policy_discards_incoming_event() {
    ESPEventBus bus;
    DropContext ctx{};
    TestPayload scratch[6]{};

    EventBusConfig cfg{};
    cfg.queueLength = 1;
    cfg.overflowPolicy = EventBusOverflowPolicy::DropNewest;
    cfg.dropCallback = dropCallback;
    cfg.dropUserArg = &ctx;
    TEST_ASSERT_TRUE(bus.init(cfg));

    bus.subscribe(TestEvent::FastTick, slowSubscriber, &ctx);
    for (int i = 0; i < 6; ++i) {
        bus.post(TestEvent::FastTick, &scratch[i], 0);
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_TRUE(ctx.dropped > 0);
    TEST_ASSERT_TRUE(ctx.received < 6);

    bus.deinit();
}

void test_pressure_callback_triggers_on_high_usage() {
    ESPEventBus bus;
    DropContext dropCtx{};
    PressureContext pressureCtx{};

    EventBusConfig cfg{};
    cfg.queueLength = 3;
    cfg.overflowPolicy = EventBusOverflowPolicy::DropOldest;
    cfg.pressureThresholdPercent = 50;
    cfg.pressureCallback = pressureCallback;
    cfg.pressureUserArg = &pressureCtx;
    cfg.dropCallback = dropCallback;
    cfg.dropUserArg = &dropCtx;

    TEST_ASSERT_TRUE(bus.init(cfg));
    bus.subscribe(TestEvent::FastTick, slowSubscriber, &dropCtx);

    TestPayload scratch[5]{};
    for (int i = 0; i < 5; ++i) {
        bus.post(TestEvent::FastTick, &scratch[i], 0);
    }

    vTaskDelay(pdMS_TO_TICKS(150));
    TEST_ASSERT_TRUE(pressureCtx.triggered);

    bus.deinit();
}

void test_deinit_completes_when_queue_is_busy() {
    ESPEventBus bus;
    DropContext ctx{};

    EventBusConfig cfg{};
    cfg.queueLength = 1;
    cfg.overflowPolicy = EventBusOverflowPolicy::Block;
    TEST_ASSERT_TRUE(bus.init(cfg));

    bus.subscribe(TestEvent::FastTick, slowSubscriber, &ctx);

    ProducerContext producer{};
    producer.bus = &bus;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(
                                  producerTask, "producer", 2048, &producer, 4, &producer.handle, 1));

    vTaskDelay(pdMS_TO_TICKS(100));
    TickType_t start = xTaskGetTickCount();
    bus.deinit();
    TickType_t elapsed = xTaskGetTickCount() - start;
    TEST_ASSERT_LESS_THAN_UINT32(pdMS_TO_TICKS(1000), elapsed);

    producer.stop = true;
    while (producer.handle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void test_deinit_is_safe_before_init_and_idempotent() {
    ESPEventBus bus;
    TestPayload payload{};

    TEST_ASSERT_FALSE(bus.isInitialized());

    bus.deinit();
    TEST_ASSERT_FALSE(bus.isInitialized());
    TEST_ASSERT_FALSE(bus.post(TestEvent::FastTick, &payload, 0));

    bus.deinit();
    TEST_ASSERT_FALSE(bus.isInitialized());
    TEST_ASSERT_FALSE(bus.post(TestEvent::FastTick, &payload, 0));
}

void test_reinit_clears_subscriptions_and_restores_bus() {
    ESPEventBus bus;
    CounterContext ctx{};
    TestPayload payload{};

    TEST_ASSERT_TRUE(bus.init());
    TEST_ASSERT_TRUE(bus.isInitialized());
    TEST_ASSERT_NOT_EQUAL(0U, bus.subscribe(TestEvent::FastTick, counterCallback, &ctx));
    TEST_ASSERT_TRUE(bus.post(TestEvent::FastTick, &payload, portMAX_DELAY));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_TRUE(ctx.callbacks > 0);

    bus.deinit();
    TEST_ASSERT_FALSE(bus.isInitialized());

    const int callbacksAfterFirstRun = ctx.callbacks;
    TEST_ASSERT_TRUE(bus.init());
    TEST_ASSERT_TRUE(bus.isInitialized());
    TEST_ASSERT_TRUE(bus.post(TestEvent::FastTick, &payload, portMAX_DELAY));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_EQUAL_INT(callbacksAfterFirstRun, ctx.callbacks);

    TEST_ASSERT_NOT_EQUAL(0U, bus.subscribe(TestEvent::FastTick, counterCallback, &ctx));
    TEST_ASSERT_TRUE(bus.post(TestEvent::FastTick, &payload, portMAX_DELAY));
    vTaskDelay(pdMS_TO_TICKS(30));
    TEST_ASSERT_TRUE(ctx.callbacks > callbacksAfterFirstRun);

    bus.deinit();
    bus.deinit();
    TEST_ASSERT_FALSE(bus.isInitialized());
}

void setUp() {
}

void tearDown() {
}

void setup() {
    delay(2000);  // Give the serial monitor time to settle
    UNITY_BEGIN();
    RUN_TEST(test_subscription_cap_limits_handles);
    RUN_TEST(test_payload_validator_rejects_unknown_payloads);
    RUN_TEST(test_drop_oldest_policy_discards_backlog);
    RUN_TEST(test_drop_newest_policy_discards_incoming_event);
    RUN_TEST(test_pressure_callback_triggers_on_high_usage);
    RUN_TEST(test_deinit_completes_when_queue_is_busy);
    RUN_TEST(test_deinit_is_safe_before_init_and_idempotent);
    RUN_TEST(test_reinit_clears_subscriptions_and_restores_bus);
    UNITY_END();
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
