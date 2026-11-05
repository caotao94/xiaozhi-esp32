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
#include "metaos_hermes.h"
#include "metaos_music.h"
#include "metaos_reminders.h"
#include "metaos_strings.h"
#include "sd_card.h"
#include "settings.h"

#define TAG "MetaOS"

namespace metaos {

namespace {
// NVS keys are persistent API: do not rename them.
constexpr const char* kSettingsNamespace = "metaos";
constexpr const char* kKeySpeakReminders = "speak_remind";
constexpr const char* kKeySpeakHermes = "speak_hermes";
// Drop spoken announcements that could not be delivered within this time.
constexpr int64_t kSpeechExpiryUs = 10LL * 60 * 1000 * 1000;
constexpr size_t kMaxPendingSpeech = 3;

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

MetaOs::MetaOs(SdmmcCard* card) : storage_(std::make_unique<Storage>(card)) {
    reminders_ = std::make_unique<ReminderStore>(*storage_);
    music_ = std::make_unique<MusicPlayer>(*storage_);
    hermes_ = std::make_unique<HermesClient>(*storage_);
    panel_ = std::make_unique<MetaPanel>(
        *storage_, *music_, *reminders_, *hermes_, settings_,
        MetaPanel::Hooks{
            .snooze = [this](const Reminder& reminder,
                             int minutes) { reminders_->Snooze(reminder, minutes); },
            .settings_changed = [this]() { SaveSettings(); },
            .retry_card = [this]() { RetryCard(); },
            .speak_hermes_result = [this]() { SpeakHermesResult(); },
        });
}

MetaOs::~MetaOs() {
    if (tick_timer_ != nullptr) {
        auto timer = static_cast<esp_timer_handle_t>(tick_timer_);
        esp_timer_stop(timer);
        esp_timer_delete(timer);
    }
}

void MetaOs::LoadSettings() {
    Settings settings(kSettingsNamespace, false);
    settings_.speak_reminders = settings.GetBool(kKeySpeakReminders, true);
    settings_.speak_hermes = settings.GetBool(kKeySpeakHermes, true);
}

void MetaOs::SaveSettings() {
    Settings settings(kSettingsNamespace, true);
    settings.SetBool(kKeySpeakReminders, settings_.speak_reminders);
    settings.SetBool(kKeySpeakHermes, settings_.speak_hermes);
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
    panel_->SetHermesConfigured(hermes_->IsConfigured());

    music_->SetFailedCallback([this](const std::string& title) {
        panel_->ShowToast(std::string(text::kPlayFailed) + ": " + title);
    });
    hermes_->SetFinishedCallback([this](const HermesClient::Snapshot& snapshot) {
        if (snapshot.state == HermesClient::State::kCancelled) {
            return;
        }
        bool done = snapshot.state == HermesClient::State::kDone;
        if (HasLvglScreen()) {
            panel_->ShowHermesAlert();
        } else {
            auto display = Board::GetInstance().GetDisplay();
            display->ShowNotification(done ? text::kHermesDone : text::kHermesFailed, 10000);
        }
        if (done && settings_.speak_hermes) {
            QueueSpeech(text::kSpeakHermesPrompt);
        } else {
            PlayAlertSound(done ? Lang::Sounds::OGG_POPUP : Lang::Sounds::OGG_EXCLAMATION);
        }
    });

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
        // Twice a second keeps the music and Hermes pages responsive.
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
    SpeakPending();
    panel_->Refresh();
}

void MetaOs::RetryCard() {
    if (storage_->Ensure(true)) {
        if (!storage_loaded_) {
            reminders_->Load();
            storage_loaded_ = true;
        }
        panel_->SetHermesConfigured(hermes_->IsConfigured());
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
    PlayAlertSound(Lang::Sounds::OGG_VIBRATION);
    if (settings_.speak_reminders) {
        std::string prompt = text::kSpeakReminderPrompt;
        for (size_t i = 0; i < due.size(); ++i) {
            prompt += (i == 0 ? "" : "; ") + due[i].message;
        }
        QueueSpeech(std::move(prompt));
    }
}

void MetaOs::QueueSpeech(std::string prompt) {
    if (pending_speech_.size() >= kMaxPendingSpeech) {
        pending_speech_.erase(pending_speech_.begin());
    }
    pending_speech_.push_back(std::move(prompt));
    speech_queued_us_ = esp_timer_get_time();
    SpeakPending();
}

void MetaOs::SpeakPending() {
    if (pending_speech_.empty()) {
        return;
    }
    if (esp_timer_get_time() - speech_queued_us_ > kSpeechExpiryUs) {
        ESP_LOGW(TAG, "Dropping %u undelivered announcement(s)",
                 static_cast<unsigned>(pending_speech_.size()));
        pending_speech_.clear();
        return;
    }
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle || app.IsLocalPlaybackActive()) {
        return;
    }
    // Combine everything waiting into one request so it becomes one spoken answer.
    std::string prompt;
    for (const auto& item : pending_speech_) {
        prompt += (prompt.empty() ? "" : "\n") + item;
    }
    if (app.InvokeWithText(prompt)) {
        pending_speech_.clear();
    }
}

void MetaOs::SpeakHermesResult() {
    auto snapshot = hermes_->GetSnapshot(0);
    if (snapshot.state != HermesClient::State::kDone) {
        return;
    }
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        panel_->ShowToast(text::kBusy);
        return;
    }
    panel_->Hide();
    if (!app.InvokeWithText(text::kSpeakHermesPrompt)) {
        panel_->ShowToast(text::kBusy);
    }
}

void MetaOs::TogglePanel() {
    if (!panel_->IsOpen()) {
        panel_->SetHermesConfigured(hermes_->IsConfigured());
    }
    panel_->Toggle();
}

void MetaOs::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    // Reminders.
    mcp.AddTool(
        "self.reminder.add",
        "Schedule a reminder. At the given time the device shows it on screen and, unless the "
        "user turned this off, starts a conversation so you can say it aloud, even when no "
        "conversation is open. Set either `in_minutes` (relative) or `at` as \"HH:MM\" (next "
        "occurrence, local time) or \"YYYY-MM-DD HH:MM\". `daily` repeats it every day.",
        PropertyList({Property("message", kPropertyTypeString).SetMaxLength(200),
                      Property("in_minutes", kPropertyTypeInteger, 0, 0, 10080),
                      Property("at", kPropertyTypeString, std::string()).SetMaxLength(16),
                      Property("daily", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& properties) -> ToolResult {
            return reminders_->Add(
                properties["message"].value<std::string>(), properties["in_minutes"].value<int>(),
                properties["at"].value<std::string>(), properties["daily"].value<bool>());
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

    // Hermes Agent bridge.
    mcp.AddTool(
        "self.hermes.submit",
        "Hand a task to Hermes Agent, a more capable assistant running on the user's server "
        "with web research, files, code and other tools. Use it for multi-step work that "
        "takes minutes (research, summaries, planning, computer tasks) or when the user "
        "explicitly asks for Hermes. Describe the full task with all needed context in one "
        "message, in the user's language. It runs in the background. When it finishes, the "
        "device shows the result and starts a conversation asking you to read it.",
        PropertyList({Property("task", kPropertyTypeString).SetMaxLength(1000)}),
        [this](const PropertyList& properties) -> ToolResult {
            return hermes_->Submit(properties["task"].value<std::string>());
        });
    mcp.AddTool("self.hermes.get_status",
                "Get the state and, when done, the result of a Hermes task. `task_id` 0 means the "
                "latest task. While it runs, `partial_result` holds the answer so far. Read the "
                "result to the user in a few sentences.",
                PropertyList({Property("task_id", kPropertyTypeInteger, 0, 0, 1000000)}),
                [this](const PropertyList& properties) -> ToolResult {
                    return hermes_->GetStatus(properties["task_id"].value<int>());
                });
    mcp.AddTool("self.hermes.cancel", "Stop waiting for the running Hermes task.", PropertyList(),
                [this](const PropertyList&) -> ToolResult { return hermes_->Cancel(); });

    // Storage overview for the LLM and for diagnostics.
    mcp.AddTool(
        "self.storage.get_info",
        "Report the SD card state and MetaOS data: free space, reminders, songs and whether "
        "Hermes is configured.",
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
            cJSON_AddBoolToObject(json.get(), "hermes_configured", hermes_->IsConfigured());
            cJSON_AddNumberToObject(
                json.get(), "free_psram_kb",
                static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            return ReturnValue(std::in_place_type<cJSON*>, json.release());
        });
}

}  // namespace metaos
