#ifndef METAOS_FILES_H
#define METAOS_FILES_H

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace metaos {

class Storage;

// File entry in the browser
struct FileEntry {
    std::string name;
    bool is_directory;
    size_t size;
    time_t mtime;
};

// File browser and viewer for TF card files.
// Supports:
// - Directory navigation with breadcrumb trail
// - Image viewing (JPG, PNG, BMP, GIF with animation)
// - Text file viewing and editing
// - File operations (delete, rename)
//
// All operations run in the main task.
class FileBrowser {
public:
    struct Callbacks {
        std::function<void()> on_file_changed;  // Called after delete/rename
    };

    explicit FileBrowser(Storage& storage, Callbacks callbacks);
    ~FileBrowser();

    // List files in the given directory. Returns false if the directory cannot be opened.
    bool ListDirectory(const std::string& path, std::vector<FileEntry>& entries);

    // Get the current directory path
    const std::string& current_path() const { return current_path_; }

    // Navigate to a directory (absolute path)
    bool NavigateTo(const std::string& path);

    // Navigate to parent directory
    bool NavigateUp();

    // Get breadcrumb trail (for UI display)
    std::vector<std::string> GetBreadcrumbs() const;

    // File operations
    bool DeleteFile(const std::string& path);
    bool RenameFile(const std::string& old_path, const std::string& new_name);

    // Check file type
    static bool IsImageFile(const std::string& filename);
    static bool IsTextFile(const std::string& filename);
    static bool IsGifFile(const std::string& filename);

    // Format file size for display
    static std::string FormatFileSize(size_t bytes);

    // Format modification time for display
    static std::string FormatFileTime(time_t mtime);

private:
    Storage& storage_;
    Callbacks callbacks_;
    std::string current_path_;
};

}  // namespace metaos

#endif  // METAOS_FILES_H
