#ifndef METAOS_MUSIC_H
#define METAOS_MUSIC_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mcp_server.h"
#include "metaos_common.h"

namespace metaos {

enum class LibraryState {
    kUnknown,  // not scanned yet
    kOk,
    kNoCard,
    kNoFolder,  // the card is mounted but /music cannot be opened
    kEmpty,     // /music holds no .ogg files
};

// Plays mono Ogg Opus files from <sd>/music. Playback goes through the notification path, so
// the wake word or the BOOT button stops it. Main task only.
//
// Convert music on a PC first, for example:
//   ffmpeg -i song.mp3 -c:a libopus -b:a 32k -ac 1 -ar 24000 -frame_duration 60 song.ogg
class MusicPlayer {
public:
    explicit MusicPlayer(Storage& storage) : storage_(storage) {}

    // Called with the track title when a track cannot be played.
    void SetFailedCallback(std::function<void(const std::string&)> callback) {
        failed_ = std::move(callback);
    }

    // MCP tools. They run during a conversation, so playback starts after the reply.
    ToolResult List(const std::string& query);
    ToolResult Play(const std::string& query, bool shuffle);
    ToolResult Next();
    ToolResult Stop();

    // Touch controls. Playback starts right away unless a conversation is running.
    LibraryState Rescan();
    // Scans once, or again after the card has been mounted.
    void EnsureScanned();
    void PlayFrom(size_t track);
    // Plays the queued track again, or the first track when nothing is queued.
    void Resume();
    void SkipNext();
    void SkipPrevious();
    void StopPlayback();
    void SetShuffle(bool shuffle);
    bool shuffle() const { return shuffle_; }

    // Main task, every second: starts queued playback once the device is idle.
    void Tick();

    bool IsPlaying() const;
    bool IsPending() const { return pending_start_; }
    // Index into tracks() of the playing or queued track, or -1.
    int current_track() const;
    // Title of the playing track, or of the track the play button would start.
    std::string queued_title() const;
    const std::vector<std::string>& tracks() const { return tracks_; }
    LibraryState library_state() const { return library_state_; }
    // Changes whenever the library, the queue or the playback state changes.
    unsigned revision() const { return revision_; }
    static std::string TitleOf(const std::string& file_name);

private:
    LibraryState Scan();
    void BuildOrder(size_t first);
    void Queue(bool after_reply);
    bool PlayIndex(size_t index);
    void OnFinished(bool completed, unsigned generation);

    Storage& storage_;
    std::function<void(const std::string&)> failed_;
    LibraryState library_state_ = LibraryState::kUnknown;
    std::vector<std::string> tracks_;  // file names relative to the music folder, sorted
    std::vector<size_t> order_;        // play order, indexes into tracks_
    size_t position_ = 0;              // index into order_
    std::string current_title_;
    bool shuffle_ = false;
    unsigned generation_ = 0;
    unsigned revision_ = 0;
    bool pending_start_ = false;
    bool pending_saw_reply_ = false;  // the assistant spoke since playback was requested
    int64_t pending_since_us_ = 0;
    int64_t started_us_ = 0;
};

}  // namespace metaos

#endif  // METAOS_MUSIC_H
