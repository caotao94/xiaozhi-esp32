#include "metaos_files.h"
#include "metaos_common.h"

#include <esp_log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>

#define TAG "MetaFiles"

namespace metaos {

FileBrowser::FileBrowser(Storage& storage, Callbacks callbacks)
    : storage_(storage), callbacks_(std::move(callbacks)) {
    current_path_ = storage_.MountPoint();
}

FileBrowser::~FileBrowser() = default;

bool FileBrowser::ListDirectory(const std::string& path, std::vector<FileEntry>& entries) {
    entries.clear();

    if (!storage_.IsMounted()) {
        ESP_LOGW(TAG, "Cannot list directory: SD card not mounted");
        return false;
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path.c_str());
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        FileEntry file_entry;
        file_entry.name = entry->d_name;

        // Use d_type when available (most FATFS implementations support it)
        if (entry->d_type == DT_DIR) {
            file_entry.is_directory = true;
            file_entry.size = 0;
            file_entry.mtime = 0;
        } else if (entry->d_type == DT_REG) {
            file_entry.is_directory = false;
            // For files, still stat to get size and mtime (only when needed for display)
            std::string full_path = path + "/" + entry->d_name;
            struct stat st;
            if (stat(full_path.c_str(), &st) == 0) {
                file_entry.size = st.st_size;
                file_entry.mtime = st.st_mtime;
            } else {
                file_entry.size = 0;
                file_entry.mtime = 0;
            }
        } else {
            // Fallback to stat if d_type is DT_UNKNOWN
            std::string full_path = path + "/" + entry->d_name;
            struct stat st;
            if (stat(full_path.c_str(), &st) != 0) {
                ESP_LOGW(TAG, "Failed to stat: %s", full_path.c_str());
                continue;
            }
            file_entry.is_directory = S_ISDIR(st.st_mode);
            file_entry.size = st.st_size;
            file_entry.mtime = st.st_mtime;
        }

        entries.push_back(file_entry);
    }

    closedir(dir);

    // Sort: directories first, then files, alphabetically
    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.is_directory != b.is_directory) {
            return a.is_directory;  // directories first
        }
        return ToLowerAscii(a.name) < ToLowerAscii(b.name);
    });

    ESP_LOGI(TAG, "Listed %d items in %s", static_cast<int>(entries.size()), path.c_str());
    return true;
}

bool FileBrowser::NavigateTo(const std::string& path) {
    if (!storage_.IsMounted()) {
        return false;
    }

    // Verify the path exists and is a directory
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "Invalid directory path: %s", path.c_str());
        return false;
    }

    current_path_ = path;
    ESP_LOGI(TAG, "Navigated to: %s", path.c_str());
    return true;
}

bool FileBrowser::NavigateUp() {
    std::string mount = storage_.MountPoint();
    if (current_path_ == mount || current_path_.length() <= mount.length()) {
        return false;  // Already at root
    }

    size_t last_slash = current_path_.find_last_of('/');
    if (last_slash == std::string::npos || last_slash < mount.length()) {
        current_path_ = mount;
    } else {
        current_path_ = current_path_.substr(0, last_slash);
    }

    ESP_LOGI(TAG, "Navigated up to: %s", current_path_.c_str());
    return true;
}

std::vector<std::string> FileBrowser::GetBreadcrumbs() const {
    std::vector<std::string> crumbs;
    std::string mount = storage_.MountPoint();

    if (current_path_ == mount) {
        crumbs.push_back("TF卡");
        return crumbs;
    }

    crumbs.push_back("TF卡");

    std::string relative = current_path_.substr(mount.length());
    if (!relative.empty() && relative[0] == '/') {
        relative = relative.substr(1);
    }

    size_t start = 0;
    while (start < relative.length()) {
        size_t end = relative.find('/', start);
        if (end == std::string::npos) {
            end = relative.length();
        }
        crumbs.push_back(relative.substr(start, end - start));
        start = end + 1;
    }

    return crumbs;
}

bool FileBrowser::DeleteFile(const std::string& path) {
    if (!storage_.IsMounted()) {
        return false;
    }

    if (unlink(path.c_str()) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", path.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Deleted file: %s", path.c_str());
    if (callbacks_.on_file_changed) {
        callbacks_.on_file_changed();
    }
    return true;
}

bool FileBrowser::RenameFile(const std::string& old_path, const std::string& new_name) {
    if (!storage_.IsMounted()) {
        return false;
    }

    // Extract directory from old_path
    size_t last_slash = old_path.find_last_of('/');
    std::string dir = (last_slash != std::string::npos) ? old_path.substr(0, last_slash) : "";
    std::string new_path = dir + "/" + new_name;

    if (rename(old_path.c_str(), new_path.c_str()) != 0) {
        ESP_LOGE(TAG, "Failed to rename file: %s -> %s", old_path.c_str(), new_path.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Renamed file: %s -> %s", old_path.c_str(), new_path.c_str());
    if (callbacks_.on_file_changed) {
        callbacks_.on_file_changed();
    }
    return true;
}

bool FileBrowser::IsImageFile(const std::string& filename) {
    std::string lower = ToLowerAscii(filename);
    return lower.ends_with(".jpg") || lower.ends_with(".jpeg") || lower.ends_with(".png") ||
           lower.ends_with(".bmp") || lower.ends_with(".gif");
}

bool FileBrowser::IsTextFile(const std::string& filename) {
    std::string lower = ToLowerAscii(filename);
    return lower.ends_with(".txt") || lower.ends_with(".log") || lower.ends_with(".json") ||
           lower.ends_with(".md");
}

bool FileBrowser::IsGifFile(const std::string& filename) {
    std::string lower = ToLowerAscii(filename);
    return lower.ends_with(".gif");
}

std::string FileBrowser::FormatFileSize(size_t bytes) {
    char buffer[32];
    if (bytes < 1024) {
        snprintf(buffer, sizeof(buffer), "%u B", static_cast<unsigned>(bytes));
    } else if (bytes < 1024 * 1024) {
        snprintf(buffer, sizeof(buffer), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buffer, sizeof(buffer), "%.1f MB", bytes / (1024.0 * 1024.0));
    }
    return buffer;
}

std::string FileBrowser::FormatFileTime(time_t mtime) {
    return FormatTime(mtime, "%Y-%m-%d %H:%M");
}

}  // namespace metaos
