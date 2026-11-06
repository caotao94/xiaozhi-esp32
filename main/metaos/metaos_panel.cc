#include "metaos_panel.h"

#include <sdkconfig.h>

#include "display.h"
#include "metaos_reminders.h"

#if HAVE_LVGL && !CONFIG_USE_EMOTE_MESSAGE_STYLE
#define METAOS_PANEL_ENABLED 1
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <material_symbols.h>

#include <algorithm>
#include <cstdio>

#include "application.h"
#include "board.h"
#include "display/lvgl_display/gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "metaos_common.h"
#include "metaos_files.h"
#include "metaos_image.h"
#include "metaos_music.h"
#include "metaos_strings.h"
#include "metaos_worker.h"
#else
#define METAOS_PANEL_ENABLED 0
#endif

namespace metaos {

#if METAOS_PANEL_ENABLED

#define TAG "MetaPanel"

// A touch callback. LVGL keeps a pointer to it as event user data, so it must outlive its
// widget; MetaPanel owns these and deletes them only after deleting the widgets.
struct MetaPanel::Button {
    std::function<void()> on_tap;
};

namespace {
constexpr int kLockTimeoutMs = 1000;
constexpr int kToastMs = 2500;
constexpr int kReminderAlertMs = 3 * 60 * 1000;
constexpr size_t kTracksPerPage = 30;

class LvglLock {
public:
    LvglLock() : locked_(lvgl_port_lock(kLockTimeoutMs)) {
        if (!locked_) {
            ESP_LOGW(TAG, "Failed to lock LVGL");
        }
    }
    ~LvglLock() {
        if (locked_) {
            lvgl_port_unlock();
        }
    }
    explicit operator bool() const { return locked_; }

private:
    bool locked_;
};

lv_obj_t* Obj(void* pointer) { return static_cast<lv_obj_t*>(pointer); }

lv_color_t Accent() { return lv_color_hex(0x1E88E5); }
lv_color_t Pressed() { return lv_color_hex(0x90CAF9); }

LvglTheme* CurrentTheme() {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr || !display->SupportsGuiOperations()) {
        return nullptr;
    }
    auto theme = static_cast<LvglTheme*>(display->GetTheme());
    if (theme == nullptr || theme->text_font() == nullptr || theme->icon_font() == nullptr) {
        return nullptr;
    }
    return theme;
}

bool IsDark(LvglTheme* theme) { return lv_color_brightness(theme->background_color()) < 128; }

lv_color_t CardColor(LvglTheme* theme) {
    return IsDark(theme) ? lv_color_hex(0x2A2A2A) : lv_color_hex(0xEDEDED);
}

lv_color_t SecondaryText(LvglTheme* theme) {
    return IsDark(theme) ? lv_color_hex(0xA0A0A0) : lv_color_hex(0x606060);
}

// A plain container: no border, no padding, transparent, not scrollable, not clickable.
lv_obj_t* Box(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box,
                       static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return box;
}

// "08:30" today, "+1 08:30" tomorrow, "11-07 08:30" later.
std::string FormatDue(time_t due) {
    time_t now = time(nullptr);
    struct tm due_tm = {};
    struct tm now_tm = {};
    localtime_r(&due, &due_tm);
    localtime_r(&now, &now_tm);
    std::string clock = FormatTime(due, "%H:%M");
    if (due_tm.tm_year == now_tm.tm_year && due_tm.tm_yday == now_tm.tm_yday) {
        return clock;
    }
    time_t tomorrow = now + 24 * 60 * 60;
    struct tm tomorrow_tm = {};
    localtime_r(&tomorrow, &tomorrow_tm);
    if (due_tm.tm_year == tomorrow_tm.tm_year && due_tm.tm_yday == tomorrow_tm.tm_yday) {
        return "+1 " + clock;
    }
    return FormatTime(due, "%m-%d ") + clock;
}

std::string FormatUsage(uint64_t total, uint64_t free) {
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "%s  %.1f / %.1f GB", text::kStorage,
             (total - free) / 1073741824.0, total / 1073741824.0);
    return buffer;
}

void DispatchTap(lv_event_t* event) {
    auto* button = static_cast<MetaPanel::Button*>(lv_event_get_user_data(event));
    if (button == nullptr || !button->on_tap) {
        return;
    }
    // Copy the callback: the main task may rebuild the page and delete `button` before the
    // scheduled call runs.
    auto callback = button->on_tap;
    Application::GetInstance().Schedule([callback = std::move(callback)]() { callback(); });
}
}  // namespace

MetaPanel::MetaPanel(Storage& storage, MusicPlayer& music, ReminderStore& reminders,
                     MetaSettings& settings, Hooks hooks)
    : storage_(storage),
      music_(music),
      reminders_(reminders),
      file_browser_(
          new FileBrowser(storage, {.on_file_changed = [this]() { Open(Page::kFiles); }})),
      settings_(settings),
      hooks_(std::move(hooks)) {}

MetaPanel::~MetaPanel() {
    delete file_browser_;
    LvglLock lock;
    if (lock) {
        CloseToast();
        CloseAlertLocked();
        CloseLocked();
    }
}

MetaPanel::Button* MetaPanel::NewButton(std::vector<Button*>& owner, std::function<void()> on_tap) {
    auto* button = new Button{std::move(on_tap)};
    owner.push_back(button);
    return button;
}

void MetaPanel::FreeButtons(std::vector<Button*>& owner) {
    for (auto* button : owner) {
        delete button;
    }
    owner.clear();
}

