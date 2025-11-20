#include <Arduino.h>
#include <ESPEventBus.h>

enum class DemoEvent : uint16_t {
    Command,
    Response,
};

struct CommandPayload {
    uint32_t id;
    const char* command;
};

struct ResponsePayload {
    uint32_t id;
    String result;
};

ESPEventBus eventBus;
static ResponsePayload sharedResponse{};

void workerCallback(void* payload, void*) {
    auto* cmd = static_cast<CommandPayload*>(payload);
    sharedResponse.id = cmd->id;
    sharedResponse.result = String("Ack for ") + cmd->command;
    delay(15);  // Simulate doing work before replying
    eventBus.post(DemoEvent::Response, &sharedResponse, portMAX_DELAY);
}

void requestTask(void*) {
    CommandPayload cmd{};
    uint32_t sequence = 0;

    for (;;) {
        cmd.id = ++sequence;
        cmd.command = (sequence % 2 == 0) ? "set-mode=eco" : "status?";
        eventBus.post(DemoEvent::Command, &cmd, portMAX_DELAY);

        auto* response = static_cast<ResponsePayload*>(
            eventBus.waitFor(DemoEvent::Response, pdMS_TO_TICKS(100)));
        if (response) {
            Serial.printf("[requestTask] got response id=%u body=%s\n",
                          response->id, response->result.c_str());
        } else {
            Serial.println("[requestTask] timed out waiting for response");
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial) {
        delay(10);
    }

    EventBusConfig cfg{};
    cfg.queueLength = 8;
    cfg.priority = 4;
    cfg.stackSize = 4096;

    if (!eventBus.init(cfg)) {
        Serial.println("Failed to init ESPEventBus");
        return;
    }

    eventBus.subscribe(DemoEvent::Command, workerCallback);
    xTaskCreatePinnedToCore(requestTask, "request-task", 4096, nullptr, 3, nullptr, 1);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
