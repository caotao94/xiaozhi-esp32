#ifndef METAOS_H
#define METAOS_H

#include <atomic>
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

private:
    void RegisterTools();
    void OnTick();
    void FireReminders();
    void LoadSettings();
    void SaveSettings();
    void RetryCard();

    std::unique_ptr<Storage> storage_;
    std::unique_ptr<ReminderStore> reminders_;
    std::unique_ptr<MusicPlayer> music_;
    std::unique_ptr<MotionDetector> motion_;
    Qmi8658* imu_;
    MetaSettings settings_;
    std::unique_ptr<MetaPanel> panel_;
    void* tick_timer_ = nullptr;
    std::atomic<bool> tick_scheduled_{false};
    bool storage_loaded_ = false;
    DeviceState last_state_ = kDeviceStateUnknown;
};

}  // namespace metaos

#endif  // METAOS_H
