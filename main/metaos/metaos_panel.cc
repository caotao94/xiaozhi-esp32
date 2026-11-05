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
#include "lvgl_theme.h"
#include "metaos_common.h"
#include "metaos_hermes.h"
#include "metaos_music.h"
#include "metaos_strings.h"
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
constexpr int kHermesAlertMs = 60 * 1000;
constexpr size_t kHermesPageTextBytes = 6 * 1024;
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
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
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

std::string FormatElapsed(int seconds) {
    char buffer[16];
    if (seconds < 60) {
        snprintf(buffer, sizeof(buffer), "%ds", seconds);
    } else {
        snprintf(buffer, sizeof(buffer), "%dm%02ds", seconds / 60, seconds % 60);
    }
    return buffer;
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
                     HermesClient& hermes, MetaSettings& settings, Hooks hooks)
    : storage_(storage),
      music_(music),
      reminders_(reminders),
      hermes_(hermes),
      settings_(settings),
      hooks_(std::move(hooks)) {}

MetaPanel::~MetaPanel() {
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
    lv_obj_add_flag(area, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
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
    lv_obj_set_style_bg_color(toggle, Accent(), static_cast<lv_style_selector_t>(static_cast<uint32_t>(LV_PART_INDICATOR) | static_cast<uint32_t>(LV_STATE_CHECKED)));
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
    title_label_ = nullptr;
    status_label_ = nullptr;
    body_label_ = nullptr;
    list_area_ = nullptr;
    for (auto& label : tile_labels_) {
        label = nullptr;
    }
    shown_text_.clear();
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
        case Page::kHermes:
            BuildHermes();
            break;
        case Page::kSettings:
            BuildSettings();
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
        case Page::kHermes:
            RefreshHermes();
            break;
        case Page::kSettings:
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
    // Hermes
    auto snapshot = hermes_.GetSnapshot(0);
    switch (snapshot.state) {
        case HermesClient::State::kIdle:
            lines[2] = text::kHermesIdle;
            break;
        case HermesClient::State::kRunning:
            lines[2] = std::string(text::kRunning) + " " + FormatElapsed(snapshot.elapsed_seconds);
            break;
        case HermesClient::State::kDone:
            lines[2] = text::kDone;
            break;
        default:
            lines[2] = text::kFailed;
            break;
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

    lv_obj_t* grid = Box(Obj(root_));
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_pad_row(grid, 8, 0);

    struct Tile {
        const char* icon;
        const char* title;
        Page page;
        uint32_t color;
    };
    const Tile tiles[4] = {
        {MATERIAL_SYMBOLS_MUSIC_NOTE, text::kMusic, Page::kMusic, 0x8E24AA},
        {MATERIAL_SYMBOLS_ALARM, text::kReminders, Page::kReminders, 0xF4511E},
        {MATERIAL_SYMBOLS_ROBOT_2, "Hermes", Page::kHermes, 0x1E88E5},
        {MATERIAL_SYMBOLS_SETTINGS, text::kSettings, Page::kSettings, 0x546E7A},
    };
    for (int i = 0; i < 4; ++i) {
        lv_color_t color = lv_color_hex(tiles[i].color);
        lv_obj_t* tile = Box(grid);
        lv_obj_set_size(tile, LV_PCT(48), LV_PCT(48));
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(tile, color, 0);
        lv_obj_set_style_bg_color(tile, lv_color_mix(color, lv_color_black(), 160),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_pad_all(tile, 10, 0);
        lv_obj_set_style_text_color(tile, lv_color_white(), 0);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        lv_obj_t* top = Box(tile);
        lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(top, 6, 0);
        lv_obj_t* icon = lv_label_create(top);
        lv_label_set_text(icon, tiles[i].icon);
        lv_obj_set_style_text_font(icon, theme_->icon_font()->font(), 0);
        lv_obj_t* title = lv_label_create(top);
        lv_label_set_text(title, tiles[i].title);

        lv_obj_t* line = lv_label_create(tile);
        lv_obj_set_width(line, LV_PCT(100));
        lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_DOTS);
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

void MetaPanel::RefreshHermes() {
    unsigned revision = hermes_.revision();
    if (revision == shown_revision_) {
        // Only the elapsed time moves; update it in place.
        auto snapshot = hermes_.GetSnapshot(0);
        if (snapshot.state == HermesClient::State::kRunning) {
            SetLabel(status_label_, HermesStatus(snapshot));
        }
        return;
    }
    auto snapshot = hermes_.GetSnapshot(kHermesPageTextBytes);
    if (snapshot.state == HermesClient::State::kRunning && body_label_ != nullptr &&
        status_label_ != nullptr) {
        // New text arrived: update the labels without rebuilding, so scrolling is kept.
        shown_revision_ = revision;
        SetLabel(status_label_, HermesStatus(snapshot));
        if (snapshot.text != shown_text_) {
            shown_text_ = snapshot.text;
            lv_label_set_text(Obj(body_label_), shown_text_.c_str());
        }
        return;
    }
    Open(Page::kHermes);
}

std::string MetaPanel::HermesStatus(const HermesClient::Snapshot& snapshot) {
    std::string status;
    switch (snapshot.state) {
        case HermesClient::State::kRunning:
            status = text::kRunning;
            break;
        case HermesClient::State::kDone:
            status = text::kDone;
            break;
        default:
            status = text::kFailed;
            break;
    }
    status += " · " + FormatElapsed(snapshot.elapsed_seconds);
    if (snapshot.tool_steps > 0) {
        status += " · " + std::to_string(snapshot.tool_steps) + " " + text::kSteps;
    }
    return status;
}

void MetaPanel::BuildHermes() {
    shown_revision_ = hermes_.revision();
    AddHeader(MATERIAL_SYMBOLS_ROBOT_2, "Hermes");
    lv_obj_t* area = Obj(AddScrollArea());
    list_area_ = area;
    auto snapshot = hermes_.GetSnapshot(kHermesPageTextBytes);

    if (snapshot.state == HermesClient::State::kIdle) {
        AddLabel(area, hermes_configured_ ? text::kHermesIdle : text::kHermesNotConfigured);
        AddLabel(area, text::kHermesHint, true);
        return;
    }

    lv_obj_t* task = Obj(AddLabel(area, snapshot.task, true));
    lv_label_set_long_mode(task, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_height(task, 48, 0);
    status_label_ = AddLabel(area, HermesStatus(snapshot));
    lv_obj_set_style_text_color(Obj(status_label_),
                                snapshot.state == HermesClient::State::kRunning ? Accent()
                                : snapshot.state == HermesClient::State::kDone
                                    ? lv_color_hex(0x43A047)
                                    : lv_color_hex(0xE53935),
                                0);
    shown_text_ = snapshot.text;
    body_label_ = AddLabel(area, shown_text_);
    if (snapshot.truncated && snapshot.state != HermesClient::State::kRunning) {
        AddLabel(area, text::kTruncated, true);
    }

    lv_obj_t* bar = Obj(AddButtonBar());
    if (snapshot.state == HermesClient::State::kRunning) {
        AddTextButton(bar, text::kStop, [this]() { hermes_.Cancel(); });
    } else if (snapshot.state == HermesClient::State::kDone) {
        AddTextButton(
            bar, text::kRead,
            [this]() {
                if (hooks_.speak_hermes_result) {
                    hooks_.speak_hermes_result();
                }
            },
            true);
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
    AddSwitchRow(area, text::kSpeakReminders, settings_.speak_reminders, [this](bool on) {
        settings_.speak_reminders = on;
        if (hooks_.settings_changed) {
            hooks_.settings_changed();
        }
    });
    AddSwitchRow(area, text::kSpeakHermes, settings_.speak_hermes, [this](bool on) {
        settings_.speak_hermes = on;
        if (hooks_.settings_changed) {
            hooks_.settings_changed();
        }
    });

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
    lv_obj_add_flag(body_area, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
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

void MetaPanel::ShowHermesAlert() {
    LvglLock lock;
    if (!lock) {
        return;
    }
    theme_ = CurrentTheme();
    if (theme_ == nullptr) {
        return;
    }
    auto snapshot = hermes_.GetSnapshot(1200);
    bool done = snapshot.state == HermesClient::State::kDone;
    std::string body = snapshot.text;
    if (snapshot.truncated) {
        body += "...";
    }
    OpenAlert(MATERIAL_SYMBOLS_ROBOT_2, done ? text::kHermesDone : text::kHermesFailed, body,
              kHermesAlertMs);
    alert_reminders_.clear();
    AddTextButton(alert_bar_, text::kClose, [this]() { CloseAlert(); });
    AddTextButton(
        alert_bar_, done ? text::kRead : "Hermes",
        [this, done]() {
            CloseAlert();
            if (done && hooks_.speak_hermes_result) {
                hooks_.speak_hermes_result();
            } else {
                Open(Page::kHermes);
            }
        },
        true);
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

#else  // !METAOS_PANEL_ENABLED

struct MetaPanel::Button {};

MetaPanel::MetaPanel(Storage& storage, MusicPlayer& music, ReminderStore& reminders,
                     HermesClient& hermes, MetaSettings& settings, Hooks hooks)
    : storage_(storage),
      music_(music),
      reminders_(reminders),
      hermes_(hermes),
      settings_(settings),
      hooks_(std::move(hooks)) {}
MetaPanel::~MetaPanel() = default;
void MetaPanel::Toggle() {}
void MetaPanel::Open(Page) {}
void MetaPanel::Hide() {}
void MetaPanel::Refresh() {}
void MetaPanel::ShowReminderAlert(const std::vector<Reminder>&) {}
void MetaPanel::ShowHermesAlert() {}
void MetaPanel::CloseAlert() {}
void MetaPanel::ShowToast(const std::string&) {}

#endif  // METAOS_PANEL_ENABLED

}  // namespace metaos
