#include "metaos_firmware.h"

#if CONFIG_METAOS_ENABLE

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstring>
#include <utility>

#include "metaos_common.h"

#define TAG "MetaFirmware"

namespace metaos {

struct FirmwareManager::Job {
    enum Type { kScan, kInstall } type;
    firmware::FirmwareEntry entry;
    const esp_partition_t* target = nullptr;
};

FirmwareManager::FirmwareManager(Storage& storage) : storage_(storage) {}

FirmwareManager::~FirmwareManager() {
    // The worker lives for the device lifetime; tasks/queues are not torn down
    // (MetaOs is never destroyed in practice). Intentionally no delete here.
}

bool FirmwareManager::Start() {
    if (queue_ != nullptr) {
        return true;
    }
    auto q = xQueueCreate(4, sizeof(Job*));
    if (q == nullptr) {
        ESP_LOGE(TAG, "queue create failed");
        return false;
    }
    queue_ = q;
    BaseType_t ok = xTaskCreate(WorkerEntry, "meta_fw", 16384, this, 2,
                                reinterpret_cast<TaskHandle_t*>(&task_));
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "worker create failed");
        vQueueDelete(q);
        queue_ = nullptr;
        return false;
    }
    return true;
}

void FirmwareManager::WorkerEntry(void* arg) {
    static_cast<FirmwareManager*>(arg)->WorkerLoop();
}

void FirmwareManager::WorkerLoop() {
    auto q = static_cast<QueueHandle_t>(queue_);
    Job* job = nullptr;
    while (true) {
        if (xQueueReceive(q, &job, portMAX_DELAY) != pdTRUE || job == nullptr) {
            continue;
        }
        if (job->type == Job::kScan) {
            std::vector<firmware::FirmwareEntry> result;
            std::string err;
            if (!storage_.IsMounted()) {
                err = "SD card not mounted";
            } else {
                result = firmware::FirmwareRepo().Scan(storage_.DataPath("firmware"));
            }
            {
                std::lock_guard<std::mutex> lock(staging_mutex_);
                staging_ = std::move(result);
                staging_error_ = err;
            }
            scan_done_.store(true);
            phase_.store(Phase::kIdle);
        } else {
            phase_.store(Phase::kVerify);
            done_.store(0);
            total_.store(job->entry.size);
            auto progress = [this](uint64_t done, uint64_t total, int phase) -> bool {
                done_.store(done);
                total_.store(total);
                phase_.store(phase == 1 ? Phase::kVerify : Phase::kWrite);
                return !cancel_.load();
            };
            std::string err;
            bool ok = firmware::FirmwareInstall::Install(job->entry, job->target, progress, err);
            {
                std::lock_guard<std::mutex> lock(staging_mutex_);
                install_error_ = ok ? "" : err;
            }
            phase_.store(ok ? Phase::kOk : Phase::kFail);
        }
        delete job;
    }
}

void FirmwareManager::RequestScan() {
    if (busy() || queue_ == nullptr) {
        return;
    }
    cancel_.store(false);
    phase_.store(Phase::kScanning);
    auto* job = new Job{Job::kScan, firmware::FirmwareEntry(), nullptr};
    if (xQueueSend(static_cast<QueueHandle_t>(queue_), &job, 0) != pdTRUE) {
        delete job;
        phase_.store(Phase::kIdle);
    }
}

bool FirmwareManager::RequestInstall(const firmware::FirmwareEntry& entry,
                                     const esp_partition_t* target) {
    if (busy() || queue_ == nullptr || target == nullptr) {
        return false;
    }
    inst_entry_ = entry;
    inst_target_ = target;
    has_result_ = false;
    cancel_.store(false);
    done_.store(0);
    total_.store(entry.size);
    phase_.store(Phase::kVerify);
    auto* job = new Job{Job::kInstall, entry, target};
    if (xQueueSend(static_cast<QueueHandle_t>(queue_), &job, 0) != pdTRUE) {
        delete job;
        phase_.store(Phase::kIdle);
        return false;
    }
    return true;
}

void FirmwareManager::Cancel() { cancel_.store(true); }

void FirmwareManager::Poll() {
    if (scan_done_.load()) {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        entries_ = std::move(staging_);
        last_error_ = staging_error_;
        staging_error_.clear();
        scan_done_.store(false);
    }
    auto p = phase_.load();
    if ((p == Phase::kOk || p == Phase::kFail) && !has_result_) {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        last_error_ = install_error_;
        has_result_ = true;
        phase_.store(Phase::kIdle);
    }
}

int FirmwareManager::percent() const {
    uint64_t total = total_.load();
    return total > 0 ? static_cast<int>(done_.load() * 100 / total) : 0;
}

std::string FirmwareManager::last_error() const { return last_error_; }

firmware::FirmwareEntry FirmwareManager::installed() const { return inst_entry_; }

void FirmwareManager::AcknowledgeResult() { has_result_ = false; }

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE
