#include "metaos_worker.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define TAG "MetaWorker"

namespace metaos {

namespace {
// Image decoding and Opus encoding are stack hungry; the stack lives in PSRAM, so be generous.
constexpr uint32_t kStackBytes = 32 * 1024;
// Below the main loop and the audio tasks.
constexpr UBaseType_t kPriority = 1;
constexpr UBaseType_t kQueueDepth = 4;

QueueHandle_t queue = nullptr;

void WorkerTask(void*) {
    while (true) {
        std::function<void()>* job = nullptr;
        if (xQueueReceive(queue, &job, portMAX_DELAY) == pdTRUE && job != nullptr) {
            (*job)();
            delete job;
        }
    }
}

bool EnsureStarted() {
    if (queue != nullptr) {
        return true;
    }
    QueueHandle_t created = xQueueCreate(kQueueDepth, sizeof(std::function<void()>*));
    if (created == nullptr) {
        return false;
    }
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCoreWithCaps(WorkerTask, "metaos_work", kStackBytes, nullptr,
                                        kPriority, &task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start the worker task");
        vQueueDelete(created);
        return false;
    }
    queue = created;
    return true;
}
}  // namespace

bool PostWork(std::function<void()> job) {
    if (!EnsureStarted()) {
        return false;
    }
    auto* boxed = new (std::nothrow) std::function<void()>(std::move(job));
    if (boxed == nullptr) {
        return false;
    }
    if (xQueueSend(queue, &boxed, 0) != pdTRUE) {
        delete boxed;
        return false;
    }
    return true;
}

}  // namespace metaos
