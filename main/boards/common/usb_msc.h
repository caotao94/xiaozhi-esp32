#ifndef USB_MSC_H
#define USB_MSC_H

#include <sdkconfig.h>

#if CONFIG_METAOS_USB_MSC

class SdmmcCard;

namespace metaos {

// Exposes the TF card to a USB host as a Mass Storage device (a U-disk), so a PC can copy
// TTS voice data / music without removing the card. The TinyUSB MSC component owns the FAT
// filesystem: mount point APP means the firmware uses the card, USB means the PC uses it.
// While USB owns the card the firmware must not touch /sdcard; SdmmcCard::IsMounted() tracks
// the owner so MetaOS file access gates off automatically.
//
// All public methods run in the main task. For these explicit calls the storage event
// callback also runs synchronously in the main task and only updates the card's atomic
// mounted flag.
class UsbMsc {
public:
    explicit UsbMsc(SdmmcCard* card);
    ~UsbMsc();

    UsbMsc(const UsbMsc&) = delete;
    UsbMsc& operator=(const UsbMsc&) = delete;

    // Initializes the bare card and starts the MSC device with the card mounted for the app.
    // The card must be configured with usb_msc=true. Returns false on any failure (cleaned up),
    // including a card without a FAT filesystem. `force` re-initializes the bare card.
    bool Start(bool force = false);
    // Deletes the storage (unmounts FAT), then uninstalls the MSC and TinyUSB drivers.
    void Stop();
    // Tears down and re-initializes (recovery after the card was pulled/reformatted under USB).
    bool Restart();

    // Switch ownership: true exposes the card to the USB host, false returns it to the
    // firmware. The caller must ensure no firmware file access is in flight first.
    bool SetUsbOwned(bool usb);
    bool IsUsbOwned() const;
    bool started() const { return started_; }

private:
    SdmmcCard* card_;
    void* storage_ = nullptr;  // tinyusb_msc_storage_handle_t
    bool started_ = false;
    bool driver_installed_ = false;
    bool msc_installed_ = false;
};

}  // namespace metaos

#endif  // CONFIG_METAOS_USB_MSC
#endif  // USB_MSC_H