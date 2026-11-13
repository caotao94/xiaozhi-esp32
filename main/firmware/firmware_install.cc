#include "firmware_install.h"

#include "sdkconfig.h"

#include <esp_app_format.h>
#include <esp_log.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "FwInstall"

namespace firmware {

namespace {

constexpr size_t kIoBufBytes = 4096;
// app descriptor sits at image offset 0x20 (24B image header + 8B segment header).
constexpr size_t kAppDescOffset = 0x20;

SlotInfo Describe(const esp_partition_t* part, const char* label) {
    SlotInfo info;
    info.part = part;
    info.label = label;
    if (part == nullptr) {
        return info;
    }
    info.size = part->size;
    esp_ota_get_state_partition(part, &info.state);
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        info.version = desc.version;
        info.project_name = desc.project_name;
        info.valid_image = true;
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    info.running = (running != nullptr && running->address == part->address);
    info.boot_selected = (boot != nullptr && boot->address == part->address);
    return info;
}

bool IsOtaSlot(const esp_partition_t* part) {
    return part != nullptr && part->type == ESP_PARTITION_TYPE_APP &&
           (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
            part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1);
}

}  // namespace

std::vector<SlotInfo> FirmwareInstall::Slots() {
    std::vector<SlotInfo> slots;
    const esp_partition_t* factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    const esp_partition_t* ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const esp_partition_t* ota1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    slots.push_back(Describe(factory, "factory"));
    slots.push_back(Describe(ota0, "ota_0"));
    slots.push_back(Describe(ota1, "ota_1"));
    return slots;
}

SlotInfo FirmwareInstall::Running() {
    const esp_partition_t* part = esp_ota_get_running_partition();
    const char* label = "unknown";
    if (part != nullptr) {
        if (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            label = "factory";
        } else if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
            label = "ota_0";
        } else if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
            label = "ota_1";
        }
    }
    return Describe(part, label);
}

SlotInfo FirmwareInstall::BootSelected() {
    const esp_partition_t* part = esp_ota_get_boot_partition();
    const char* label = "unknown";
    if (part != nullptr) {
        if (part->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            label = "factory";
        } else if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
            label = "ota_0";
        } else if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
            label = "ota_1";
        }
    }
    return Describe(part, label);
}

const char* FirmwareInstall::StateName(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW:
            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY:
            return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:
            return "VALID";
        case ESP_OTA_IMG_INVALID:
            return "INVALID";
        case ESP_OTA_IMG_ABORTED:
            return "ABORTED";
        default:
            return "UNDEFINED";
    }
}