void MetaPanel::AttachTap(void* obj, std::function<void()> on_tap) {
    Button* button = NewButton(*building_buttons_, std::move(on_tap));
    lv_obj_add_flag(Obj(obj), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(Obj(obj), DispatchTap, LV_EVENT_CLICKED, button);
}

void* MetaPanel::AddLabel(void* parent, const std::string& text, bool secondary) {
    lv_obj_t* label = lv_label_create(Obj(parent));
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(label, text.c_str());
    if (secondary) {
        lv_obj_set_style_text_color(label, SecondaryText(theme_), 0);
    }
    return label;
}

void* MetaPanel::AddIconButton(void* parent, const char* icon, std::function<void()> on_tap,
                               bool accent) {
    lv_obj_t* button = lv_button_create(Obj(parent));
    lv_obj_set_size(button, 56, 44);
    lv_obj_set_style_radius(button, 22, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, accent ? Accent() : CardColor(theme_), 0);
    lv_obj_set_style_bg_color(button, Pressed(), LV_STATE_PRESSED);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, theme_->icon_font()->font(), 0);
    lv_obj_set_style_text_color(label, accent ? lv_color_white() : theme_->text_color(), 0);
    lv_obj_center(label);
    AttachTap(button, std::move(on_tap));
    return button;
}

void* MetaPanel::AddTextButton(void* parent, const std::string& text, std::function<void()> on_tap,
                               bool accent) {
    lv_obj_t* button = lv_button_create(Obj(parent));
    lv_obj_set_height(button, 40);
    lv_obj_set_style_pad_hor(button, 14, 0);
    lv_obj_set_style_radius(button, 20, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, accent ? Accent() : CardColor(theme_), 0);
    lv_obj_set_style_bg_color(button, Pressed(), LV_STATE_PRESSED);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text.c_str());
    lv_obj_set_style_text_color(label, accent ? lv_color_white() : theme_->text_color(), 0);
    lv_obj_center(label);
    AttachTap(button, std::move(on_tap));
    return button;
}

void* MetaPanel::AddHeader(const char* icon, const std::string& title) {
    lv_obj_t* header = Box(Obj(root_));
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 6, 0);

    // Large tap target: back to the home screen, or close the panel from the home screen.
    lv_obj_t* back = Box(header);
    lv_obj_set_size(back, 44, 40);
    lv_obj_set_style_bg_color(back, Pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_t* back_icon = lv_label_create(back);
    lv_label_set_text(back_icon,
                      page_ == Page::kHome ? MATERIAL_SYMBOLS_CLOSE : MATERIAL_SYMBOLS_ARROW_BACK);
    lv_obj_set_style_text_font(back_icon, theme_->icon_font()->font(), 0);
    lv_obj_center(back_icon);
    AttachTap(back, [this]() {
        if (page_ == Page::kHome) {
            Hide();
        } else {
            Open(Page::kHome);
        }
    });

    if (icon != nullptr) {
        lv_obj_t* icon_label = lv_label_create(header);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, theme_->icon_font()->font(), 0);
        lv_obj_set_style_text_color(icon_label, Accent(), 0);
    }
    lv_obj_t* title_label = lv_label_create(header);
    lv_label_set_text(title_label, title.c_str());
    lv_obj_set_flex_grow(title_label, 1);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
    return title_label;
}

void* MetaPanel::AddScrollArea() {
    lv_obj_t* area = Box(Obj(root_));
    lv_obj_add_flag(area,
                    static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_width(area, LV_PCT(100));
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(area, 6, 0);
    lv_obj_set_scroll_dir(area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_ACTIVE);
    return area;
}

void* MetaPanel::AddListRow(void* list, const char* icon, const std::string& text,
                            const std::string& detail, std::function<void()> on_tap,
                            bool highlighted) {
    lv_obj_t* row = Box(Obj(list));
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 44, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, highlighted ? Accent() : CardColor(theme_), 0);
    lv_obj_set_style_bg_color(row, Pressed(), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_color_t text_color = highlighted ? lv_color_white() : theme_->text_color();
    lv_obj_set_style_text_color(row, text_color, 0);

    if (icon != nullptr) {
        lv_obj_t* icon_label = lv_label_create(row);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, theme_->icon_font()->font(), 0);
    }
    lv_obj_t* texts = Box(row);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* main_label = lv_label_create(texts);
    lv_obj_set_width(main_label, LV_PCT(100));
    lv_label_set_long_mode(main_label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(main_label, text.c_str());
    if (!detail.empty()) {
        lv_obj_t* detail_label = lv_label_create(texts);
        lv_obj_set_width(detail_label, LV_PCT(100));
        lv_label_set_long_mode(detail_label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(detail_label, detail.c_str());
        if (!highlighted) {
            lv_obj_set_style_text_color(detail_label, SecondaryText(theme_), 0);
        }
    }
    if (on_tap) {
        AttachTap(row, std::move(on_tap));
    }
    return row;
}

void* MetaPanel::AddButtonBar() {
    lv_obj_t* bar = Box(Obj(root_));
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(bar, 4, 0);
    return bar;
}

void MetaPanel::AddSwitchRow(void* parent, const std::string& text, bool value,
                             std::function<void(bool)> on_change) {
    lv_obj_t* row = Box(Obj(parent));
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, text.c_str());
    lv_obj_t* toggle = lv_switch_create(row);
    lv_obj_set_style_bg_color(
        toggle, Accent(),
        static_cast<lv_style_selector_t>(static_cast<uint32_t>(LV_PART_INDICATOR) |
                                         static_cast<uint32_t>(LV_STATE_CHECKED)));
    if (value) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    Button* button = NewButton(*building_buttons_, [toggle, on_change = std::move(on_change)]() {
        bool checked = false;
        {
            LvglLock lock;
            if (!lock || !lv_obj_is_valid(toggle)) {
                return;
            }
            checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);
        }
        on_change(checked);
    });
    lv_obj_add_event_cb(toggle, DispatchTap, LV_EVENT_VALUE_CHANGED, button);
}

void MetaPanel::AddSliderRow(void* parent, const char* icon, int value,
                             std::function<void(int)> on_change) {
    lv_obj_t* row = Box(Obj(parent));
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_t* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, theme_->icon_font()->font(), 0);
    lv_obj_t* slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_style_margin_right(slider, 14, 0);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, Accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, Accent(), LV_PART_KNOB);
    Button* button = NewButton(*building_buttons_, [slider, on_change = std::move(on_change)]() {
        int current = 0;
        {
            LvglLock lock;
            if (!lock || !lv_obj_is_valid(slider)) {
                return;
            }
            current = lv_slider_get_value(slider);
        }
        on_change(current);
    });
    // Apply the value when the finger lifts, not on every drag step.
    lv_obj_add_event_cb(slider, DispatchTap, LV_EVENT_RELEASED, button);
}

