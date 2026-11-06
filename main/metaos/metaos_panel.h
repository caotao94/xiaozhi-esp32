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

// User settings shown on the settings page. They live in NVS ("metaos" namespace).
struct MetaSettings {
    bool speak_reminders = true;
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
    enum class Page { kHome, kMusic, kReminders, kFiles, kSettings };

    struct Hooks {
        // Main task. Actions that belong to MetaOs.
        std::function<void(const Reminder& reminder, int minutes)> snooze;
        std::function<void()> settings_changed;
        std::function<void()> retry_card;
    };

    struct Button;

    MetaPanel(Storage& storage, MusicPlayer& music, ReminderStore& reminders,
              MetaSettings& settings, Hooks hooks);
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
    void RefreshLocked();
    void RefreshHome();

    // File viewer/editor sub-pages
    // Read-only text viewer above the panel. Closed with the panel.
    void ShowTextViewer(const std::string& path);
    void CloseFileViewer();

    void OpenAlert(const char* icon, const std::string& title, const std::string& body,
                   int timeout_ms);
    void CloseAlertLocked();
    void CloseToast();

    Storage& storage_;
    MusicPlayer& music_;
    ReminderStore& reminders_;
    FileBrowser* file_browser_;
    MetaSettings& settings_;
    Hooks hooks_;

    LvglTheme* theme_ = nullptr;
    // Touch callbacks referenced by LVGL event user data. Freed after their widgets.
    std::vector<Button*> page_buttons_;
    std::vector<Button*> alert_buttons_;
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
    std::string viewing_file_;

    void* alert_ = nullptr;
    void* alert_bar_ = nullptr;
    void* alert_timer_ = nullptr;
    std::vector<Reminder> alert_reminders_;
    void* toast_ = nullptr;
    void* toast_timer_ = nullptr;
};

}  // namespace metaos

#endif  // METAOS_PANEL_H
