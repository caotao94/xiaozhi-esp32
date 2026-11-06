#include "metaos_common.h"

#include <esp_log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <memory>

#include "sd_card.h"

#define TAG "MetaOS"

namespace metaos {

namespace {
constexpr const char* kDataFolder = "metaos";
constexpr const char* kMusicFolder = "music";
// 2024-01-01T00:00:00Z. Anything earlier means the clock was never set.
constexpr time_t kClockValidAfter = 1704067200;

struct FileCloser {
    void operator()(FILE* file) const {
        if (file != nullptr) {
            fclose(file);
        }
    }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

bool EnsureFolder(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(path.c_str(), 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGW(TAG, "Failed to create %s (errno %d)", path.c_str(), errno);
    return false;
}

bool ReadFileExact(const std::string& path, std::string& out, size_t max_bytes) {
    FilePtr file(fopen(path.c_str(), "rb"));
    if (!file) {
        return false;
    }
    out.clear();
    char buffer[512];
    while (out.size() < max_bytes) {
        size_t want = std::min(sizeof(buffer), max_bytes - out.size());
        size_t got = fread(buffer, 1, want, file.get());
        if (got == 0) {
            break;
        }
        out.append(buffer, got);
    }
    return !ferror(file.get());
}
}  // namespace

bool Storage::Ensure(bool force) {
    if (card_ == nullptr || !card_->Mount(force)) {
        return false;
    }
    if (!folders_ready_) {
        folders_ready_ =
            EnsureFolder(card_->mount_point() + "/" + kDataFolder) && EnsureFolder(MusicPath());
    }
    return folders_ready_;
}

bool Storage::IsMounted() const { return card_ != nullptr && card_->IsMounted(); }

CardState Storage::state() const {
    if (IsMounted()) {
        return CardState::kMounted;
    }
    if (card_ != nullptr && card_->last_error() == ESP_FAIL) {
        return CardState::kNoFilesystem;
    }
    return CardState::kMissing;
}

std::string Storage::DataPath(const char* name) const {
    return MountPoint() + "/" + kDataFolder + "/" + name;
}

std::string Storage::MusicPath() const { return MountPoint() + "/" + kMusicFolder; }

std::string Storage::MountPoint() const {
    return card_ != nullptr ? card_->mount_point() : std::string();
}

bool Storage::GetUsage(uint64_t& total_bytes, uint64_t& free_bytes) const {
    return card_ != nullptr && card_->GetUsage(total_bytes, free_bytes);
}

size_t Utf8PrefixLength(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text.size();
    }
    size_t length = max_bytes;
    // text[length] is the first excluded byte. While it is a continuation byte, the character
    // it belongs to would be cut, so drop that character entirely.
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
        --length;
    }
    return length;
}

std::string Utf8Truncate(const std::string& text, size_t max_bytes) {
    return text.substr(0, Utf8PrefixLength(text, max_bytes));
}

std::string Trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    });
    return text;
}

bool ReadFile(const std::string& path, std::string& out, size_t max_bytes) {
    if (ReadFileExact(path, out, max_bytes)) {
        return true;
    }
    return ReadFileExact(path + ".tmp", out, max_bytes);
}

bool WriteFileReplacing(const std::string& path, const std::string& content) {
    std::string temp_path = path + ".tmp";
    {
        FilePtr file(fopen(temp_path.c_str(), "wb"));
        if (!file) {
            ESP_LOGW(TAG, "Failed to open %s for writing", temp_path.c_str());
            return false;
        }
        if (fwrite(content.data(), 1, content.size(), file.get()) != content.size() ||
            fflush(file.get()) != 0 || fsync(fileno(file.get())) != 0) {
            ESP_LOGW(TAG, "Failed to write %s", temp_path.c_str());
            return false;
        }
    }
    // FAT rename() does not replace an existing file, so remove the old one first.
    // ReadFile() falls back to the temporary file if power is lost in between.
    unlink(path.c_str());
    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        ESP_LOGW(TAG, "Failed to rename %s (errno %d)", temp_path.c_str(), errno);
        return false;
    }
    return true;
}

bool IsClockValid() { return time(nullptr) > kClockValidAfter; }

std::string FormatTime(time_t value, const char* format) {
    struct tm local = {};
    localtime_r(&value, &local);
    char buffer[32];
    size_t length = strftime(buffer, sizeof(buffer), format, &local);
    return std::string(buffer, length);
}

}  // namespace metaos
