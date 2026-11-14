#ifndef METAOS_PANEL_H
#define METAOS_PANEL_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "metaos_reminders.h"

class LvglTheme;

namespace metaos {

class MusicPlayer;
class Storage;
class FileBrowser;
class FirmwareManager;

// User settings shown on the settings page. They live in NVS ("metaos" namespace).
struct MetaSettings {
    bool speak_reminders = true;
    bool shake_next = false;
    bool flip_stop = false;
};

// Touch UI on the LVGL top layer: a home screen with four tiles and one page each for music,
// reminders, files and settings, plus alert dialogs for reminders.
//
// Widgets exist only while a screen is open, so a later font or theme change never leaves
// them with a stale font pointer. Every public method runs in the main task and takes the
// LVGL lock itself. LVGL touch handlers run in the LVGL task; they only schedule work onto
// the main task.
//
// Without LVGL (OLED, e-paper, emote display) every method is a no-op.
class MetaPanel {
public:
    enum class Page { kHome, kMusic, kReminders, kFiles, kSettings, kImage, kText, kFirmware };

    struct Hooks {
        // Main task. Actions that belong to MetaOs.
        std::function<void(const Reminder& reminder, int minutes)> snooze;
        std::function<void()> settings_changed;
        std::function<void()> retry_card;
        // USB MSC (TF card as U-disk). available() gates the settings switch; active() is its
        // state; set() requests entering/leaving USB mode. Empty when the feature is off.
        std::function<bool()> usb_msc_available;
        std::function<bool()> usb_msc_active;
        std::function<void(bool)> usb_msc_set;
    };

    struct Button;

    MetaPanel(Storage& storage, MusicPlayer& music, ReminderStore& reminders,
              MetaSettings& settings, FirmwareManager& firmware, Hooks hooks);
    ~MetaPanel();

    MetaPanel(const MetaPanel&) = delete;
    MetaPanel& operator=(const MetaPanel&) = delete;

    void Toggle();
    void Open(Page page);
    void Hide();
    bool IsOpen() const { return root_ != nullptr; }
    // Main task, about twice a second: refreshes what is on screen.
    void Refresh();

    // Alert dialogs above everything else. They stay until a button is tapped or they
    // time out (conversations don't auto-close them).
    void ShowReminderAlert(const std::vector<Reminder>& due);
    void CloseAlert();
    bool IsAlertOpen() const { return alert_ != nullptr; }
    // Short message at the bottom of the screen, also shown without the panel.
    void ShowToast(const std::string& text);

private:
    // Helpers below expect the LVGL lock to be held and theme_ to be set.
    Button* NewButton(std::vector<Button*>& owner, std::function<void()> on_tap);
    static void FreeButtons(std::vector<Button*>& owner);
    void AttachTap(void* obj, std::function<void()> on_tap);
    void* AddLabel(void* parent, const std::string& text, bool secondary = false);
    void* AddIconButton(void* parent, const char* icon, std::function<void()> on_tap,
                        bool accent = false);
    void* AddTextButton(void* parent, const std::string& text, std::function<void()> on_tap,
                        bool accent = false);
    void* AddHeader(const char* icon, const std::string& title);
    void* AddScrollArea();
    void* AddListRow(void* list, const char* icon, const std::string& text,
                     const std::string& detail, std::function<void()> on_tap,
                     bool highlighted = false);
    void* AddButtonBar();
    void AddSwitchRow(void* parent, const std::string& text, bool value,
                      std::function<void(bool)> on_change);
    void AddSliderRow(void* parent, const char* icon, int value,
                      std::function<void(int)> on_change);
    static void SetLabel(void* label, const std::string& text);

    void CloseLocked();
    void ClearPageRefs();
    void BuildHome();
    void BuildMusic();
    void BuildReminders();
    void BuildFiles();
    void BuildSettings();
    void BuildFirmware();
    void RefreshLocked();
    void RefreshHome();
    void RefreshFirmware();

    // File viewer/editor sub-pages
    void ShowImageViewer(const std::string& path);
    void ShowTextViewer(const std::string& path);
    void CloseFileViewer();
    void OnImageDecoded(void* decoded_image, const std::string& path);
    // 文本分页：工作线程读取一页，主任务更新界面。工作队列满时返回 false。
    bool LoadTextPage(long offset);
    // 刷新位置指示与上一页/下一页按钮状态。需持 LVGL 锁。
    void UpdateTextPager();

    void OpenAlert(const char* icon, const std::string& title, const std::string& body,
                   int timeout_ms);
    void CloseAlertLocked();
    void CloseToast();

    Storage& storage_;
    MusicPlayer& music_;
    ReminderStore& reminders_;
    FileBrowser* file_browser_ = nullptr;
    MetaSettings& settings_;
    FirmwareManager& firmware_;
    Hooks hooks_;

    // Firmware page state (main task only).
    void* fw_status_area_ = nullptr;
    void* fw_list_area_ = nullptr;
    void* fw_progress_bar_ = nullptr;
    void* fw_progress_label_ = nullptr;
    void* fw_cancel_button_ = nullptr;
    int fw_selected_index_ = -1;

    LvglTheme* theme_ = nullptr;
    // Touch callbacks referenced by LVGL event user data. Freed after their widgets.
    std::vector<Button*> page_buttons_;
    std::vector<Button*> alert_buttons_;
    // 文件查看器（文本/图片）的按钮单独存放，CloseFileViewer 时立即释放，
    // 避免在文件页内反复开合查看器时累积到 page_buttons_（后者仅在切页时释放）。
    std::vector<Button*> viewer_buttons_;
    std::vector<Button*>* building_buttons_ = &page_buttons_;

    Page page_ = Page::kHome;
    void* root_ = nullptr;
    // Widgets that Refresh() updates on the current page.
    void* list_area_ = nullptr;
    void* tile_labels_[4] = {};
    unsigned shown_revision_ = 0;
    size_t list_offset_ = 0;

    // File viewer state
    void* file_viewer_ = nullptr;
    void* file_viewer_content_ = nullptr;  // Image or text content widget
    std::string viewing_file_;
    void* loading_spinner_ = nullptr;
    void* gif_player_ = nullptr;  // LvglGif* when viewing a GIF
    void* image_dsc_ = nullptr;   // lv_img_dsc_t* for static images, needs manual cleanup

    // 文本分页状态（仅主任务访问）。路径复用 viewing_file_；页内容只保存在 label 里，
    // 不随文件增大而增加内存。翻页历史用于"上一页"。
    long text_offset_ = 0;            // 当前页起始字节偏移
    long text_next_offset_ = 0;       // 下一页起始偏移（末页时等于 text_size_）
    long text_size_ = 0;              // 文件总字节数
    std::vector<long> text_history_;  // 之前各页的起始偏移
    unsigned text_session_ = 0;       // 每次打开递增，用于丢弃过期的翻页结果
    bool text_loading_ = false;       // 有翻页读取正在进行
    void* text_label_ = nullptr;
    void* text_prev_button_ = nullptr;
    void* text_next_button_ = nullptr;
    void* text_position_label_ = nullptr;

    void* alert_ = nullptr;
    void* alert_bar_ = nullptr;
    void* alert_timer_ = nullptr;
    std::vector<Reminder> alert_reminders_;
    void* toast_ = nullptr;
    void* toast_timer_ = nullptr;
};

}  // namespace metaos

#endif  // METAOS_PANEL_H
