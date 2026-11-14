#include "usb_msc.h"

#if CONFIG_METAOS_USB_MSC

#include <esp_log.h>

#include "sd_card.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#define TAG "UsbMsc"

namespace metaos {

namespace {
// Storage event callback. For explicit Start()/SetUsbOwned() calls the component runs this
// synchronously in the calling (main) task; only the auto-mount path (disabled here via
// auto_mount_off) would run it in the TinyUSB task. It only flips the card's atomic mounted
// flag so MetaOS file access gates off before FAT is handed to the USB host, and back on when
// it returns.
void StorageCallback(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t* event, void* arg) {
    auto* card = static_cast<SdmmcCard*>(arg);
    if (card == nullptr || event == nullptr) {
        return;
    }
    switch (event->id) {
        case TINYUSB_MSC_EVENT_MOUNT_START:
            // The event carries the OLD mount point, so gate off on every transition (both
            // APP->USB unmount and USB->APP remount) before the volume changes hands.
            card->SetAppMounted(false);
            break;
        case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
            card->SetAppMounted(event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP);
            if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
                card->SetLastError(ESP_OK);
            }
            break;
        case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        case TINYUSB_MSC_EVENT_FORMAT_FAILED:
            // No usable FAT (e.g. an exFAT card with do_not_format). Report kNoFilesystem so
            // the UI shows the FAT32 guidance instead of a mounted-but-unusable card.
            card->SetAppMounted(false);
            card->SetLastError(ESP_FAIL);
            break;
        default:
            break;
    }
}
}  // namespace

UsbMsc::UsbMsc(SdmmcCard* card) : card_(card) {}

UsbMsc::~UsbMsc() { Stop(); }

bool UsbMsc::Start(bool force) {
    if (started_) {
        return true;
    }
    if (card_ == nullptr || !card_->usb_msc()) {
        ESP_LOGE(TAG, "card not configured for USB MSC");
        return false;
    }
    if (!card_->Mount(force)) {
        ESP_LOGE(TAG, "SD card init failed");
        return false;
    }

    // Native full-speed port (GPIO19/20) with the internal PHY. The PHY is shared with
    // USB-Serial-JTAG, so the secondary USB console is unavailable while MSC runs; the main
    // UART console is unaffected.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(ret));
        return false;
    }
    driver_installed_ = true;

    tinyusb_msc_driver_config_t msc_cfg = {};
    msc_cfg.callback = StorageCallback;
    msc_cfg.callback_arg = card_;
    // Explicit ownership control only: plugging USB must not silently steal the card while
    // MetaOS is using it. The user enters/exits USB mode deliberately.
    msc_cfg.user_flags.auto_mount_off = 1;
    ret = tinyusb_msc_install_driver(&msc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_install_driver: %s", esp_err_to_name(ret));
        Stop();
        return false;
    }
    msc_installed_ = true;

    tinyusb_msc_storage_config_t st = {};
    st.medium.card = card_->card();
    st.fat_fs.base_path = const_cast<char*>(card_->mount_point().c_str());
    st.fat_fs.config.format_if_mount_failed = false;
    st.fat_fs.config.max_files = 5;
    st.fat_fs.config.allocation_unit_size = 16 * 1024;
    st.fat_fs.do_not_format = true;
    st.mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;

    tinyusb_msc_storage_handle_t handle = nullptr;
    ret = tinyusb_msc_new_storage_sdmmc(&st, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc: %s", esp_err_to_name(ret));
        Stop();
        return false;
    }
    storage_ = handle;
    // The component mounts FAT synchronously and fires MOUNT_COMPLETE(APP) on success, which
    // sets the card's mounted flag. A non-FAT card (exFAT) returns ESP_OK via FORMAT_REQUIRED
    // without mounting; do not claim success in that case.
    if (!card_->IsMounted()) {
        ESP_LOGE(TAG, "TF card has no FAT filesystem; format it as FAT32");
        Stop();
        return false;
    }
    started_ = true;
    ESP_LOGI(TAG, "USB MSC started; TF card mounted for the app");
    return true;
}

void UsbMsc::Stop() {
    if (storage_ != nullptr) {
        auto handle = static_cast<tinyusb_msc_storage_handle_t>(storage_);
        // Force USB ownership first: delete_storage() aborts via ESP_ERROR_CHECK if it tries
        // to unmount an APP storage whose medium is already unregistered (the FORMAT_REQUIRED
        // state). Forcing USB makes delete_storage skip that unmount. Safe in every state.
        tinyusb_msc_set_storage_mount_point(handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
        tinyusb_msc_delete_storage(handle);
        storage_ = nullptr;
    }
    if (msc_installed_) {
        tinyusb_msc_uninstall_driver();
        msc_installed_ = false;
    }
    if (driver_installed_) {
        tinyusb_driver_uninstall();
        driver_installed_ = false;
    }
    if (card_ != nullptr) {
        card_->SetAppMounted(false);
    }
    started_ = false;
}

bool UsbMsc::Restart() {
    Stop();
    return Start(true);
}

bool UsbMsc::SetUsbOwned(bool usb) {
    if (!started_ || storage_ == nullptr) {
        return false;
    }
    auto point = usb ? TINYUSB_MSC_STORAGE_MOUNT_USB : TINYUSB_MSC_STORAGE_MOUNT_APP;
    esp_err_t ret =
        tinyusb_msc_set_storage_mount_point(static_cast<tinyusb_msc_storage_handle_t>(storage_), point);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set mount point: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool UsbMsc::IsUsbOwned() const {
    if (!started_ || storage_ == nullptr) {
        return false;
    }
    tinyusb_msc_mount_point_t point = TINYUSB_MSC_STORAGE_MOUNT_APP;
    if (tinyusb_msc_get_storage_mount_point(
            static_cast<tinyusb_msc_storage_handle_t>(storage_), &point) != ESP_OK) {
        return false;
    }
    return point == TINYUSB_MSC_STORAGE_MOUNT_USB;
}

}  // namespace metaos

#endif  // CONFIG_METAOS_USB_MSC