#include "metaos.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstdio>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "metaos_common.h"
#include "metaos_motion.h"
#include "metaos_music.h"
#include "metaos_reminders.h"
#include "metaos_strings.h"
#include "qmi8658.h"
#include "sd_card.h"
#include "settings.h"

#define TAG "MetaOS"

namespace metaos {

namespace {
// NVS keys are persistent API: do not rename them.
constexpr const char* kSettingsNamespace = "metaos";
// Kept (unused for now) so the user's choice survives until spoken reminders return.
constexpr const char* kKeySpeakReminders = "speak_remind";
constexpr const char* kKeyShakeNext = "shake_next";
constexpr const char* kKeyFlipStop = "flip_stop";

// Plays a short sound unless it would interrupt the assistant or local music.
void PlayAlertSound(const std::string_view& sound) {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateIdle && !app.IsLocalPlaybackActive()) {
        app.PlaySound(sound);
    }
}

bool HasLvglScreen() {
    auto display = Board::GetInstance().GetDisplay();
    return display != nullptr && display->SupportsGuiOperations();
}
}  // namespace

MetaOs::MetaOs(SdmmcCard* card, Qmi8658* imu)
    : storage_(std::make_unique<Storage>(card)), imu_(imu) {
    reminders_ = std::make_unique<ReminderStore>(*storage_);
    music_ = std::make_unique<MusicPlayer>(*storage_);
    panel_ = std::make_unique<MetaPanel>(
        *storage_, *music_, *reminders_, settings_,
        MetaPanel::Hooks{
            .snooze = [this](const Reminder& reminder,
                             int minutes) { reminders_->Snooze(reminder, minutes); },
            .settings_changed = [this]() { SaveSettings(); },
            .retry_card = [this]() { RetryCard(); },
        });
    if (imu_ != nullptr) {
        motion_ = std::make_unique<MotionDetector>(imu_);
    }
}

MetaOs::~MetaOs() {
    if (motion_ != nullptr) {
        motion_->Stop();
    }
    if (tick_timer_ != nullptr) {
        auto timer = static_cast<esp_timer_handle_t>(tick_timer_);
        esp_timer_stop(timer);
        esp_timer_delete(timer);
    }
}

void MetaOs::LoadSettings() {
    Settings settings(kSettingsNamespace, false);
    settings_.speak_reminders = settings.GetBool(kKeySpeakReminders, true);
    settings_.shake_next = settings.GetBool(kKeyShakeNext, false);
    settings_.flip_stop = settings.GetBool(kKeyFlipStop, false);
}

void MetaOs::SaveSettings() {
    Settings settings(kSettingsNamespace, true);
    settings.SetBool(kKeySpeakReminders, settings_.speak_reminders);
    settings.SetBool(kKeyShakeNext, settings_.shake_next);
    settings.SetBool(kKeyFlipStop, settings_.flip_stop);

    // Update motion detector if active
    if (motion_ != nullptr) {
        MotionDetector::Settings motion_settings;
        motion_settings.shake_enabled = settings_.shake_next;
        motion_settings.flip_enabled = settings_.flip_stop;
        motion_->UpdateSettings(motion_settings);
    }
}

