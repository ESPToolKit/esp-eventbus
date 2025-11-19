#include <Arduino.h>
#include <ESPEventBus.h>

enum class DemoEvent : uint16_t {
    NetworkGotIP,
};

struct NetworkGotIpPayload {
    uint32_t sequence;
    char address[16];
};

EventBus eventBus;

void networkSimulatorTask(void* pv) {
    static NetworkGotIpPayload payload{};
    uint8_t octet = 100;

    for (;;) {
        payload.sequence++;
        snprintf(payload.address, sizeof(payload.address), "192.168.1.%u", octet++);
        if (octet > 200) {
            octet = 100;
        }

        eventBus.post(DemoEvent::NetworkGotIP, &payload, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

void blockingConsumerTask(void* pv) {
    for (;;) {
        auto* payload = static_cast<NetworkGotIpPayload*>(
            eventBus.waitFor(DemoEvent::NetworkGotIP, portMAX_DELAY));
        if (payload) {
            Serial.printf("[waitFor] seq=%u ip=%s\n", payload->sequence, payload->address);
        }
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial) {
        delay(10);
    }

    if (!eventBus.init()) {
        Serial.println("Failed to init EventBus");
        return;
    }

    eventBus.subscribe(DemoEvent::NetworkGotIP, [](void* payload, void*) {
        auto* info = static_cast<NetworkGotIpPayload*>(payload);
        Serial.printf("[callback] seq=%u ip=%s\n", info->sequence, info->address);
    });

    xTaskCreatePinnedToCore(networkSimulatorTask, "network-sim", 2048, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(blockingConsumerTask, "network-wait", 2048, nullptr, 4, nullptr, 1);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
