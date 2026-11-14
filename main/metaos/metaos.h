#ifndef METAOS_H
#define METAOS_H

#include <sdkconfig.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "device_state.h"
#include "metaos_panel.h"

class SdmmcCard;
class Qmi8658;

namespace metaos {

class Storage;
class ReminderStore;
class MusicPlayer;
class MotionDetector;
class SpeechSynthesizer;
class FirmwareManager;
class UsbMsc;
struct Reminder;

// MetaOS V2 on top of XiaoZhi. Adds SD card storage, reminders and local music playback,
// exposed to the XiaoZhi LLM as device-side MCP tools and through a touch UI.
//
// The board creates one instance after its display is set up and calls Initialize() from
// InitializeTools(). Everything else runs in the main task: MCP tool callbacks already do,
// and the 1-second timer reschedules itself there.
class MetaOs {
public:
    // `card` may be null; storage-backed features then report that no card is present.
    // `imu` may be null; motion gestures are then unavailable.
    explicit MetaOs(SdmmcCard* card, Qmi8658* imu = nullptr);
    ~MetaOs();

    MetaOs(const MetaOs&) = delete;
    MetaOs& operator=(const MetaOs&) = delete;

    void Initialize();

    // Main task. Opens the touch UI, or closes it when it is open (LCD with LVGL only).
    void TogglePanel();

    // USB MSC (TF card as a U-disk). Main task. available() is true when the feature is
    // compiled in and started; active() is true while entering or holding USB ownership.
    bool usb_msc_available() const;
    bool usb_msc_active() const;
    // Request entering (on=true) or leaving (on=false) USB mode. Entering is asynchronous:
    // file access is quiesced and the card is handed to USB once all workers are idle.
    void RequestUsbMode(bool on);

private:
    void RegisterTools();
    void OnTick();
    void FireReminders();
    void LoadSettings();
    void SaveSettings();
    void RetryCard();
    // Main task. Completes a pending USB-mode entry once every file user is idle.
    void TickUsb();

    std::unique_ptr<Storage> storage_;
    std::unique_ptr<ReminderStore> reminders_;
    std::unique_ptr<MusicPlayer> music_;
    std::unique_ptr<MotionDetector> motion_;
    std::unique_ptr<SpeechSynthesizer> speech_;
    std::unique_ptr<FirmwareManager> firmware_;
    Qmi8658* imu_;
    SdmmcCard* card_ = nullptr;
#if CONFIG_METAOS_USB_MSC
    std::unique_ptr<UsbMsc> usb_msc_;
    enum class UsbState { kOff, kPending, kActive };
    UsbState usb_state_ = UsbState::kOff;
    int64_t usb_pending_since_us_ = 0;
#endif
    MetaSettings settings_;
    std::unique_ptr<MetaPanel> panel_;
    void* tick_timer_ = nullptr;
    std::atomic<bool> tick_scheduled_{false};
    bool storage_loaded_ = false;
    DeviceState last_state_ = kDeviceStateUnknown;
};

}  // namespace metaos

#endif  // METAOS_H