void MetaOs::Initialize() {
    LoadSettings();
    // Mount early so the first tool call does not pay for card initialization.
    if (storage_->Ensure()) {
        reminders_->Load();
        storage_loaded_ = true;
        // The first free-space query can scan the whole FAT on a large card. Do it once at
        // boot; FatFs caches the result for later queries.
        uint64_t total = 0, free = 0;
        if (storage_->GetUsage(total, free)) {
            ESP_LOGI(TAG, "SD card: %llu MB free of %llu MB",
                     static_cast<unsigned long long>(free >> 20),
                     static_cast<unsigned long long>(total >> 20));
        }
    } else {
        ESP_LOGW(TAG, "Running without an SD card");
    }

    music_->SetFailedCallback([this](const std::string& title) {
        panel_->ShowToast(std::string(text::kPlayFailed) + ": " + title);
    });

    // Start motion detection if IMU is available
    if (motion_ != nullptr) {
        MotionDetector::Settings motion_settings;
        motion_settings.shake_enabled = settings_.shake_next;
        motion_settings.flip_enabled = settings_.flip_stop;
        motion_->Start(MotionDetector::Callbacks{
                           .on_shake =
                               [this]() {
                                   if (music_->IsPlaying()) {
                                       music_->SkipNext();
                                       panel_->ShowToast("已切歌");
                                   }
                               },
                           .on_flip_over =
                               [this]() {
                                   if (music_->IsPlaying()) {
                                       music_->StopPlayback();
                                       panel_->ShowToast("已停止");
                                   } else if (panel_->IsAlertOpen()) {
                                       panel_->CloseAlert();
                                   }
                               },
                       },
                       motion_settings);
    }

    RegisterTools();

    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        auto self = static_cast<MetaOs*>(arg);
        // Skip a tick while the previous one is still queued, so a busy main loop never
        // accumulates them.
        if (self->tick_scheduled_.exchange(true)) {
            return;
        }
        Application::GetInstance().Schedule([self]() {
            self->tick_scheduled_ = false;
            self->OnTick();
        });
    };
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "metaos_tick";
    args.skip_unhandled_events = true;
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        tick_timer_ = timer;
        // Twice a second keeps the music page responsive.
        esp_timer_start_periodic(timer, 500 * 1000);
    }
    ESP_LOGI(TAG, "MetaOS ready (SD card %s)", storage_->IsMounted() ? "mounted" : "missing");
}

void MetaOs::OnTick() {
    // A new conversation needs the face and subtitles, so the panel and dialogs give way.
    DeviceState state = Application::GetInstance().GetDeviceState();
    if (state != last_state_) {
        if (state == kDeviceStateConnecting || state == kDeviceStateListening) {
            panel_->Hide();
        }
        last_state_ = state;
    }
    if (!storage_loaded_ && storage_->IsMounted()) {
        reminders_->Load();
        storage_loaded_ = true;
    }
    FireReminders();
    music_->Tick();
    panel_->Refresh();
}

void MetaOs::RetryCard() {
    if (storage_->Ensure(true)) {
        if (!storage_loaded_) {
            reminders_->Load();
            storage_loaded_ = true;
        }
    }
}

void MetaOs::FireReminders() {
    if (!IsClockValid() || reminders_->size() == 0) {
        return;
    }
    auto due = reminders_->TakeDue(time(nullptr));
    if (due.empty()) {
        return;
    }
    for (const auto& reminder : due) {
        ESP_LOGI(TAG, "Reminder %d: %s", reminder.id, reminder.message.c_str());
    }
    // A reminder must be noticed, so it stops local music.
    if (Application::GetInstance().IsLocalPlaybackActive()) {
        music_->StopPlayback();
    }
    if (HasLvglScreen()) {
        panel_->ShowReminderAlert(due);
    } else if (auto display = Board::GetInstance().GetDisplay()) {
        display->ShowNotification(std::string(text::kReminderDue) + ": " + due.front().message,
                                  30000);
    }
    // Spoken reminders need on-device TTS: the XiaoZhi protocol has no way to make the server
    // say a given text (the wake-word "detect" message rejects long text).
    PlayAlertSound(Lang::Sounds::OGG_VIBRATION);
}

void MetaOs::TogglePanel() { panel_->Toggle(); }

