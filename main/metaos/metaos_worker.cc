#include "metaos_worker.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>

#define TAG "MetaWorker"

namespace metaos {

namespace {
// Image decoding and Opus encoding are stack hungry; the stack lives in PSRAM, so be generous.
constexpr uint32_t kStackBytes = 32 * 1024;
// Below the main loop and the audio tasks.
constexpr UBaseType_t kPriority = 1;
constexpr UBaseType_t kQueueDepth = 4;

QueueHandle_t queue = nullptr;
// Gates new jobs (USB MSC quiesce) and counts queued+running jobs so callers can wait for
// the worker to drain before handing the SD card to USB.
std::atomic<bool> enabled{true};
std::atomic<int> pending{0};

void WorkerTask(void*) {
    while (true) {
        std::function<void()>* job = nullptr;
        if (xQueueReceive(queue, &job, portMAX_DELAY) == pdTRUE && job != nullptr) {
            (*job)();
            delete job;
            pending.fetch_sub(1);
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
    if (!enabled.load() || !EnsureStarted()) {
        return false;
    }
    auto* boxed = new (std::nothrow) std::function<void()>(std::move(job));
    if (boxed == nullptr) {
        return false;
    }
    // Count before enqueue so WorkIdle() never reads idle between enqueue and pickup.
    pending.fetch_add(1);
    if (xQueueSend(queue, &boxed, 0) != pdTRUE) {
        pending.fetch_sub(1);
        delete boxed;
        return false;
    }
    return true;
}

void SetWorkEnabled(bool on) { enabled.store(on); }

bool WorkIdle() { return pending.load() == 0; }

}  // namespace metaos
