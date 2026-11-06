#ifndef METAOS_COMMON_H
#define METAOS_COMMON_H

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

class SdmmcCard;

namespace metaos {

enum class CardState {
    kMounted,
    kMissing,       // no card, or the card does not answer
    kNoFilesystem,  // the card answered but is not FAT (usually exFAT)
};

// Where MetaOS keeps its data on the SD card.
//   <mount>/metaos/reminders.json pending reminders
//   <mount>/music/                Ogg Opus files for local playback
class Storage {
public:
    explicit Storage(SdmmcCard* card) : card_(card) {}

    // Main task only: mounts the card on demand and creates the MetaOS folders. `force`
    // skips the retry rate limit (for an explicit user retry).
    bool Ensure(bool force = false);
    // Any task.
    bool IsMounted() const;
    CardState state() const;
    // Absolute path below the MetaOS data folder, e.g. DataPath("reminders.json").
    std::string DataPath(const char* name) const;
    std::string MusicPath() const;
    std::string MountPoint() const;
    bool GetUsage(uint64_t& total_bytes, uint64_t& free_bytes) const;

private:
    SdmmcCard* card_;
    bool folders_ready_ = false;
};

// Length of the longest prefix of `text` that fits in `max_bytes` without splitting a UTF-8
// sequence.
size_t Utf8PrefixLength(const std::string& text, size_t max_bytes);
std::string Utf8Truncate(const std::string& text, size_t max_bytes);
std::string Trim(const std::string& text);
// Lowercases ASCII letters only; other bytes (including UTF-8) are kept as they are.
std::string ToLowerAscii(std::string text);

// Reads at most `max_bytes` (the file head). Falls back to "<path>.tmp" when the file itself
// is missing, which happens if power was lost during WriteFileReplacing().
bool ReadFile(const std::string& path, std::string& out, size_t max_bytes);
// Replaces the file contents via a temporary file so an interrupted write never leaves a
// truncated file behind.
bool WriteFileReplacing(const std::string& path, const std::string& content);

// True once the clock was set from the server (the OTA check does this at startup).
bool IsClockValid();
std::string FormatTime(time_t value, const char* format);

}  // namespace metaos

#endif  // METAOS_COMMON_H
