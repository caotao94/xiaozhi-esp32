#include "metaos_hermes.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>

#include "application.h"
#include "board.h"
#include "cjson_utils.h"

#define TAG "Hermes"

namespace metaos {

namespace {
constexpr const char* kConfigFile = "hermes.json";
constexpr const char* kCompletionsPath = "/v1/chat/completions";
constexpr const char* kDefaultModel = "hermes-agent";
constexpr size_t kMaxConfigBytes = 2048;
constexpr size_t kMaxTaskBytes = 1000;
constexpr size_t kMaxResultBytes = 12 * 1024;
constexpr size_t kMaxSseLineBytes = 16 * 1024;
constexpr size_t kMaxJsonBodyBytes = 64 * 1024;
constexpr size_t kMaxErrorBodyBytes = 512;
constexpr size_t kStatusResultBytes = 3000;
constexpr size_t kKeptJobs = 5;
// Hermes sends an SSE keepalive comment every 10 seconds, so a silent minute means the
// connection is gone.
constexpr int kReadTimeoutMs = 60 * 1000;
constexpr int64_t kJobDeadlineUs = 15LL * 60 * 1000 * 1000;
// The TLS handshake runs on this task.
constexpr uint32_t kWorkerStackSize = 10240;
constexpr UBaseType_t kWorkerPriority = 3;

constexpr const char* kSystemPrompt =
    "You are called by a voice assistant device on behalf of its user. Complete the task. "
    "Then reply with a short plain-text summary (at most 5 sentences, no Markdown, no code "
    "blocks) that can be read aloud, in the same language as the task. Do not perform "
    "destructive or irreversible actions (deleting data, sending messages or emails, payments, "
    "system changes) unless the task explicitly asks for that exact action.";

const char* StateName(HermesClient::State state) {
    switch (state) {
        case HermesClient::State::kIdle:
            return "idle";
        case HermesClient::State::kRunning:
            return "running";
        case HermesClient::State::kDone:
            return "done";
        case HermesClient::State::kFailed:
            return "failed";
        case HermesClient::State::kCancelled:
            return "cancelled";
    }
    return "unknown";
}

bool StartsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

// Extracts the human-readable message from an OpenAI-style {"error": {...}} body.
std::string ErrorMessage(const cJSON* root) {
    auto error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(error)) {
        return error->valuestring;
    }
    auto message = cJSON_GetObjectItem(error, "message");
    if (cJSON_IsString(message)) {
        return message->valuestring;
    }
    return {};
}
}  // namespace

struct HermesClient::WorkerArgs {
    HermesClient* client;
    int id;
    Config config;
    std::string task;
};

bool HermesClient::LoadConfig(Config& config, std::string& error) const {
    if (!storage_.IsMounted()) {
        error =
            "The SD card is not available, so the Hermes settings (metaos/hermes.json) "
            "cannot be read";
        return false;
    }
    std::string text;
    if (!ReadFile(storage_.DataPath(kConfigFile), text, kMaxConfigBytes)) {
        error = "Hermes is not configured. Create metaos/hermes.json on the SD card.";
        return false;
    }
    CJsonUniquePtr root(cJSON_ParseWithLength(text.data(), text.size()));
    auto url = cJSON_GetObjectItem(root.get(), "url");
    auto api_key = cJSON_GetObjectItem(root.get(), "api_key");
    auto model = cJSON_GetObjectItem(root.get(), "model");
    if (!cJSON_IsString(url) || !cJSON_IsString(api_key) || api_key->valuestring[0] == '\0') {
        error = "metaos/hermes.json must contain \"url\" and \"api_key\" strings";
        return false;
    }
    config.url = Trim(url->valuestring);
    if (!StartsWith(config.url, "http://") && !StartsWith(config.url, "https://")) {
        error = "The Hermes url must start with http:// or https://";
        return false;
    }
    while (!config.url.empty() && config.url.back() == '/') {
        config.url.pop_back();
    }
    std::string path(kCompletionsPath);
    if (config.url.size() < path.size() ||
        config.url.compare(config.url.size() - path.size(), path.size(), path) != 0) {
        if (config.url.size() >= 3 && config.url.compare(config.url.size() - 3, 3, "/v1") == 0) {
            config.url.resize(config.url.size() - 3);
        }
        config.url += path;
    }
    config.api_key = api_key->valuestring;
    config.model =
        cJSON_IsString(model) && model->valuestring[0] != '\0' ? model->valuestring : kDefaultModel;
    return true;
}