void MetaPanel::Toggle() {
    if (root_ != nullptr) {
        Hide();
    } else {
        Open(Page::kHome);
    }
}

void MetaPanel::Hide() {
    if (root_ == nullptr) {
        return;
    }
    LvglLock lock;
    if (lock) {
        CloseLocked();
    }
}

void MetaPanel::CloseLocked() {
    // The text viewer's close button callback lives in page_buttons_, which is freed below, so
    // the viewer must go first.
    CloseFileViewer();
    if (root_ != nullptr) {
        // lv_obj_delete also drops the widgets' event callbacks, so no handler can run with a
        // freed Button afterwards.
        lv_obj_delete(Obj(root_));
        root_ = nullptr;
    }
    ClearPageRefs();
    FreeButtons(page_buttons_);
}

void MetaPanel::ClearPageRefs() {
    list_area_ = nullptr;
    for (auto& label : tile_labels_) {
        label = nullptr;
    }
}

void MetaPanel::Open(Page page) {
    LvglLock lock;
    if (!lock) {
        return;
    }
    theme_ = CurrentTheme();
    if (theme_ == nullptr) {
        return;
    }
    // Rebuilding the same page keeps the list scrolled where the user left it.
    int32_t scroll_y = 0;
    if (page == page_ && root_ != nullptr && list_area_ != nullptr) {
        scroll_y = lv_obj_get_scroll_y(Obj(list_area_));
    } else if (page != page_) {
        list_offset_ = 0;
    }
    page_ = page;
    if (page == Page::kMusic) {
        music_.EnsureScanned();
    }
    CloseLocked();

    lv_obj_t* root = lv_obj_create(lv_layer_top());
    root_ = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, theme_->background_color(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 8, 0);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(root, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(root, theme_->text_color(), 0);
    // Taps must not fall through to the chat screen underneath.
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);

    building_buttons_ = &page_buttons_;
    switch (page_) {
        case Page::kHome:
            BuildHome();
            break;
        case Page::kMusic:
            BuildMusic();
            break;
        case Page::kReminders:
            BuildReminders();
            break;
        case Page::kFiles:
            BuildFiles();
            break;
        case Page::kSettings:
            BuildSettings();
            break;
        case Page::kImage:
        case Page::kText:
            // These are handled by ShowImageViewer/ShowTextViewer, not BuildX methods
            break;
    }
    if (scroll_y > 0 && list_area_ != nullptr) {
        lv_obj_update_layout(root);
        lv_obj_scroll_to_y(Obj(list_area_), scroll_y, LV_ANIM_OFF);
    }
    // Alerts and toasts stay above the panel.
    if (alert_ != nullptr) {
        lv_obj_move_foreground(Obj(alert_));
    }
    if (toast_ != nullptr) {
        lv_obj_move_foreground(Obj(toast_));
    }
    RefreshLocked();
}

void MetaPanel::Refresh() {
    if (root_ == nullptr) {
        return;
    }
    LvglLock lock;
    if (lock && root_ != nullptr) {
        RefreshLocked();
    }
}

// Called with the LVGL lock held. Open() may run from here: the lock is recursive and the
// rebuilt page records the revision it shows, so this does not loop.
void MetaPanel::RefreshLocked() {
    switch (page_) {
        case Page::kHome:
            RefreshHome();
            break;
        case Page::kMusic:
            if (music_.revision() != shown_revision_) {
                Open(Page::kMusic);
            }
            break;
        case Page::kReminders:
            if (reminders_.revision() != shown_revision_) {
                Open(Page::kReminders);
            }
            break;
        case Page::kFiles:
        case Page::kImage:
        case Page::kText:
        case Page::kSettings:
            // Static pages: nothing to refresh on a timer.
            break;
    }
}

void MetaPanel::SetLabel(void* label, const std::string& text) {
    if (label != nullptr && text != lv_label_get_text(Obj(label))) {
        lv_label_set_text(Obj(label), text.c_str());
    }
}

void MetaPanel::RefreshHome() {
    std::string lines[4];
    // Music
    if (music_.IsPlaying()) {
        lines[0] = music_.queued_title();
    } else if (music_.library_state() == LibraryState::kOk) {
        lines[0] = std::to_string(music_.tracks().size()) + " " + text::kSongs;
    } else {
        lines[0] = storage_.IsMounted() ? text::kNotPlaying : text::kNoCard;
    }
    // Reminders
    if (reminders_.items().empty()) {
        lines[1] = text::kNoReminders;
    } else {
        const Reminder& next = reminders_.items().front();
        lines[1] = FormatTime(next.due, "%H:%M ") + next.message;
    }
    // Files - show file count or card state
    if (storage_.IsMounted()) {
        std::vector<FileEntry> entries;
        if (file_browser_->ListDirectory(file_browser_->current_path(), entries)) {
            int file_count = 0, dir_count = 0;
            for (const auto& entry : entries) {
                if (entry.is_directory) {
                    dir_count++;
                } else {
                    file_count++;
                }
            }
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%d 文件, %d 文件夹", file_count, dir_count);
            lines[2] = buffer;
        } else {
            lines[2] = "无法访问";
        }
    } else {
        lines[2] = text::kNoCard;
    }
    // Settings tile shows the card state.
    uint64_t total = 0, free = 0;
    if (storage_.GetUsage(total, free) && total > 0) {
        lines[3] = FormatUsage(total, free);
    } else {
        lines[3] = storage_.state() == CardState::kNoFilesystem ? text::kNeedFat32 : text::kNoCard;
    }
    for (int i = 0; i < 4; ++i) {
        SetLabel(tile_labels_[i], lines[i]);
    }
}

