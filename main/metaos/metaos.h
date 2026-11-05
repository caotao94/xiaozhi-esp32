#ifndef METAOS_H
#define METAOS_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "device_state.h"
#include "metaos_panel.h"

class SdmmcCard;

namespace metaos {

class Storage;
class ReminderStore;
class MusicPlayer;
class HermesClient;
struct Reminder;

// MetaOS V2 on top of XiaoZhi. Adds SD card storage, reminders, local music and a Hermes
// Agent bridge, exposed to the XiaoZhi LLM as device-side MCP tools and through a touch UI.
//
// The board creates one instance after its display is set up and calls Initialize() from
// InitializeTools(). Everything else runs in the main task: MCP tool callbacks already do,
// and the 1-second timer reschedules itself there.
class MetaOs {
public:
    // `card` may be null; storage-backed features then report that no card is present.
    explicit MetaOs(SdmmcCard* card);
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
    // Queues text for the assistant to answer aloud once the device is idle.
    void QueueSpeech(std::string prompt);
    void SpeakPending();
    void SpeakHermesResult();
    void RetryCard();

    std::unique_ptr<Storage> storage_;
    std::unique_ptr<ReminderStore> reminders_;
    std::unique_ptr<MusicPlayer> music_;
    std::unique_ptr<HermesClient> hermes_;
    MetaSettings settings_;
    std::unique_ptr<MetaPanel> panel_;
    void* tick_timer_ = nullptr;
    std::atomic<bool> tick_scheduled_{false};
    bool storage_loaded_ = false;
    DeviceState last_state_ = kDeviceStateUnknown;
    // Spoken announcements waiting for the device to become idle, oldest first.
    std::vector<std::string> pending_speech_;
    int64_t speech_queued_us_ = 0;
};

}  // namespace metaos

#endif  // METAOS_H