void MetaOs::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    // Reminders.
    mcp.AddTool("self.reminder.add",
                "Schedule a reminder. At the given time the device shows it on screen and plays an "
                "alert sound; it does not speak the message. Set either `in_minutes` (relative) or "
                "`at` as \"HH:MM\" (next occurrence, local time) or \"YYYY-MM-DD HH:MM\". `daily` "
                "repeats it every day.",
                PropertyList({Property("message", kPropertyTypeString).SetMaxLength(200),
                              Property("in_minutes", kPropertyTypeInteger, 0, 0, 10080),
                              Property("at", kPropertyTypeString, std::string()).SetMaxLength(16),
                              Property("daily", kPropertyTypeBoolean, false)}),
                [this](const PropertyList& properties) -> ToolResult {
                    return reminders_->Add(properties["message"].value<std::string>(),
                                           properties["in_minutes"].value<int>(),
                                           properties["at"].value<std::string>(),
                                           properties["daily"].value<bool>());
                });
    mcp.AddTool("self.reminder.list", "List pending reminders with their ids.", PropertyList(),
                [this](const PropertyList&) -> ToolResult { return reminders_->List(); });
    mcp.AddTool("self.reminder.cancel", "Cancel a reminder by id.",
                PropertyList({Property("id", kPropertyTypeInteger, 1, 1000000)}),
                [this](const PropertyList& properties) -> ToolResult {
                    return reminders_->Cancel(properties["id"].value<int>());
                });

    // Local music from the SD card.
    mcp.AddTool(
        "self.music.list",
        "List the songs stored on the device's SD card. Optional `query` filters by title.",
        PropertyList({Property("query", kPropertyTypeString, std::string()).SetMaxLength(60)}),
        [this](const PropertyList& properties) -> ToolResult {
            return music_->List(properties["query"].value<std::string>());
        });
    mcp.AddTool(
        "self.music.play",
        "Play songs from the device's SD card. `query` selects the songs whose title contains "
        "it; empty plays all. The conversation ends right after your reply so the music can "
        "play. Use it only when the user asks for local or stored music.",
        PropertyList({Property("query", kPropertyTypeString, std::string()).SetMaxLength(60),
                      Property("shuffle", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& properties) -> ToolResult {
            return music_->Play(properties["query"].value<std::string>(),
                                properties["shuffle"].value<bool>());
        });
    mcp.AddTool("self.music.control",
                "Control local music playback. `action` is \"next\" (skip to the next queued "
                "song) or \"stop\".",
                PropertyList({Property("action", kPropertyTypeString).SetMaxLength(8)}),
                [this](const PropertyList& properties) -> ToolResult {
                    auto action = properties["action"].value<std::string>();
                    if (action == "next") {
                        return music_->Next();
                    }
                    if (action == "stop") {
                        return music_->Stop();
                    }
                    return std::unexpected("Unknown action; use \"next\" or \"stop\"");
                });

    // Storage overview for the LLM and for diagnostics.
    mcp.AddTool(
        "self.storage.get_info",
        "Report the SD card state and MetaOS data: free space, reminders and songs.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            CJsonUniquePtr json(cJSON_CreateObject());
            if (json == nullptr) {
                return std::unexpected("Out of memory");
            }
            bool mounted = storage_->Ensure();
            cJSON_AddBoolToObject(json.get(), "sd_card_mounted", mounted);
            if (!mounted && storage_->state() == CardState::kNoFilesystem) {
                cJSON_AddStringToObject(json.get(), "sd_card_problem",
                                        "not FAT32 (exFAT is not supported)");
            }
            uint64_t total = 0, free = 0;
            if (mounted && storage_->GetUsage(total, free)) {
                cJSON_AddNumberToObject(json.get(), "total_mb", static_cast<double>(total >> 20));
                cJSON_AddNumberToObject(json.get(), "free_mb", static_cast<double>(free >> 20));
            }
            music_->EnsureScanned();
            cJSON_AddNumberToObject(json.get(), "songs",
                                    static_cast<double>(music_->tracks().size()));
            cJSON_AddNumberToObject(json.get(), "pending_reminders",
                                    static_cast<double>(reminders_->size()));
            cJSON_AddNumberToObject(
                json.get(), "free_psram_kb",
                static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            return ReturnValue(std::in_place_type<cJSON*>, json.release());
        });

    // Motion sensor state (if available)
    if (motion_ != nullptr) {
        mcp.AddTool("self.motion.get_state",
                    "Get IMU motion sensor state: accelerometer readings (in milligravity), "
                    "orientation (lying_flat), and motion detection (moving).",
                    PropertyList(), [this](const PropertyList&) -> ToolResult {
                        auto state = motion_->GetState();
                        CJsonUniquePtr json(cJSON_CreateObject());
                        if (json == nullptr) {
                            return std::unexpected("Out of memory");
                        }
                        cJSON_AddNumberToObject(json.get(), "accel_x_mg", state.accel_x_mg);
                        cJSON_AddNumberToObject(json.get(), "accel_y_mg", state.accel_y_mg);
                        cJSON_AddNumberToObject(json.get(), "accel_z_mg", state.accel_z_mg);
                        cJSON_AddBoolToObject(json.get(), "lying_flat", state.lying_flat);
                        cJSON_AddBoolToObject(json.get(), "moving", state.moving);
                        return ReturnValue(std::in_place_type<cJSON*>, json.release());
                    });
    }
}

}  // namespace metaos