bool HermesClient::IsConfigured() const {
    Config config;
    std::string error;
    return LoadConfig(config, error);
}

ToolResult HermesClient::Submit(const std::string& task) {
    std::string text = Trim(task);
    if (text.empty()) {
        return std::unexpected("The task is empty");
    }
    text = Utf8Truncate(text, kMaxTaskBytes);

    storage_.Ensure();
    Config config;
    std::string error;
    if (!LoadConfig(config, error)) {
        return std::unexpected(error);
    }

    int id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return std::unexpected("Hermes is still working on task " +
                                   std::to_string(jobs_.back().id) +
                                   ". Wait for it to finish or cancel it.");
        }
        id = next_id_++;
        Job job;
        job.id = id;
        job.task = text;
        job.started_us = esp_timer_get_time();
        jobs_.push_back(std::move(job));
        while (jobs_.size() > kKeptJobs) {
            jobs_.pop_front();
        }
        running_ = true;
        cancel_requested_ = false;
    }

    auto* args = new WorkerArgs{this, id, std::move(config), text};
    if (xTaskCreate(WorkerEntry, "hermes", kWorkerStackSize, args, kWorkerPriority, nullptr) !=
        pdPASS) {
        delete args;
        FinishJob(id, State::kFailed, "Failed to start the Hermes worker task");
        return std::unexpected("Failed to start the Hermes worker task");
    }

    CJsonUniquePtr json(cJSON_CreateObject());
    if (json == nullptr) {
        return std::unexpected("Out of memory");
    }
    cJSON_AddNumberToObject(json.get(), "task_id", id);
    cJSON_AddStringToObject(json.get(), "state", "running");
    cJSON_AddStringToObject(json.get(), "note",
                            "Hermes works in the background. Tell the user it may take a few "
                            "minutes; the device chimes when the result is ready. Use "
                            "self.hermes.get_status to read it.");
    return ReturnValue(std::in_place_type<cJSON*>, json.release());
}

void HermesClient::WorkerEntry(void* arg) {
    std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs*>(arg));
    args->client->RunJob(args->id, args->config, args->task);
    args.reset();
    vTaskDelete(nullptr);
}

void HermesClient::RunJob(int id, const Config& config, const std::string& task) {
    std::string error;
    bool ok = Stream(id, config, task, error);
    State state = cancel_requested_ ? State::kCancelled : (ok ? State::kDone : State::kFailed);
    if (state == State::kDone) {
        std::lock_guard<std::mutex> lock(mutex_);
        const Job* job = FindLocked(id);
        if (job == nullptr || Trim(job->text).empty()) {
            state = State::kFailed;
            error = "Hermes returned an empty answer";
        }
    }
    FinishJob(id, state, std::move(error));
}

void HermesClient::AppendText(int id, const char* text) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = FindLocked(id);
    if (job == nullptr || job->truncated) {
        return;
    }
    std::string piece(text);
    if (job->text.size() + piece.size() > kMaxResultBytes) {
        job->text += Utf8Truncate(piece, kMaxResultBytes - job->text.size());
        job->truncated = true;
    } else {
        job->text += piece;
    }
    ++revision_;
}

