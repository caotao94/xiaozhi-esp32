#include "sd_card.h"

#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>

#include <cstdio>
#include <cstdlib>

#define TAG "SdmmcCard"

namespace {
constexpr int64_t kMountRetryIntervalUs = 30LL * 1000 * 1000;
}  // namespace

SdmmcCard::SdmmcCard(const Config& config) : config_(config), mount_point_(config.mount_point) {}

SdmmcCard::~SdmmcCard() {
    if (config_.usb_msc) {
        // FAT is owned by TinyUSB MSC, which must already have deleted its storage (and
        // unmounted FAT) before the card is freed. Only tear down the bare card here.
        if (card_inited_) {
            sdmmc_host_deinit();
            free(card_);
            card_ = nullptr;
            card_inited_ = false;
            mounted_ = false;
        }
        return;
    }
    if (card_ != nullptr) {
        esp_vfs_fat_sdcard_unmount(mount_point_.c_str(), card_);
        card_ = nullptr;
        mounted_ = false;
    }
}

bool SdmmcCard::Mount(bool force) {
    if (config_.usb_msc) {
        if (card_inited_ && !force) {
            return true;
        }
        // Same failed-attempt rate limit as the normal path, so a missing card does not stall
        // the main task on every Ensure(). `force` bypasses it (explicit retry / recovery).
        int64_t now = esp_timer_get_time();
        if (!force && last_attempt_us_ != 0 && now - last_attempt_us_ < kMountRetryIntervalUs) {
            return false;
        }
        last_attempt_us_ = now;
        return MountBare(force);
    }
    if (mounted_) {
        return true;
    }
    int64_t now = esp_timer_get_time();
    if (!force && last_attempt_us_ != 0 && now - last_attempt_us_ < kMountRetryIntervalUs) {
        return false;
    }
    last_attempt_us_ = now;

    bool four_bit =
        config_.d1 != GPIO_NUM_NC && config_.d2 != GPIO_NUM_NC && config_.d3 != GPIO_NUM_NC;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // Filled field by field: SDMMC_SLOT_CONFIG_DEFAULT() assigns D1..D7 to fixed GPIOs and
    // uses C designated initializers.
    sdmmc_slot_config_t slot = {};
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.clk = config_.clk;
    slot.cmd = config_.cmd;
    slot.d0 = config_.d0;
    // Leave unused data lines unconnected so the driver never claims pins that the board
    // uses for something else.
    slot.d1 = four_bit ? config_.d1 : GPIO_NUM_NC;
    slot.d2 = four_bit ? config_.d2 : GPIO_NUM_NC;
    slot.d3 = four_bit ? config_.d3 : GPIO_NUM_NC;
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    slot.width = four_bit ? 4 : 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = config_.max_files;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret =
        esp_vfs_fat_sdmmc_mount(mount_point_.c_str(), &host, &slot, &mount_config, &card);
    if (ret != ESP_OK) {
        last_error_ = ret;
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG,
                     "SD card has no FAT filesystem. Format it as FAT32 (exFAT is not "
                     "supported)");
        } else {
            ESP_LOGW(TAG, "SD card not available: %s", esp_err_to_name(ret));
        }
        return false;
    }

    card_ = card;
    last_error_ = ESP_OK;
    mounted_ = true;
    sdmmc_card_print_info(stdout, card_);
    ESP_LOGI(TAG, "SD card mounted at %s (%d-bit)", mount_point_.c_str(), four_bit ? 4 : 1);
    return true;
}

bool SdmmcCard::MountBare(bool force) {
    if (card_inited_) {
        if (!force) {
            return true;
        }
        // Forced re-init: recovery after the card was pulled or reformatted while USB owned
        // it. The external FAT owner must already have released the card (storage deleted).
        sdmmc_host_deinit();
        free(card_);
        card_ = nullptr;
        card_inited_ = false;
        mounted_ = false;
    }
    bool four_bit =
        config_.d1 != GPIO_NUM_NC && config_.d2 != GPIO_NUM_NC && config_.d3 != GPIO_NUM_NC;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = {};
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.clk = config_.clk;
    slot.cmd = config_.cmd;
    slot.d0 = config_.d0;
    slot.d1 = four_bit ? config_.d1 : GPIO_NUM_NC;
    slot.d2 = four_bit ? config_.d2 : GPIO_NUM_NC;
    slot.d3 = four_bit ? config_.d3 : GPIO_NUM_NC;
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    slot.width = four_bit ? 4 : 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // Initialize the card without mounting FAT: TinyUSB MSC owns the filesystem and mounts
    // it itself (APP) or exposes the raw card to the USB host (USB).
    esp_err_t ret = sdmmc_host_init();
    if (ret != ESP_OK) {
        last_error_ = ret;
        ESP_LOGW(TAG, "SDMMC host init failed: %s", esp_err_to_name(ret));
        return false;
    }
    ret = sdmmc_host_init_slot(host.slot, &slot);
    if (ret != ESP_OK) {
        sdmmc_host_deinit();
        last_error_ = ret;
        ESP_LOGW(TAG, "SDMMC slot init failed: %s", esp_err_to_name(ret));
        return false;
    }
    sdmmc_card_t* card = static_cast<sdmmc_card_t*>(malloc(sizeof(sdmmc_card_t)));
    if (card == nullptr) {
        sdmmc_host_deinit();
        last_error_ = ESP_ERR_NO_MEM;
        return false;
    }
    ret = sdmmc_card_init(&host, card);
    if (ret != ESP_OK) {
        free(card);
        sdmmc_host_deinit();
        last_error_ = ret;
        ESP_LOGW(TAG, "SD card not available: %s", esp_err_to_name(ret));
        return false;
    }
    card_ = card;
    card_inited_ = true;
    last_error_ = ESP_OK;
    sdmmc_card_print_info(stdout, card_);
    ESP_LOGI(TAG, "SD card initialized for USB MSC (%d-bit), FAT owned by TinyUSB",
             four_bit ? 4 : 1);
    return true;
}

void SdmmcCard::SetAppMounted(bool mounted) {
    if (config_.usb_msc) {
        mounted_ = mounted;
    }
}

void SdmmcCard::SetLastError(esp_err_t err) {
    if (config_.usb_msc) {
        last_error_ = err;
    }
}

bool SdmmcCard::GetUsage(uint64_t& total_bytes, uint64_t& free_bytes) const {
    if (!mounted_) {
        return false;
    }
    return esp_vfs_fat_info(mount_point_.c_str(), &total_bytes, &free_bytes) == ESP_OK;
}