void MetaPanel::BuildHome() {
    AddHeader(nullptr, "MetaOS");

    // 2x2 Grid layout
    lv_obj_t* grid = Box(Obj(root_));
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 4, 0);

    struct Tile {
        const char* icon;
        const char* title;
        Page page;
        uint32_t color;
    };
    const Tile tiles[4] = {
        {MATERIAL_SYMBOLS_MUSIC_NOTE, text::kMusic, Page::kMusic, 0x8E24AA},
        {MATERIAL_SYMBOLS_ALARM, text::kReminders, Page::kReminders, 0xF4511E},
        {MATERIAL_SYMBOLS_SD_CARD, text::kFiles, Page::kFiles, 0x43A047},
        {MATERIAL_SYMBOLS_SETTINGS, text::kSettings, Page::kSettings, 0x546E7A},
    };

    for (int i = 0; i < 4; ++i) {
        lv_color_t color = lv_color_hex(tiles[i].color);
        lv_obj_t* tile = Box(grid);

        // Each tile is 50% width minus gap, auto height
        lv_obj_set_size(tile, LV_PCT(48), LV_PCT(48));
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(tile, color, 0);
        lv_obj_set_style_bg_color(tile, lv_color_mix(color, lv_color_black(), 160),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_pad_all(tile, 8, 0);
        lv_obj_set_style_text_color(tile, lv_color_white(), 0);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        // Top row: icon + title
        lv_obj_t* top = Box(tile);
        lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(top, 4, 0);

        lv_obj_t* icon = lv_label_create(top);
        lv_label_set_text(icon, tiles[i].icon);
        lv_obj_set_style_text_font(icon, theme_->icon_font()->font(), 0);

        lv_obj_t* title = lv_label_create(top);
        lv_label_set_text(title, tiles[i].title);

        // Bottom line: status text (single line, truncate with dots)
        lv_obj_t* line = lv_label_create(tile);
        lv_obj_set_width(line, LV_PCT(100));
        lv_label_set_long_mode(line, LV_LABEL_LONG_CLIP);
        lv_label_set_text(line, "");
        lv_obj_set_style_text_opa(line, LV_OPA_80, 0);
        tile_labels_[i] = line;

        Page target = tiles[i].page;
        AttachTap(tile, [this, target]() { Open(target); });
    }
}