bool HermesClient::Stream(int id, const Config& config, const std::string& task,
                          std::string& error) {
    CJsonUniquePtr request(cJSON_CreateObject());
    if (request == nullptr) {
        error = "Out of memory";
        return false;
    }
    cJSON_AddStringToObject(request.get(), "model", config.model.c_str());
    cJSON_AddBoolToObject(request.get(), "stream", true);
    cJSON* messages = cJSON_AddArrayToObject(request.get(), "messages");
    for (const auto& [role, content] :
         {std::pair<const char*, const char*>{"system", kSystemPrompt},
          std::pair<const char*, const char*>{"user", task.c_str()}}) {
        cJSON* message = cJSON_CreateObject();
        if (messages == nullptr || message == nullptr) {
            cJSON_Delete(message);
            error = "Out of memory";
            return false;
        }
        cJSON_AddStringToObject(message, "role", role);
        cJSON_AddStringToObject(message, "content", content);
        cJSON_AddItemToArray(messages, message);
    }
    CJsonStringUniquePtr body(cJSON_PrintUnformatted(request.get()));
    if (body == nullptr) {
        error = "Out of memory";
        return false;
    }
    request.reset();

    // Connection id 3 is the one other auxiliary HTTP requests use (camera, screen snapshot).
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        error = "Network is not available";
        return false;
    }
    http->SetTimeout(kReadTimeoutMs);
    http->SetHeader("Authorization", "Bearer " + config.api_key);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "text/event-stream");
    http->SetContent(std::string(body.get()));
    body.reset();

    ESP_LOGI(TAG, "Task %d: POST %s", id, config.url.c_str());
    if (auto opened = http->Open("POST", config.url); !opened) {
        error = "Cannot reach Hermes: " + opened.error().ToString();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        error = "Hermes did not answer: " + status.error().ToString();
        http->Close();
        return false;
    }

    const int64_t deadline = esp_timer_get_time() + kJobDeadlineUs;
    std::array<char, 1024> buffer;
    std::string pending;
    auto read_more = [&]() -> int {
        if (cancel_requested_) {
            error = "Cancelled";
            return -1;
        }
        if (esp_timer_get_time() > deadline) {
            error = "Hermes did not finish within 15 minutes";
            return -1;
        }
        auto size = http->Read(buffer.data(), buffer.size());
        if (!size) {
            error = "Connection to Hermes failed: " + size.error().ToString();
            return -1;
        }
        return *size;
    };

    if (*status < 200 || *status >= 300) {
        int size = 0;
        while (pending.size() < kMaxErrorBodyBytes && (size = read_more()) > 0) {
            pending.append(buffer.data(), size);
        }
        CJsonUniquePtr root(cJSON_ParseWithLength(pending.data(), pending.size()));
        std::string message = root ? ErrorMessage(root.get()) : std::string();
        if (message.empty()) {
            message = Utf8Truncate(pending, kMaxErrorBodyBytes);
        }
        error = "Hermes returned HTTP " + std::to_string(*status);
        if (*status == 401 || *status == 403) {
            error += " (check api_key in metaos/hermes.json)";
        }
        if (!message.empty()) {
            error += ": " + message;
        }
        http->Close();
        return false;
    }

    bool event_stream =
        ToLowerAscii(http->GetResponseHeader("Content-Type")).find("text/event-stream") !=
        std::string::npos;
    bool ok = false;
    if (!event_stream) {
        // The server ignored "stream": read one JSON completion.
        int size = 0;
        while ((size = read_more()) > 0) {
            if (pending.size() + size > kMaxJsonBodyBytes) {
                error = "The Hermes answer is too large";
                http->Close();
                return false;
            }
            pending.append(buffer.data(), size);
        }
        if (size == 0) {
            CJsonUniquePtr root(cJSON_ParseWithLength(pending.data(), pending.size()));
            auto choices = cJSON_GetObjectItem(root.get(), "choices");
            auto message = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "message");
            auto content = cJSON_GetObjectItem(message, "content");
            if (cJSON_IsString(content)) {
                AppendText(id, content->valuestring);
                ok = true;
            } else {
                std::string message_text = root ? ErrorMessage(root.get()) : std::string();
                error = message_text.empty() ? "Unexpected answer from Hermes" : message_text;
            }
        }
        http->Close();
        return ok;
    }

    // Server-sent events: "data: {chunk}" lines, blank line ends an event, ":" lines are
    // keepalive comments, and "event: hermes.tool.progress" marks tool progress.
    std::string event_name;
    bool done = false;
    auto handle_line = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            event_name.clear();
            return;
        }
        if (line[0] == ':') {
            return;
        }
        if (StartsWith(line, "event:")) {
            event_name = Trim(line.substr(6));
            return;
        }
        if (!StartsWith(line, "data:")) {
            return;
        }
        std::string data = Trim(line.substr(5));
        if (!event_name.empty() && event_name != "message") {
            if (event_name == "hermes.tool.progress") {
                std::lock_guard<std::mutex> lock(mutex_);
                if (auto job = FindLocked(id)) {
                    ++job->tool_steps;
                    ++revision_;
                }
            }
            return;
        }
        if (data == "[DONE]") {
            done = true;
            return;
        }
        CJsonUniquePtr chunk(cJSON_ParseWithLength(data.data(), data.size()));
        if (chunk == nullptr) {
            return;
        }
        std::string message = ErrorMessage(chunk.get());
        if (!message.empty()) {
            error = message;
            done = true;
            return;
        }
        auto choices = cJSON_GetObjectItem(chunk.get(), "choices");
        auto delta = cJSON_GetObjectItem(cJSON_GetArrayItem(choices, 0), "delta");
        auto content = cJSON_GetObjectItem(delta, "content");
        if (cJSON_IsString(content)) {
            AppendText(id, content->valuestring);
        }
    };

    int size = 0;
    bool line_too_long = false;
    while (!done && (size = read_more()) > 0) {
        size_t start = 0;
        for (int i = 0; i < size; ++i) {
            if (buffer[i] != '\n') {
                continue;
            }
            if (!line_too_long) {
                pending.append(buffer.data() + start, i - start);
                handle_line(std::move(pending));
            }
            pending.clear();
            line_too_long = false;
            start = i + 1;
            if (done) {
                break;
            }
        }
        if (!done && start < static_cast<size_t>(size) && !line_too_long) {
            pending.append(buffer.data() + start, size - start);
            if (pending.size() > kMaxSseLineBytes) {
                // Skip an oversized event line instead of buffering it.
                pending.clear();
                line_too_long = true;
            }
        }
    }
    http->Close();

    if (!error.empty()) {
        return false;
    }
    if (done || size == 0) {
        // A stream that ends without [DONE] still carries a usable answer.
        return true;
    }
    return false;
}

