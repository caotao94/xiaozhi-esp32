#ifndef METAOS_FIRMWARE_H
#define METAOS_FIRMWARE_H

#include <sdkconfig.h>

#if CONFIG_METAOS_ENABLE

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "firmware_install.h"
#include "firmware_repo.h"

namespace metaos {

class Storage;

// Main-firmware-side firmware manager. Reuses the shared firmware:: core
// (repository scan / SHA-256 / OTA install / boot switching) and runs the heavy
// SD + flash work on a dedicated worker task so the main event loop and audio
// are never blocked. The UI polls state from the main task.
//
// Unlike the rescue firmware, the SD card here is already mounted by MetaOs on
// the main task; the worker only does read-only scans and OTA writes (it never
// calls SdmmcCard::Mount, honoring its single-task contract).
class FirmwareManager {
public:
    enum class Phase { kIdle, kScanning, kVerify, kWrite, kOk, kFail };

    explicit FirmwareManager(Storage& storage);
    ~FirmwareManager();

    FirmwareManager(const FirmwareManager&) = delete;
    FirmwareManager& operator=(const FirmwareManager&) = delete;

    // Creates the worker task and job queue. Call once from the main task.
    bool Start();

    // Main task. Requests a repository rescan (no-op if busy).
    void RequestScan();
    // Main task. Begins verify+install of entry into target (ota_0/ota_1 only).
    bool RequestInstall(const firmware::FirmwareEntry& entry, const esp_partition_t* target);
    // Main task. Requests cancellation of the running job.
    void Cancel();
    // Main task, ~2x/second. Moves completed worker results into main-task state.
    void Poll();

    Phase phase() const { return phase_.load(); }
    bool busy() const {
        auto p = phase_.load();
        // Include the terminal states: until Poll() consumes the result, a new
        // job must not start (it would overwrite phase_ and lose the result).
        return p == Phase::kScanning || p == Phase::kVerify || p == Phase::kWrite ||
               p == Phase::kOk || p == Phase::kFail;
    }
    int percent() const;
    // Main task. Snapshot of the last completed scan.
    const std::vector<firmware::FirmwareEntry>& entries() const { return entries_; }
    std::string last_error() const;
    // Main task. The entry/target of the last finished install (for the result UI).
    firmware::FirmwareEntry installed() const;
    const esp_partition_t* install_target() const { return inst_target_; }
    // Main task. True once a finished install result has been consumed by the UI.
    void AcknowledgeResult();
    bool has_result() const { return has_result_; }

private:
    struct Job;
    static void WorkerEntry(void* arg);
    void WorkerLoop();

    Storage& storage_;
    void* queue_ = nullptr;
    void* task_ = nullptr;

    std::atomic<Phase> phase_{Phase::kIdle};
    std::atomic<uint64_t> done_{0};
    std::atomic<uint64_t> total_{0};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> scan_done_{false};

    std::mutex staging_mutex_;
    std::vector<firmware::FirmwareEntry> staging_;
    std::string staging_error_;        // scan error
    std::string install_error_;        // install error (separate so a pending scan
                                       // result cannot clobber it in Poll)

    // Main-task state (only touched from the main task).
    std::vector<firmware::FirmwareEntry> entries_;
    firmware::FirmwareEntry inst_entry_;
    const esp_partition_t* inst_target_ = nullptr;
    std::string last_error_;
    bool has_result_ = false;
};

}  // namespace metaos

#endif  // CONFIG_METAOS_ENABLE
#endif  // METAOS_FIRMWARE_H
