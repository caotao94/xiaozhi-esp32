#ifndef METAOS_HERMES_H
#define METAOS_HERMES_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include "mcp_server.h"
#include "metaos_common.h"

namespace metaos {

// Client for a Hermes Agent API server (OpenAI-compatible /v1/chat/completions).
//
// The XiaoZhi LLM calls self.hermes.submit through MCP; a worker task sends the job to Hermes
// with "stream": true and reads the answer as server-sent events. One job runs at a time.
// The last few jobs stay in RAM and finished results are archived to the SD card.
//
// Connection settings come from <sd>/metaos/hermes.json:
//   {"url": "https://hermes.example.com", "api_key": "...", "model": "hermes-agent"}
class HermesClient {
public:
    enum class State { kIdle, kRunning, kDone, kFailed, kCancelled };

    struct Snapshot {
        int id = 0;
        State state = State::kIdle;
        std::string task;
        int elapsed_seconds = 0;
        int tool_steps = 0;
        std::string text;  // the answer so far, the full answer, or the error
        bool truncated = false;
        std::string archive_path;
    };

    // Runs in the main task when a job ends.
    using FinishedCallback = std::function<void(const Snapshot& snapshot)>;

    explicit HermesClient(Storage& storage) : storage_(storage) {}

    void SetFinishedCallback(FinishedCallback callback) {
        finished_callback_ = std::move(callback);
    }

    // Main task only.
    ToolResult Submit(const std::string& task);
    // Any task.
    ToolResult GetStatus(int id) const;
    ToolResult Cancel();
    bool IsRunning() const;
    // Latest job; `max_text_bytes` limits the copied answer text.
    Snapshot GetSnapshot(size_t max_text_bytes) const;
    // Changes whenever a job starts, receives text or ends.
    unsigned revision() const { return revision_.load(); }
    bool IsConfigured() const;

private:
    struct Config {
        std::string url;
        std::string api_key;
        std::string model;
    };

    struct Job {
        int id = 0;
        std::string task;
        int64_t started_us = 0;
        int64_t finished_us = 0;
        State state = State::kRunning;
        int tool_steps = 0;
        std::string text;
        bool truncated = false;
        std::string archive_path;
    };

    struct WorkerArgs;

    bool LoadConfig(Config& config, std::string& error) const;
    static void WorkerEntry(void* arg);
    void RunJob(int id, const Config& config, const std::string& task);
    bool Stream(int id, const Config& config, const std::string& task, std::string& error);
    void AppendText(int id, const char* text);
    void FinishJob(int id, State state, std::string error);
    Job* FindLocked(int id);
    const Job* FindLocked(int id) const;
    Snapshot MakeSnapshotLocked(const Job& job, size_t max_text_bytes) const;

    Storage& storage_;
    FinishedCallback finished_callback_;
    mutable std::mutex mutex_;
    std::deque<Job> jobs_;
    int next_id_ = 1;
    bool running_ = false;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<unsigned> revision_{0};
};

}  // namespace metaos

#endif  // METAOS_HERMES_H