void HermesClient::FinishJob(int id, State state, std::string error) {
    std::string task;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const Job* job = FindLocked(id)) {
            task = job->task;
            text = job->text;
        }
    }

    std::string archive_path;
    if (state == State::kDone && storage_.IsMounted()) {
        std::string name = IsClockValid() ? FormatTime(time(nullptr), "%Y%m%d-%H%M%S") + "-" +
                                                std::to_string(id) + ".md"
                                          : "task-" + std::to_string(id) + ".md";
        archive_path = storage_.DataPath("hermes") + "/" + name;
        std::string content = "# Hermes task " + std::to_string(id) + "\n\n## Task\n\n" + task +
                              "\n\n## Result\n\n" + text + "\n";
        if (!WriteFileReplacing(archive_path, content)) {
            archive_path.clear();
        }
    }

    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        Job* job = FindLocked(id);
        if (job == nullptr) {
            return;
        }
        job->state = state;
        job->finished_us = esp_timer_get_time();
        if (state != State::kDone) {
            // Keep a partial answer visible after a failure, followed by the reason.
            job->text = Trim(job->text).empty() ? error : job->text + "\n\n" + error;
        }
        job->archive_path = std::move(archive_path);
        snapshot = MakeSnapshotLocked(*job, kMaxResultBytes);
    }
    ++revision_;
    ESP_LOGI(TAG, "Task %d %s", id, StateName(state));

    if (finished_callback_) {
        Application::GetInstance().Schedule([this, snapshot]() { finished_callback_(snapshot); });
    }
}

