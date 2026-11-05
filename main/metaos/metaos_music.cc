#include "metaos_music.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <dirent.h>

#include <algorithm>

#include "application.h"
#include "cjson_utils.h"

#define TAG "MetaOS"

namespace metaos {

namespace {
constexpr size_t kMaxTracks = 300;
constexpr size_t kMaxListed = 30;
// Give up on a queued start if the conversation keeps the device busy this long.
constexpr int64_t kPendingStartTimeoutUs = 60LL * 1000 * 1000;
// If the assistant does not start speaking within this time, end the conversation anyway.
constexpr int64_t kReplyGraceUs = 8LL * 1000 * 1000;
// A track that stops this soon after it started most likely failed to decode.
constexpr int64_t kEarlyFailureUs = 1500LL * 1000;

bool HasOggExtension(const std::string& name) {
    if (name.size() < 5) {
        return false;
    }
    std::string extension = ToLowerAscii(name.substr(name.size() - 4));
    return extension == ".ogg" || extension == ".oga" || extension == "opus";
}

std::string LibraryError(LibraryState state) {
    switch (state) {
        case LibraryState::kNoCard:
            return "The SD card is not available. Insert a FAT32-formatted card and restart the "
                   "device.";
        case LibraryState::kNoFolder:
            return "Cannot open the music folder on the SD card";
        case LibraryState::kEmpty:
            return "No .ogg files in the music folder. Convert songs to mono Ogg Opus on a PC "
                   "and copy them to /music on the SD card.";
        default:
            return "The music library is not available";
    }
}
}  // namespace

std::string MusicPlayer::TitleOf(const std::string& file_name) {
    size_t dot = file_name.find_last_of('.');
    return dot == std::string::npos ? file_name : file_name.substr(0, dot);
}

LibraryState MusicPlayer::Scan() {
    std::vector<std::string> tracks;
    LibraryState state = LibraryState::kOk;
    if (!storage_.Ensure()) {
        state = LibraryState::kNoCard;
    } else if (DIR* dir = opendir(storage_.MusicPath().c_str())) {
        while (struct dirent* entry = readdir(dir)) {
            std::string name = entry->d_name;
            if (name.empty() || name[0] == '.' || !HasOggExtension(name)) {
                continue;
            }
            tracks.push_back(std::move(name));
            if (tracks.size() >= kMaxTracks) {
                break;
            }
        }
        closedir(dir);
        std::sort(tracks.begin(), tracks.end());
        state = tracks.empty() ? LibraryState::kEmpty : LibraryState::kOk;
    } else {
        state = LibraryState::kNoFolder;
    }
    if (tracks != tracks_) {
        // Queue indexes refer to the old list. The playing track keeps playing.
        tracks_ = std::move(tracks);
        order_.clear();
        position_ = 0;
    }
    library_state_ = state;
    ++revision_;
    return state;
}

LibraryState MusicPlayer::Rescan() { return Scan(); }

void MusicPlayer::EnsureScanned() {
    if (library_state_ == LibraryState::kUnknown ||
        (library_state_ == LibraryState::kNoCard && storage_.IsMounted())) {
        Scan();
    }
}

void MusicPlayer::BuildOrder(size_t first) {
    order_.clear();
    size_t count = tracks_.size();
    for (size_t i = 0; i < count; ++i) {
        order_.push_back((first + i) % count);
    }
    if (shuffle_) {
        // Keep the chosen track first and shuffle the rest (Fisher-Yates over 1..n-1).
        for (size_t i = order_.size() > 0 ? order_.size() - 1 : 0; i > 1; --i) {
            size_t j = 1 + esp_random() % i;
            std::swap(order_[i], order_[j]);
        }
    }
    position_ = 0;
}

void MusicPlayer::Queue(bool after_reply) {
    ++generation_;
    current_title_.clear();
    Application::GetInstance().StopLocalPlayback();
    pending_start_ = true;
    pending_saw_reply_ = !after_reply;
    pending_since_us_ = esp_timer_get_time();
    ++revision_;
    Tick();
}

ToolResult MusicPlayer::List(const std::string& query) {
    LibraryState state = Scan();
    if (state != LibraryState::kOk) {
        return std::unexpected(LibraryError(state));
    }
    std::string lowered = ToLowerAscii(Trim(query));
    CJsonUniquePtr json(cJSON_CreateObject());
    cJSON* titles = json ? cJSON_AddArrayToObject(json.get(), "tracks") : nullptr;
    if (titles == nullptr) {
        return std::unexpected("Out of memory");
    }
    int matched = 0;
    for (const auto& track : tracks_) {
        if (!lowered.empty() && ToLowerAscii(track).find(lowered) == std::string::npos) {
            continue;
        }
        ++matched;
        if (cJSON_GetArraySize(titles) < static_cast<int>(kMaxListed)) {
            cJSON_AddItemToArray(titles, cJSON_CreateString(TitleOf(track).c_str()));
        }
    }
    cJSON_AddNumberToObject(json.get(), "matched", matched);
    cJSON_AddNumberToObject(json.get(), "total", static_cast<double>(tracks_.size()));
    if (matched > static_cast<int>(kMaxListed)) {
        cJSON_AddStringToObject(json.get(), "note", "Only the first 30 matches are listed");
    }
    return ReturnValue(std::in_place_type<cJSON*>, json.release());
}

ToolResult MusicPlayer::Play(const std::string& query, bool shuffle) {
    LibraryState state = Scan();
    if (state != LibraryState::kOk) {
        return std::unexpected(LibraryError(state));
    }
    std::string lowered = ToLowerAscii(Trim(query));
    std::vector<size_t> matches;
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (lowered.empty() || ToLowerAscii(tracks_[i]).find(lowered) != std::string::npos) {
            matches.push_back(i);
        }
    }
    if (matches.empty()) {
        return std::unexpected("No track matches \"" + Trim(query) +
                               "\". Use self.music.list to see the titles.");
    }
    if (shuffle) {
        for (size_t i = matches.size(); i > 1; --i) {
            std::swap(matches[i - 1], matches[esp_random() % i]);
        }
    }
    order_ = std::move(matches);
    position_ = 0;
    // The tool runs while the assistant is still answering; Tick() starts playback once the
    // reply has finished and the conversation has ended.
    Queue(true);

