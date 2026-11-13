#pragma once

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <functional>
#include <string>
#include <vector>

#include "firmware_repo.h"

namespace firmware {

struct SlotInfo {
    std::string label;  // "factory" / "ota_0" / "ota_1"
    const esp_partition_t* part = nullptr;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    std::string version;
    std::string project_name;
    uint32_t size = 0;
    bool running = false;
    bool boot_selected = false;
    bool valid_image = false;
};

// (bytes_done, bytes_total, phase) with phase 1=verify, 2=write; return false to abort.
using InstallProgressFn = std::function<bool(uint64_t, uint64_t, int)>;

// OTA install / boot switching built on the ESP-IDF app_update API.
class FirmwareInstall {
public:
    // All app slots present in the partition table (factory, ota_0, ota_1).
    static std::vector<SlotInfo> Slots();
    static SlotInfo Running();
    static SlotInfo BootSelected();
    static const char* StateName(esp_ota_img_states_t state);

    // Verifies (SHA-256, chip/board, size, app descriptor) then writes the image
    // to target with esp_ota_begin/write/end. target MUST be ota_0 or ota_1,
    // never factory and never the running partition. Does NOT change the boot
    // partition; call Switch() after user confirmation.
    static bool Install(const FirmwareEntry& entry, const esp_partition_t* target,
                        const InstallProgressFn& progress, std::string& err);

    // Sets the next boot partition (factory/ota_0/ota_1). Caller reboots.
    static bool Switch(const esp_partition_t* target, std::string& err);
};

}  // namespace firmware