HermesClient::Job* HermesClient::FindLocked(int id) {
    auto it =
        std::find_if(jobs_.begin(), jobs_.end(), [id](const Job& job) { return job.id == id; });
    return it != jobs_.end() ? &*it : nullptr;
}

const HermesClient::Job* HermesClient::FindLocked(int id) const {
    auto it =
        std::find_if(jobs_.begin(), jobs_.end(), [id](const Job& job) { return job.id == id; });
    return it != jobs_.end() ? &*it : nullptr;
}

HermesClient::Snapshot HermesClient::MakeSnapshotLocked(const Job& job,
                                                        size_t max_text_bytes) const {
    Snapshot snapshot;
    snapshot.id = job.id;
    snapshot.state = job.state;
    snapshot.task = job.task;
    int64_t end = job.state == State::kRunning ? esp_timer_get_time() : job.finished_us;
    snapshot.elapsed_seconds = static_cast<int>((end - job.started_us) / 1000000);
    snapshot.tool_steps = job.tool_steps;
    snapshot.text = Utf8Truncate(job.text, max_text_bytes);
    snapshot.truncated = job.truncated || job.text.size() > max_text_bytes;
    snapshot.archive_path = job.archive_path;
    return snapshot;
}

HermesClient::Snapshot HermesClient::GetSnapshot(size_t max_text_bytes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (jobs_.empty()) {
        return {};
    }
    return MakeSnapshotLocked(jobs_.back(), max_text_bytes);
}

bool HermesClient::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

ToolResult HermesClient::GetStatus(int id) const {
    CJsonUniquePtr json(cJSON_CreateObject());
    if (json == nullptr) {
        return std::unexpected("Out of memory");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const Job* job = id > 0 ? FindLocked(id) : (jobs_.empty() ? nullptr : &jobs_.back());
    if (job == nullptr) {
        if (id > 0) {
            return std::unexpected("Unknown task id " + std::to_string(id) +
                                   " (only the last 5 tasks since the last restart are kept)");
        }
        cJSON_AddStringToObject(json.get(), "state", "idle");
        cJSON_AddStringToObject(json.get(), "note", "No Hermes task since the last restart");
        return ReturnValue(std::in_place_type<cJSON*>, json.release());
    }

    Snapshot snapshot = MakeSnapshotLocked(*job, kStatusResultBytes);
    cJSON_AddNumberToObject(json.get(), "task_id", job->id);
    cJSON_AddStringToObject(json.get(), "state", StateName(job->state));
    cJSON_AddStringToObject(json.get(), "task", job->task.c_str());
    cJSON_AddNumberToObject(json.get(), "elapsed_seconds", snapshot.elapsed_seconds);
    if (job->tool_steps > 0) {
        cJSON_AddNumberToObject(json.get(), "tool_steps", job->tool_steps);
    }
    if (job->state == State::kDone) {
        cJSON_AddStringToObject(json.get(), "result", snapshot.text.c_str());
        if (snapshot.truncated) {
            cJSON_AddBoolToObject(json.get(), "result_truncated", true);
        }
        if (!job->archive_path.empty()) {
            cJSON_AddStringToObject(json.get(), "saved_to", job->archive_path.c_str());
        }
    } else if (job->state == State::kRunning) {
        if (!snapshot.text.empty()) {
            cJSON_AddStringToObject(json.get(), "partial_result", snapshot.text.c_str());
        }
    } else if (!snapshot.text.empty()) {
        cJSON_AddStringToObject(json.get(), "error", snapshot.text.c_str());
    }
    return ReturnValue(std::in_place_type<cJSON*>, json.release());
}

ToolResult HermesClient::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return std::string("No Hermes task is running");
    }
    cancel_requested_ = true;
    return std::string("Cancelling task " + std::to_string(jobs_.back().id) +
                       ". Hermes may still finish the work on its side.");
}

}  // namespace metaos