void MetaPanel::BuildMusic() {
    shown_revision_ = music_.revision();
    LibraryState state = music_.library_state();
    if (state != LibraryState::kOk) {
        AddHeader(MATERIAL_SYMBOLS_MUSIC_NOTE, text::kMusic);
        lv_obj_t* area = Obj(AddScrollArea());
        const char* message = text::kNoSongs;
        if (state == LibraryState::kNoCard) {
            message =
                storage_.state() == CardState::kNoFilesystem ? text::kNeedFat32 : text::kNoCard;
        } else if (state == LibraryState::kNoFolder) {
            message = text::kNoMusicFolder;
        }
        AddLabel(area, message);
        lv_obj_t* bar = Obj(AddButtonBar());
        AddTextButton(bar, text::kRetry, [this]() {
            if (hooks_.retry_card) {
                hooks_.retry_card();
            }
            music_.Rescan();
        });
        return;
    }

    // The header shows what is playing, so the list keeps most of the small screen.
    const auto& tracks = music_.tracks();
    std::string title;
    if (music_.IsPlaying()) {
        title = music_.queued_title();
    } else if (music_.IsPending()) {
        title = text::kWaitingForChat;
    } else {
        title =
            std::string(text::kMusic) + " · " + std::to_string(tracks.size()) + " " + text::kSongs;
    }
    lv_obj_t* title_label = Obj(AddHeader(MATERIAL_SYMBOLS_MUSIC_NOTE, title));
    if (music_.IsPlaying()) {
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    }

    // One page of rows at a time keeps large libraries light on memory.
    if (list_offset_ >= tracks.size()) {
        list_offset_ = 0;
    }
    lv_obj_t* list = Obj(AddScrollArea());
    list_area_ = list;
    int playing = music_.current_track();
    size_t end = std::min(tracks.size(), list_offset_ + kTracksPerPage);
    for (size_t i = list_offset_; i < end; ++i) {
        bool current = static_cast<int>(i) == playing;
        AddListRow(
            list, current ? MATERIAL_SYMBOLS_PLAY_ARROW : nullptr, MusicPlayer::TitleOf(tracks[i]),
            "", [this, i]() { music_.PlayFrom(i); }, current);
    }
    if (tracks.size() > kTracksPerPage) {
        lv_obj_t* pager = Box(list);
        lv_obj_set_size(pager, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        if (list_offset_ > 0) {
            AddTextButton(pager, text::kPrevPage, [this]() {
                list_offset_ = list_offset_ >= kTracksPerPage ? list_offset_ - kTracksPerPage : 0;
                Open(Page::kMusic);
            });
        }
        char range[32];
        snprintf(range, sizeof(range), "%u-%u / %u", static_cast<unsigned>(list_offset_ + 1),
                 static_cast<unsigned>(end), static_cast<unsigned>(tracks.size()));
        lv_obj_t* range_label = Obj(AddLabel(pager, range, true));
        lv_obj_set_width(range_label, LV_SIZE_CONTENT);
        if (end < tracks.size()) {
            AddTextButton(pager, text::kNextPage, [this]() {
                list_offset_ += kTracksPerPage;
                Open(Page::kMusic);
            });
        }
    }

    // Transport controls.
    lv_obj_t* bar = Obj(AddButtonBar());
    AddIconButton(bar, MATERIAL_SYMBOLS_SKIP_PREVIOUS, [this]() { music_.SkipPrevious(); });
    bool active = music_.IsPlaying() || music_.IsPending();
    AddIconButton(
        bar, active ? MATERIAL_SYMBOLS_STOP : MATERIAL_SYMBOLS_PLAY_ARROW,
        [this]() {
            if (music_.IsPlaying() || music_.IsPending()) {
                music_.StopPlayback();
            } else {
                music_.Resume();
            }
        },
        true);
    AddIconButton(bar, MATERIAL_SYMBOLS_SKIP_NEXT, [this]() { music_.SkipNext(); });
    lv_obj_t* shuffle = Obj(AddIconButton(bar, MATERIAL_SYMBOLS_REPEAT,
                                          [this]() { music_.SetShuffle(!music_.shuffle()); }));
    if (music_.shuffle()) {
        lv_obj_set_style_bg_color(shuffle, Accent(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(shuffle, 0), lv_color_white(), 0);
    }
}

void MetaPanel::BuildReminders() {
    shown_revision_ = reminders_.revision();
    AddHeader(MATERIAL_SYMBOLS_ALARM, text::kReminders);
    lv_obj_t* list = Obj(AddScrollArea());
    list_area_ = list;
    const auto& items = reminders_.items();
    if (items.empty()) {
        AddLabel(list, text::kNoReminders);
        AddLabel(list, text::kReminderHint, true);
        return;
    }
    if (!reminders_.persisted()) {
        lv_obj_t* warning = Obj(AddLabel(list, text::kNotSaved));
        lv_obj_set_style_text_color(warning, lv_color_hex(0xE53935), 0);
    }
    for (const auto& reminder : items) {
        std::string when = FormatDue(reminder.due);
        if (reminder.daily) {
            when += std::string(" · ") + text::kDaily;
        }
        lv_obj_t* row = Obj(AddListRow(list, nullptr, reminder.message, when, nullptr));
        // Cancel button at the end of the row.
        int id = reminder.id;
        lv_obj_t* cancel = Obj(AddIconButton(row, MATERIAL_SYMBOLS_DELETE, [this, id]() {
            reminders_.Cancel(id);
            ShowToast(text::kCancelled);
        }));
        lv_obj_set_size(cancel, 44, 36);
    }
}

void MetaPanel::BuildFiles() {
    // Breadcrumb header showing current path
    auto crumbs = file_browser_->GetBreadcrumbs();
    std::string title;
    if (crumbs.size() <= 1) {
        title = text::kFiles;
    } else {
        title = crumbs.back();
    }
    AddHeader(MATERIAL_SYMBOLS_SD_CARD, title);

    lv_obj_t* area = Obj(AddScrollArea());
    list_area_ = area;

    if (!storage_.IsMounted()) {
        AddLabel(area, text::kNoCard);
        AddTextButton(area, text::kRetry, [this]() { hooks_.retry_card(); });
        return;
    }

    // Breadcrumb navigation if not at root
    if (crumbs.size() > 1) {
        lv_obj_t* breadcrumb = Box(area);
        lv_obj_set_width(breadcrumb, LV_PCT(100));
        lv_obj_set_style_pad_all(breadcrumb, 8, 0);
        lv_obj_set_flex_flow(breadcrumb, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(breadcrumb, 4, 0);

        for (size_t i = 0; i < crumbs.size(); ++i) {
            if (i > 0) {
                AddLabel(breadcrumb, "/", true);
            }
            if (i == crumbs.size() - 1) {
                // Current folder - not clickable
                AddLabel(breadcrumb, crumbs[i]);
            } else {
                // Clickable parent folder
                lv_obj_t* link = Obj(AddLabel(breadcrumb, crumbs[i]));
                lv_obj_set_style_text_color(link, lv_color_hex(0x2196F3), 0);
                AttachTap(link, [this, i]() {
                    // Navigate to this level
                    std::string path = storage_.MountPoint();
                    auto crumbs = file_browser_->GetBreadcrumbs();
                    for (size_t j = 1; j <= i; ++j) {
                        path += "/" + crumbs[j];
                    }
                    if (file_browser_->NavigateTo(path)) {
                        Open(Page::kFiles);
                    }
                });
            }
        }
    }

    // List files and directories
    std::vector<FileEntry> entries;
    if (!file_browser_->ListDirectory(file_browser_->current_path(), entries)) {
        AddLabel(area, "无法读取目录");
        return;
    }

    if (entries.empty()) {
        AddLabel(area, "空文件夹", true);
        return;
    }

    // Add parent directory link if not at root
    if (crumbs.size() > 1) {
        AddListRow(area, MATERIAL_SYMBOLS_ARROW_UPWARD, "..", "", [this]() {
            if (file_browser_->NavigateUp()) {
                Open(Page::kFiles);
            }
        });
    }

    // Show each file/directory
    for (const auto& entry : entries) {
        std::string detail;
        const char* icon;

        if (entry.is_directory) {
            icon = MATERIAL_SYMBOLS_SD_CARD;  // Use SD_CARD icon for folders
            detail = "";
        } else {
            icon = MATERIAL_SYMBOLS_IMAGE;  // Default file icon
            detail = FileBrowser::FormatFileSize(entry.size);

            // Special icons for known file types
            if (FileBrowser::IsImageFile(entry.name)) {
                icon = MATERIAL_SYMBOLS_IMAGE;
            } else if (FileBrowser::IsTextFile(entry.name)) {
                icon = MATERIAL_SYMBOLS_EDIT_SQUARE;  // Use edit icon for text files
            }
        }

        std::string full_path = file_browser_->current_path() + "/" + entry.name;

        AddListRow(area, icon, entry.name, detail, [this, entry, full_path]() {
            if (entry.is_directory) {
                // Navigate into directory
                if (file_browser_->NavigateTo(full_path)) {
                    Open(Page::kFiles);
                }
            } else if (FileBrowser::IsImageFile(entry.name)) {
                ShowImageViewer(full_path);
            } else if (FileBrowser::IsTextFile(entry.name)) {
                ShowTextViewer(full_path);
            } else {
                // Unknown file type
                ShowToast("不支持的文件类型");
            }
        });
    }
}

void MetaPanel::BuildSettings() {
    AddHeader(MATERIAL_SYMBOLS_SETTINGS, text::kSettings);
    lv_obj_t* area = Obj(AddScrollArea());
    list_area_ = area;
    auto& board = Board::GetInstance();

    // Volume and brightness apply when the finger lifts.
    if (auto codec = board.GetAudioCodec()) {
        AddSliderRow(area, MATERIAL_SYMBOLS_VOLUME_UP, codec->output_volume(), [](int value) {
            Board::GetInstance().GetAudioCodec()->SetOutputVolume(value);
        });
    }
    if (auto backlight = board.GetBacklight()) {
        AddSliderRow(area, MATERIAL_SYMBOLS_BRIGHTNESS_6, backlight->brightness(), [](int value) {
            if (auto light = Board::GetInstance().GetBacklight()) {
                light->SetBrightness(static_cast<uint8_t>(std::max(value, 5)), true);
            }
        });
    }

    // Card state with a retry button.
    std::string card;
    uint64_t total = 0, free = 0;
    if (storage_.GetUsage(total, free) && total > 0) {
        card = FormatUsage(total, free);
    } else {
        card = storage_.state() == CardState::kNoFilesystem ? text::kNeedFat32 : text::kNoCard;
    }
    lv_obj_t* row = Obj(AddListRow(area, MATERIAL_SYMBOLS_SD_CARD, card, "", nullptr));
    if (!storage_.IsMounted()) {
        AddTextButton(row, text::kRetry, [this]() {
            if (hooks_.retry_card) {
                hooks_.retry_card();
            }
            Open(Page::kSettings);
        });
    }

    // Motion gesture settings (if IMU is available)
    // TODO: Check if IMU exists before showing these
    AddSwitchRow(area, text::kShakeNext, settings_.shake_next, [this](bool enabled) {
        settings_.shake_next = enabled;
        if (hooks_.settings_changed) {
            hooks_.settings_changed();
        }
    });
    AddSwitchRow(area, text::kFlipStop, settings_.flip_stop, [this](bool enabled) {
        settings_.flip_stop = enabled;
        if (hooks_.settings_changed) {
            hooks_.settings_changed();
        }
    });

    AddLabel(area, text::kVoiceHint, true);
}

void MetaPanel::OpenAlert(const char* icon, const std::string& title, const std::string& body,
                          int timeout_ms) {
    CloseAlertLocked();
    building_buttons_ = &alert_buttons_;

    // Dim layer that blocks taps on whatever is underneath.
    lv_obj_t* scrim = Box(lv_layer_top());
    alert_ = scrim;
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* card = Box(scrim);
    lv_obj_set_width(card, LV_PCT(92));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, LV_PCT(92), 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, theme_->background_color(), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_text_font(card, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(card, theme_->text_color(), 0);

    lv_obj_t* header = Box(card);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 8, 0);
    lv_obj_t* icon_label = lv_label_create(header);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, theme_->icon_font()->font(), 0);
    lv_obj_set_style_text_color(icon_label, Accent(), 0);
    lv_obj_t* title_label = lv_label_create(header);
    lv_label_set_text(title_label, title.c_str());

    // The body scrolls when it is longer than the screen.
    lv_obj_t* body_area = Box(card);
    lv_obj_add_flag(body_area,
                    static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_width(body_area, LV_PCT(100));
    lv_obj_set_height(body_area, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(body_area, 120, 0);
    lv_obj_set_scroll_dir(body_area, LV_DIR_VER);
    AddLabel(body_area, body);

    alert_bar_ = Box(card);
    lv_obj_set_size(Obj(alert_bar_), LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(Obj(alert_bar_), LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(Obj(alert_bar_), LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(Obj(alert_bar_), 8, 0);

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* panel = static_cast<MetaPanel*>(lv_timer_get_user_data(timer));
            // The LVGL task runs timers; closing belongs to the main task.
            Application::GetInstance().Schedule([panel]() { panel->CloseAlert(); });
        },
        timeout_ms, this);
    lv_timer_set_repeat_count(timer, 1);
    lv_timer_set_auto_delete(timer, false);
    alert_timer_ = timer;
}

void MetaPanel::ShowReminderAlert(const std::vector<Reminder>& due) {
    if (due.empty()) {
        return;
    }
    LvglLock lock;
    if (!lock) {
        return;
    }
    theme_ = CurrentTheme();
    if (theme_ == nullptr) {
        return;
    }
    std::string body;
    for (const auto& reminder : due) {
        if (!body.empty()) {
            body += "\n";
        }
        body += FormatTime(reminder.due, "%H:%M  ") + reminder.message;
    }
    OpenAlert(MATERIAL_SYMBOLS_ALARM, text::kReminderDue, body, kReminderAlertMs);
    alert_reminders_ = due;
    AddTextButton(alert_bar_, text::kSnooze, [this]() {
        auto snoozed = alert_reminders_;
        CloseAlert();
        for (const auto& reminder : snoozed) {
            if (hooks_.snooze) {
                hooks_.snooze(reminder, 10);
            }
        }
        ShowToast(text::kSnoozed);
    });
    AddTextButton(alert_bar_, text::kGotIt, [this]() { CloseAlert(); }, true);
    building_buttons_ = &page_buttons_;
}

void MetaPanel::CloseAlert() {
    if (alert_ == nullptr) {
        return;
    }
    LvglLock lock;
    if (lock) {
        CloseAlertLocked();
    }
}

void MetaPanel::CloseAlertLocked() {
    if (alert_timer_ != nullptr) {
        lv_timer_delete(static_cast<lv_timer_t*>(alert_timer_));
        alert_timer_ = nullptr;
    }
    if (alert_ != nullptr) {
        lv_obj_delete(Obj(alert_));
        alert_ = nullptr;
    }
    alert_bar_ = nullptr;
    alert_reminders_.clear();
    FreeButtons(alert_buttons_);
}

void MetaPanel::ShowToast(const std::string& message) {
    LvglLock lock;
    if (!lock) {
        return;
    }
    LvglTheme* theme = CurrentTheme();
    if (theme == nullptr) {
        return;
    }
    CloseToast();
    lv_obj_t* toast = Box(lv_layer_top());
    toast_ = toast;
    lv_obj_set_width(toast, LV_SIZE_CONTENT);
    lv_obj_set_height(toast, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(toast, LV_PCT(90), 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x323232), 0);
    lv_obj_set_style_radius(toast, 16, 0);
    lv_obj_set_style_pad_hor(toast, 14, 0);
    lv_obj_set_style_pad_ver(toast, 8, 0);
    lv_obj_t* label = lv_label_create(toast);
    lv_obj_set_style_text_font(label, theme->text_font()->font(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, message.c_str());

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* panel = static_cast<MetaPanel*>(lv_timer_get_user_data(timer));
            Application::GetInstance().Schedule([panel]() {
                LvglLock lock;
                if (lock) {
                    panel->CloseToast();
                }
            });
        },
        kToastMs, this);
    lv_timer_set_repeat_count(timer, 1);
    lv_timer_set_auto_delete(timer, false);
    toast_timer_ = timer;
}

void MetaPanel::CloseToast() {
    if (toast_timer_ != nullptr) {
        lv_timer_delete(static_cast<lv_timer_t*>(toast_timer_));
        toast_timer_ = nullptr;
    }
    if (toast_ != nullptr) {
        lv_obj_delete(Obj(toast_));
        toast_ = nullptr;
    }
}

void MetaPanel::ShowImageViewer(const std::string& path) {
    LvglLock lock;
    if (!lock) {
        return;
    }
    theme_ = CurrentTheme();
    if (theme_ == nullptr) {
        return;
    }

    CloseFileViewer();
    viewing_file_ = path;

    // Full-screen viewer on the top layer
    lv_obj_t* viewer = lv_obj_create(lv_layer_top());
    file_viewer_ = viewer;
    lv_obj_remove_style_all(viewer);
    lv_obj_set_size(viewer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(viewer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(viewer, 0, 0);
    lv_obj_set_flex_flow(viewer, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(viewer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(viewer, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(viewer, lv_color_white(), 0);
    lv_obj_add_flag(viewer, LV_OBJ_FLAG_CLICKABLE);

    // Header bar with close button
    lv_obj_t* header = Box(viewer);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);

    size_t last_slash = path.find_last_of('/');
    std::string filename = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
    lv_obj_t* name = lv_label_create(header);
    lv_label_set_text(name, filename.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name, 1);

    AddIconButton(header, MATERIAL_SYMBOLS_CLOSE, [this]() { CloseFileViewer(); });

    // Content area (image will be placed here when decoded)
    lv_obj_t* content = Box(viewer);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    file_viewer_content_ = content;

    // Show loading text
    lv_obj_t* loading_label = lv_label_create(content);
    lv_label_set_text(loading_label, text::kLoading);
    lv_obj_set_style_text_color(loading_label, lv_color_white(), 0);
    loading_spinner_ = loading_label;  // Track so we can delete it when the image arrives

    // Start async decode on the worker
    std::string path_copy = path;
    bool posted = PostWork([this, path_copy]() {
        ImageError error = ImageError::kNone;
        auto decoded = DecodeImage(path_copy, 320, 240, error);

        // Hand result back to main task
        Application::GetInstance().Schedule(
            [this, decoded_ptr = decoded.release(), path_copy, error]() {
                std::unique_ptr<DecodedImage> decoded(decoded_ptr);

                // Check if viewer is still open for this file
                if (file_viewer_ == nullptr || viewing_file_ != path_copy) {
                    return;
                }

                LvglLock lock;
                if (!lock) {
                    return;
                }

                // Remove loading spinner
                if (loading_spinner_ != nullptr) {
                    lv_obj_delete(Obj(loading_spinner_));
                    loading_spinner_ = nullptr;
                }

                if (error != ImageError::kNone || decoded == nullptr) {
                    // Show error message
                    lv_obj_t* error_label = lv_label_create(Obj(file_viewer_content_));
                    const char* msg = text::kImageLoadFailed;
                    if (error == ImageError::kUnsupported) {
                        msg = text::kUnsupportedFormat;
                    }
                    lv_label_set_text(error_label, msg);
                    lv_obj_set_style_text_color(error_label, lv_color_hex(0xE53935), 0);
                    return;
                }

                OnImageDecoded(decoded.release(), path_copy);
            });
    });

    if (!posted) {
        // Worker queue full
        if (loading_spinner_ != nullptr) {
            lv_obj_delete(Obj(loading_spinner_));
            loading_spinner_ = nullptr;
        }
        lv_obj_t* error_label = lv_label_create(content);
        lv_label_set_text(error_label, text::kImageLoadFailed);
        lv_obj_set_style_text_color(error_label, lv_color_hex(0xE53935), 0);
    }

    // Keep alerts and toasts on top
    if (alert_ != nullptr) {
        lv_obj_move_foreground(Obj(alert_));
    }
    if (toast_ != nullptr) {
        lv_obj_move_foreground(Obj(toast_));
    }
}

void MetaPanel::OnImageDecoded(void* decoded_image, const std::string& path) {
    auto* image = static_cast<DecodedImage*>(decoded_image);
    if (image == nullptr || file_viewer_content_ == nullptr) {
        delete image;
        return;
    }

    if (image->kind == DecodedImage::Kind::kGif) {
        // Check LVGL memory before creating GIF player (needs contiguous space)
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        size_t needed = image->width * image->height * 5 + 32768;  // GIF canvas + overhead
        if (mon.free_biggest_size < needed) {
            ESP_LOGW(TAG, "Not enough LVGL memory for GIF: need %zu, have %u", needed,
                     mon.free_biggest_size);
            lv_obj_t* error_label = lv_label_create(Obj(file_viewer_content_));
            lv_label_set_text(error_label, "内存不足");
            lv_obj_set_style_text_color(error_label, lv_color_hex(0xE53935), 0);
            delete image;
            return;
        }

        // Create LVGL image descriptor for GIF. Value-initialized: LVGL 9 checks
        // header.magic and reads header.stride, so they must not be garbage.
        lv_img_dsc_t* img_dsc = new (std::nothrow) lv_img_dsc_t();
        if (img_dsc == nullptr) {
            delete image;
            return;
        }
        img_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        img_dsc->header.cf = LV_COLOR_FORMAT_RAW;
        img_dsc->header.w = image->width;
        img_dsc->header.h = image->height;
        img_dsc->data = image->data;
        img_dsc->data_size = image->size;

        // Create GIF player
        LvglGif* gif = new (std::nothrow) LvglGif(img_dsc);
        if (gif == nullptr || !gif->IsLoaded()) {
            delete gif;
            delete img_dsc;
            delete image;
            lv_obj_t* error_label = lv_label_create(Obj(file_viewer_content_));
            lv_label_set_text(error_label, text::kImageLoadFailed);
            lv_obj_set_style_text_color(error_label, lv_color_hex(0xE53935), 0);
            return;
        }

        lv_obj_t* img_obj = lv_image_create(Obj(file_viewer_content_));
        lv_image_set_src(img_obj, gif->image_dsc());
        gif->Start();
        gif_player_ = gif;

        // Transfer ownership - don't delete image data, GIF player needs it
        image->data = nullptr;
        delete image;
        delete img_dsc;  // LvglGif makes its own copy
    } else {
        // Static bitmap (RGB565). Value-initialized; LVGL 9 needs magic and stride set.
        lv_img_dsc_t* img_dsc = new (std::nothrow) lv_img_dsc_t();
        if (img_dsc == nullptr) {
            delete image;
            return;
        }
        img_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
        img_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
        img_dsc->header.w = image->width;
        img_dsc->header.h = image->height;
        img_dsc->header.stride = image->width * 2;
        img_dsc->data = image->data;
        img_dsc->data_size = image->size;

        lv_obj_t* img_obj = lv_image_create(Obj(file_viewer_content_));
        lv_image_set_src(img_obj, img_dsc);

        // Track for cleanup
        image_dsc_ = img_dsc;

        // Transfer ownership - LVGL will use the data, we'll free it in CloseFileViewer
        image->data = nullptr;
        delete image;
    }
}

void MetaPanel::ShowTextViewer(const std::string& path) {
    LvglLock lock;
    if (!lock) {
        return;
    }
    theme_ = CurrentTheme();
    if (theme_ == nullptr) {
        return;
    }

    CloseFileViewer();
    viewing_file_ = path;

    // Read file content
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        ShowToast("无法打开文件");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Limit to 64KB for memory safety
    if (size > 65536) {
        fclose(f);
        ShowToast("文件太大");
        return;
    }

    std::string content;
    content.resize(size);
    size_t read = fread(&content[0], 1, size, f);
    fclose(f);

    if (read != static_cast<size_t>(size)) {
        ShowToast("读取失败");
        return;
    }

    // Full-screen viewer on the top layer, above the panel (the panel root lives there too, so
    // the viewer must be created after it to be drawn on top).
    lv_obj_t* viewer = lv_obj_create(lv_layer_top());
    file_viewer_ = viewer;
    lv_obj_remove_style_all(viewer);
    lv_obj_set_size(viewer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(viewer, theme_->background_color(), 0);
    lv_obj_set_style_bg_opa(viewer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(viewer, 8, 0);
    lv_obj_set_style_pad_row(viewer, 6, 0);
    lv_obj_set_flex_flow(viewer, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(viewer, LV_OBJ_FLAG_SCROLLABLE);
    // Not a child of root_, so the theme font and color must be set here; the default font has
    // no Chinese glyphs.
    lv_obj_set_style_text_font(viewer, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(viewer, theme_->text_color(), 0);
    // Taps must not fall through to the panel underneath.
    lv_obj_add_flag(viewer, LV_OBJ_FLAG_CLICKABLE);

    // Header: file name, then a close button that stays visible however long the name is.
    lv_obj_t* header = Box(viewer);
    lv_obj_set_size(header, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 6, 0);

    size_t last_slash = path.find_last_of('/');
    std::string filename = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
    lv_obj_t* name = lv_label_create(header);
    lv_label_set_text(name, filename.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name, 1);
    AddIconButton(header, MATERIAL_SYMBOLS_CLOSE, [this]() { CloseFileViewer(); });

    // Read-only text: a plain label in a scrollable box, no text area or keyboard.
    lv_obj_t* scroll_area = Box(viewer);
    lv_obj_add_flag(scroll_area,
                    static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_width(scroll_area, LV_PCT(100));
    lv_obj_set_flex_grow(scroll_area, 1);
    lv_obj_set_scroll_dir(scroll_area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_area, LV_SCROLLBAR_MODE_ACTIVE);

    lv_obj_t* text_label = lv_label_create(scroll_area);
    lv_obj_set_width(text_label, LV_PCT(100));
    lv_label_set_long_mode(text_label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(text_label, content.empty() ? "(空文件)" : content.c_str());

    // Alerts and toasts stay above the viewer.
    if (alert_ != nullptr) {
        lv_obj_move_foreground(Obj(alert_));
    }
    if (toast_ != nullptr) {
        lv_obj_move_foreground(Obj(toast_));
    }
}

// Main task. Takes the (recursive) LVGL lock: the close button calls this without it.
void MetaPanel::CloseFileViewer() {
    if (file_viewer_ == nullptr) {
        return;
    }
    LvglLock lock;
    if (lock && file_viewer_ != nullptr) {
        // Clean up GIF player if present
        if (gif_player_ != nullptr) {
            delete static_cast<LvglGif*>(gif_player_);
            gif_player_ = nullptr;
        }
        // Clean up image descriptor and its data
        if (image_dsc_ != nullptr) {
            auto* img_dsc = static_cast<lv_img_dsc_t*>(image_dsc_);
            if (img_dsc->data != nullptr) {
                heap_caps_free(const_cast<uint8_t*>(img_dsc->data));
            }
            delete img_dsc;
            image_dsc_ = nullptr;
        }
        lv_obj_delete(Obj(file_viewer_));
        file_viewer_ = nullptr;
        file_viewer_content_ = nullptr;
        loading_spinner_ = nullptr;
        viewing_file_.clear();
    }
}

#else  // !METAOS_PANEL_ENABLED

struct MetaPanel::Button {};

MetaPanel::MetaPanel(Storage& storage, MusicPlayer& music, ReminderStore& reminders,
                     MetaSettings& settings, Hooks hooks)
    : storage_(storage),
      music_(music),
      reminders_(reminders),
      settings_(settings),
      hooks_(std::move(hooks)) {}
MetaPanel::~MetaPanel() = default;
void MetaPanel::Toggle() {}
void MetaPanel::Open(Page) {}
void MetaPanel::Hide() {}
void MetaPanel::Refresh() {}
void MetaPanel::ShowReminderAlert(const std::vector<Reminder>&) {}
void MetaPanel::CloseAlert() {}
void MetaPanel::ShowToast(const std::string&) {}

#endif  // METAOS_PANEL_ENABLED

}  // namespace metaos