bool FirmwareInstall::Install(const FirmwareEntry& entry, const esp_partition_t* target,
                              const InstallProgressFn& progress, std::string& err) {
    if (!IsOtaSlot(target)) {
        err = "target must be ota_0 or ota_1 (factory is protected)";
        return false;
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr && running->address == target->address) {
        err = "cannot install over the running partition";
        return false;
    }
    if (entry.size == 0 || entry.size > target->size) {
        err = "image size exceeds target slot";
        return false;
    }
    if (!entry.target_chip.empty() && entry.target_chip != CONFIG_IDF_TARGET) {
        err = "target chip mismatch: " + entry.target_chip;
        return false;
    }
    // The installer's own identity (rescue BOARD_NAME) differs from the target
    // firmware's. Compare against the hardware board type (BOARD_TYPE, shared by
    // all variants of this board); also accept our own BOARD_NAME for self-update.
    // Manifest target_board should be the board type, e.g. "lichuang-dev".
    if (!entry.target_board.empty() && entry.target_board != BOARD_TYPE &&
        entry.target_board != BOARD_NAME) {
        err = "target board mismatch: " + entry.target_board;
        return false;
    }

    // Cross-check the manifest size against the actual file (TOCTOU hardening).
    struct stat st;
    if (stat(entry.bin_path.c_str(), &st) != 0 ||
        static_cast<uint64_t>(st.st_size) != entry.size) {
        err = "firmware size does not match manifest";
        return false;
    }

    // Re-parse the manifest and bind it to the scanned entry: the signature is
    // verified over these exact bytes, so they must still describe the image the
    // user selected (guards against the card being swapped between scan/install).
    FirmwareEntry reparsed;
    std::string parse_err;
    if (!FirmwareRepo::ParseManifest(entry.manifest_path, reparsed, parse_err) ||
        reparsed.sha256 != entry.sha256 || reparsed.size != entry.size ||
        reparsed.bin_path != entry.bin_path) {
        err = "manifest changed since scan";
        return false;
    }

    // Integrity gate: SHA-256 must match the manifest before any write. The
    // progress callback keeps the UI cancellable during verification too.
    FirmwareEntry mutable_entry = entry;
    auto verify_progress = [&](uint64_t done, uint64_t total) -> bool {
        return progress ? progress(done, total, 1) : true;
    };
    if (!FirmwareRepo::Verify(mutable_entry, verify_progress)) {
        err = mutable_entry.error.empty() ? "SHA-256 verification failed" : mutable_entry.error;
        return false;
    }
    // An invalid/unverifiable signature is always rejected. Whether unsigned
    // images are allowed is a policy choice (CONFIG_METAOS_FW_REQUIRE_SIGNATURE).
    if (!FirmwareRepo::VerifySignature(mutable_entry)) {
        err = "manifest signature invalid or unverifiable";
        return false;
    }
#if CONFIG_METAOS_FW_REQUIRE_SIGNATURE
    if (!mutable_entry.signature_verified) {
        err = "firmware manifest is not signed";
        return false;
    }
#endif
    if (progress && !progress(0, entry.size, 1)) {
        err = "cancelled";
        return false;
    }

    // Sanity-check the app descriptor magic; esp_ota_end does full validation.
    FILE* f = fopen(entry.bin_path.c_str(), "rb");
    if (f == nullptr) {
        err = "cannot open firmware bin";
        return false;
    }
    uint8_t head[kAppDescOffset + sizeof(esp_app_desc_t)];
    size_t got = fread(head, 1, sizeof(head), f);
    // Require the full image header + app descriptor; a short read would leave
    // desc uninitialized (and the version comparison/log could over-read).
    if (got != sizeof(head) || head[0] != 0xE9) {
        fclose(f);
        err = "not a valid ESP image";
        return false;
    }
    uint32_t magic;
    memcpy(&magic, head + kAppDescOffset, sizeof(magic));
    if (magic != ESP_APP_DESC_MAGIC_WORD) {
        fclose(f);
        err = "app descriptor magic invalid";
        return false;
    }
    // Cross-check the real image identity against the manifest (esp_ota_end also
    // validates chip id and checksums later).
    esp_app_desc_t desc;
    memcpy(&desc, head + kAppDescOffset, sizeof(desc));
    if (!entry.version.empty() && entry.version != desc.version) {
        ESP_LOGW(TAG, "manifest version %s != image version %s", entry.version.c_str(),
                 desc.version);
    }

    esp_ota_handle_t handle = 0;
    esp_err_t ota_err = esp_ota_begin(target, entry.size, &handle);
    if (ota_err != ESP_OK) {
        fclose(f);
        if (ota_err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            err = "running firmware is PENDING_VERIFY; confirm or reboot before installing";
        } else {
            err = std::string("esp_ota_begin: ") + esp_err_to_name(ota_err);
        }
        return false;
    }

    // Rewind: the header check above consumed 288 bytes; the OTA image must be
    // written from offset 0 (including the image header and app descriptor).
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        esp_ota_abort(handle);
        err = "firmware seek failed";
        return false;
    }

    // Heap IO buffer: keeps the worker stack small (Verify also buffers 4KB).
    uint8_t* buf = static_cast<uint8_t*>(malloc(kIoBufBytes));
    if (buf == nullptr) {
        fclose(f);
        esp_ota_abort(handle);
        err = "out of memory";
        return false;
    }
    uint64_t done = 0;
    bool ok = true;
    while (done < entry.size) {
        size_t want =
            static_cast<size_t>(std::min<uint64_t>(kIoBufBytes, entry.size - done));
        size_t n = fread(buf, 1, want, f);
        if (n == 0) {
            err = "firmware read failed";
            ok = false;
            break;
        }
        ota_err = esp_ota_write(handle, buf, n);
        if (ota_err != ESP_OK) {
            err = std::string("esp_ota_write: ") + esp_err_to_name(ota_err);
            ok = false;
            break;
        }
        done += n;
        if (progress && !progress(done, entry.size, 2)) {
            err = "cancelled";
            ok = false;
            break;
        }
    }
    free(buf);
    fclose(f);

    if (ok) {
        ota_err = esp_ota_end(handle);
        if (ota_err != ESP_OK) {
            err = std::string("esp_ota_end: ") + esp_err_to_name(ota_err);
            ok = false;
        }
    } else {
        esp_ota_abort(handle);
    }
    if (ok) {
        ESP_LOGI(TAG, "installed %s (%llu bytes) to %s", entry.name.c_str(),
                 static_cast<unsigned long long>(entry.size), target->label);
    }
    return ok;
}

bool FirmwareInstall::Switch(const esp_partition_t* target, std::string& err) {
    if (target == nullptr || target->type != ESP_PARTITION_TYPE_APP) {
        err = "invalid boot target";
        return false;
    }
    esp_err_t e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) {
        err = std::string("set_boot_partition: ") + esp_err_to_name(e);
        return false;
    }
    ESP_LOGI(TAG, "next boot set to %s", target->label);
    return true;
}

}  // namespace firmware