    CJsonUniquePtr json(cJSON_CreateObject());
    if (json == nullptr) {
        return std::unexpected("Out of memory");
    }
    cJSON_AddStringToObject(json.get(), "first_track", TitleOf(tracks_[order_[0]]).c_str());
    cJSON_AddNumberToObject(json.get(), "queued_tracks", static_cast<double>(order_.size()));
    cJSON_AddStringToObject(json.get(), "note",
                            "Say one short sentence. The conversation then ends and the music "
                            "starts. The wake word or the button stops the music.");
    return ReturnValue(std::in_place_type<cJSON*>, json.release());
}

ToolResult MusicPlayer::Next() {
    if (order_.empty() || position_ + 1 >= order_.size()) {
        return std::unexpected("There is no next track in the queue");
    }
    ++position_;
    Queue(true);
    return std::string("Next: " + TitleOf(tracks_[order_[position_]]));
}

ToolResult MusicPlayer::Stop() {
    StopPlayback();
    return std::string("Music stopped");
}

void MusicPlayer::PlayFrom(size_t track) {
    if (library_state_ != LibraryState::kOk || track >= tracks_.size()) {
        return;
    }
    BuildOrder(track);
    Queue(false);
}

void MusicPlayer::Resume() {
    if (order_.empty() || position_ >= order_.size()) {
        if (library_state_ != LibraryState::kOk || tracks_.empty()) {
            return;
        }
        BuildOrder(0);
    }
    Queue(false);
}

void MusicPlayer::SkipNext() {
    if (order_.empty()) {
        return;
    }
    position_ = position_ + 1 < order_.size() ? position_ + 1 : 0;
    Queue(false);
}

void MusicPlayer::SkipPrevious() {
    if (order_.empty()) {
        return;
    }
    if (position_ > 0) {
        --position_;
    }
    Queue(false);
}

void MusicPlayer::StopPlayback() {
    ++generation_;
    pending_start_ = false;
    current_title_.clear();
    Application::GetInstance().StopLocalPlayback();
    ++revision_;
}

void MusicPlayer::SetShuffle(bool shuffle) {
    shuffle_ = shuffle;
    ++revision_;
}

bool MusicPlayer::PlayIndex(size_t index) {
    if (index >= order_.size() || order_[index] >= tracks_.size()) {
        return false;
    }
    const std::string& file = tracks_[order_[index]];
    std::string title = TitleOf(file);
    unsigned generation = ++generation_;
    bool started = Application::GetInstance().StartLocalPlayback(
        storage_.MusicPath() + "/" + file, title,
        [this, generation](bool completed) { OnFinished(completed, generation); });
    current_title_ = started ? title : std::string();
    started_us_ = esp_timer_get_time();
    if (started) {
        ESP_LOGI(TAG, "Playing %s", file.c_str());
    } else if (failed_) {
        failed_(title);
    }
    ++revision_;
    return started;
}

void MusicPlayer::OnFinished(bool completed, unsigned generation) {
    if (generation != generation_) {
        return;
    }
    std::string title = std::move(current_title_);
    current_title_.clear();
    ++revision_;
    if (!completed && esp_timer_get_time() - started_us_ < kEarlyFailureUs) {
        ESP_LOGW(TAG, "Playback of %s failed", title.c_str());
        if (failed_) {
            failed_(title);
        }
        return;
    }
    if (completed && position_ + 1 < order_.size()) {
        // Start the next track once StopNotification() has returned the device to idle.
        ++position_;
        pending_start_ = true;
        pending_saw_reply_ = true;
        pending_since_us_ = esp_timer_get_time();
        Tick();
    }
}

void MusicPlayer::Tick() {
    if (!pending_start_) {
        return;
    }
    auto& app = Application::GetInstance();
    if (app.CanStartLocalPlayback()) {
        pending_start_ = false;
        PlayIndex(position_);
        return;
    }
    int64_t waited_us = esp_timer_get_time() - pending_since_us_;
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        pending_saw_reply_ = true;
    } else if (pending_saw_reply_ || waited_us > kReplyGraceUs) {
        // The assistant has answered, so end the conversation and let the music play.
        app.EndConversation();
    }
    if (waited_us > kPendingStartTimeoutUs) {
        ESP_LOGW(TAG, "Music start dropped: the device stayed busy");
        pending_start_ = false;
        ++revision_;
    }
}

bool MusicPlayer::IsPlaying() const {
    return !current_title_.empty() && Application::GetInstance().IsLocalPlaybackActive();
}

int MusicPlayer::current_track() const {
    if ((!IsPlaying() && !pending_start_) || position_ >= order_.size()) {
        return -1;
    }
    return static_cast<int>(order_[position_]);
}

std::string MusicPlayer::queued_title() const {
    if (!current_title_.empty()) {
        return current_title_;
    }
    if (position_ < order_.size() && order_[position_] < tracks_.size()) {
        return TitleOf(tracks_[order_[position_]]);
    }
    return {};
}

}  // namespace metaos
