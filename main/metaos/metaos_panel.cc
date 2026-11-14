#include "metaos_panel.h"

#include <sdkconfig.h>

#include "display.h"
#include "metaos_reminders.h"

#if HAVE_LVGL && !CONFIG_USE_EMOTE_MESSAGE_STYLE
#define METAOS_PANEL_ENABLED 1
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <material_symbols.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

#include "application.h"
#include "board.h"
#include "display/lvgl_display/gif/lvgl_gif.h"
#include "firmware_install.h"
#include "firmware_repo.h"
#include "lvgl_theme.h"
#include "metaos_common.h"
#include "metaos_files.h"
#include "metaos_firmware.h"
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
// Firmware confirmations time out to "cancel" if the user walks away.
constexpr int kFwDialogMs = 30 * 1000;
constexpr size_t kTracksPerPage = 30;
// 文本分页：每页目标字节数（页尾对齐到换行/UTF-8 边界，实际长度可变）。
constexpr size_t kTextPageSize = 6 * 1024;
// 文件总大小的宽松上限，只防极端文件；分页读取本身内存恒定。
constexpr long kTextMaxSize = 16 * 1024 * 1024;

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

const esp_partition_t* OtaPartition(int slot) {
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        slot == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
}

// Running partition, next boot and the state of all three slots, one line each.
std::string FirmwareOverview() {
    firmware::SlotInfo running = firmware::FirmwareInstall::Running();
    firmware::SlotInfo boot = firmware::FirmwareInstall::BootSelected();
    std::string text = "运行: " + running.label;
    if (!running.version.empty()) {
        text += " · " + running.version;
    }
    text += "\n下次启动: " + boot.label;
    for (const auto& slot : firmware::FirmwareInstall::Slots()) {
        text += "\n" + slot.label + ": ";
        if (slot.part == nullptr) {
            text += "不存在";
            continue;
        }
        if (slot.valid_image) {
            text += slot.version.empty() ? "未知版本" : slot.version.c_str();
        } else {
            text += "无有效镜像";
        }
        text += std::string(" (") + firmware::FirmwareInstall::StateName(slot.state) + ")";
    }
    return text;
}

// Fingerprint of the repository list plus the last scan/install error. RefreshFirmware
// rebuilds the page only when a rescan changed something (same entry count with
// different content included, or a new error on an empty repository).
unsigned FirmwareRevision(const std::vector<firmware::FirmwareEntry>& entries,
                          const std::string& last_error) {
    unsigned revision = static_cast<unsigned>(entries.size());
    auto fold = [&revision](const std::string& text) {
        for (char c : text) {
            revision = revision * 31u + static_cast<unsigned char>(c);
        }
    };
    fold(last_error);
    for (const auto& entry : entries) {
        fold(entry.name);
        fold(entry.version);
        fold(entry.error);
        fold(entry.bin_path);
        revision = revision * 31u + static_cast<unsigned>(entry.size);
        revision = revision * 31u + (entry.has_signature ? 1u : 0u);
        revision = revision * 31u + (entry.sha256_ok ? 1u : 0u);
    }
    return revision;
}

// Visually mute a button that must not be tapped (running slot, missing partition).
// LVGL does not deliver clicks to disabled objects.
void DisableButton(void* button, LvglTheme* theme) {
    lv_obj_t* obj = Obj(button);
    lv_obj_add_state(obj, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(obj, SecondaryText(theme), LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(obj, LV_OPA_50, LV_STATE_DISABLED);
}

// 文本分页读取结果。error 为 kNone 时 content/next_offset/size 有效。
enum class TextPageError { kNone, kOpen, kRead, kTooLarge };

struct TextPage {
    std::string content;
    long next_offset = 0;  // 下一页起始偏移
    long size = 0;         // 文件总大小
    TextPageError error = TextPageError::kNone;
};

// 工作线程执行：打开文件、从 offset 读取一页、立即关闭，不跨任务持有 FILE*。
// 页尾优先对齐到块内最后一个换行符；没有换行（超长单行）时按 UTF-8 字符边界硬切。
TextPage ReadTextPage(const std::string& path, long offset) {
    TextPage page;
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        page.error = TextPageError::kOpen;
        return page;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        page.error = TextPageError::kRead;
        return page;
    }
    page.size = size;
    if (size > kTextMaxSize) {
        fclose(f);
        page.error = TextPageError::kTooLarge;
        return page;
    }
    if (offset >= size) {
        // 空文件（或文件已变小）：返回空的末页。
        fclose(f);
        page.next_offset = size;
        return page;
    }
    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        page.error = TextPageError::kRead;
        return page;
    }
    // 多读 4 个字节，用于检查切点处的字符是否完整（UTF-8 字符最长 4 字节）。
    std::string buf(kTextPageSize + 4, '\0');
    size_t n = fread(&buf[0], 1, buf.size(), f);
    fclose(f);
    if (n == 0) {
        page.error = TextPageError::kRead;
        return page;
    }
    size_t end = n;
    if (n == buf.size() && offset + static_cast<long>(n) < size) {
        // 后面还有内容：尽量让页尾落在完整的一行之后。
        size_t newline = buf.rfind('\n', kTextPageSize - 1);
        // 仅当换行对齐不会把页缩得过小（<半页）时才采用，避免稀疏换行文件产生
        // 极小页（如唯一换行在位置 0 时只含 1 字节）。否则按 UTF-8 边界硬切。
        if (newline != std::string::npos && newline + 1 >= kTextPageSize / 2) {
            // '\n' 是 ASCII，不会出现在多字节字符中间，行尾切分天然安全。
            end = newline + 1;
        } else {
            // 超长单行/稀疏换行：硬切。若切点落在多字节字符中间（buf[end] 是后续字节），
            // 回退到字符起始处；最多回退 3 字节，保证非法 UTF-8 数据也能前进。
            end = kTextPageSize;
            int backoff = 0;
            while (end > 0 && backoff < 3 &&
                   (static_cast<unsigned char>(buf[end]) & 0xC0) == 0x80) {
                --end;
                ++backoff;
            }
        }
    }
    page.content = buf.substr(0, end);
    page.next_offset = offset + static_cast<long>(end);
    return page;
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
                     MetaSettings& settings, FirmwareManager& firmware, Hooks hooks)
    : storage_(storage),
      music_(music),
      reminders_(reminders),
      file_browser_(
          new FileBrowser(storage, {.on_file_changed = [this]() { Open(Page::kFiles); }})),
      settings_(settings),
      firmware_(firmware),
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
    fw_status_area_ = nullptr;
    fw_list_area_ = nullptr;
    fw_progress_bar_ = nullptr;
    fw_progress_label_ = nullptr;
    fw_cancel_button_ = nullptr;
    // fw_selected_index_ is not a widget pointer: it survives page rebuilds and is
    // reset when the repository content changes.
    // 文本分页控件随查看器一起销毁，这里只清引用；数值状态在 CloseFileViewer 里复位。
    text_label_ = nullptr;
    text_prev_button_ = nullptr;
    text_next_button_ = nullptr;
    text_position_label_ = nullptr;
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
        case Page::kFirmware:
            BuildFirmware();
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
        case Page::kFirmware:
            RefreshFirmware();
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

    // USB U-disk: expose the TF card to a PC. Only shown when the feature is available.
    if (hooks_.usb_msc_available && hooks_.usb_msc_available()) {
        bool usb_on = hooks_.usb_msc_active && hooks_.usb_msc_active();
        AddSwitchRow(area, text::kUsbMsc, usb_on, [this](bool enabled) {
            if (hooks_.usb_msc_set) {
                hooks_.usb_msc_set(enabled);
            }
        });
    }

    AddLabel(area, text::kVoiceHint, true);

    // Firmware management: install / switch images from the TF-card repository.
    AddTextButton(area, "固件管理", [this]() { Open(Page::kFirmware); });
}

void MetaPanel::BuildFirmware() {
    const auto& entries = firmware_.entries();
    shown_revision_ = FirmwareRevision(entries, firmware_.last_error());
    if (fw_selected_index_ >= static_cast<int>(entries.size())) {
        fw_selected_index_ = -1;
    }
    AddHeader(MATERIAL_SYMBOLS_MEMORY, "固件管理");
    lv_obj_t* area = Obj(AddScrollArea());
    list_area_ = area;

    // Slot overview: running partition, next boot and all three slots.
    fw_status_area_ = Box(area);
    lv_obj_set_width(Obj(fw_status_area_), LV_PCT(100));
    lv_obj_set_height(Obj(fw_status_area_), LV_SIZE_CONTENT);
    AddLabel(fw_status_area_, FirmwareOverview());

    // Second confirmation for a destructive install: overwrite one OTA slot. The
    // heavy verify/write work runs on the FirmwareManager worker, never here.
    auto confirm_target = [this](const firmware::FirmwareEntry& entry, int slot) {
        LvglLock lock;
        if (!lock) {
            return;
        }
        theme_ = CurrentTheme();
        if (theme_ == nullptr || root_ == nullptr || page_ != Page::kFirmware) {
            return;
        }
        if (firmware_.busy()) {
            ShowToast("任务进行中，请稍候");
            return;
        }
        const esp_partition_t* target = OtaPartition(slot);
        if (target == nullptr) {
            ShowToast("目标分区不存在");
            return;
        }
        std::string body = "将覆盖 " + std::string(target->label) + " 分区：\n" + entry.name +
                           "  " + entry.version + "\n安装过程中请勿断电或拔卡。";
        OpenAlert(MATERIAL_SYMBOLS_WARNING, "确认安装", body, kFwDialogMs);
        lv_obj_set_flex_flow(Obj(alert_bar_), LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(Obj(alert_bar_), 6, 0);
        AddTextButton(
            alert_bar_, "确认安装",
            [this, entry, target]() {
                CloseAlert();
                if (!firmware_.RequestInstall(entry, target)) {
                    ShowToast("无法开始安装");
                }
            },
            true);
        AddTextButton(alert_bar_, text::kCancel, [this]() { CloseAlert(); });
        building_buttons_ = &page_buttons_;
    };

    // Boot switch confirmation. Switch() writes otadata (a brief flash write, same
    // as the rescue firmware) and the device reboots right after.
    auto confirm_switch = [this](const esp_partition_t* target) {
        LvglLock lock;
        if (!lock) {
            return;
        }
        theme_ = CurrentTheme();
        if (theme_ == nullptr || root_ == nullptr || page_ != Page::kFirmware) {
            return;
        }
        if (firmware_.busy()) {
            ShowToast("任务进行中，不能切换启动分区");
            return;
        }
        if (target == nullptr) {
            ShowToast("目标分区不存在");
            return;
        }
        std::string body = "下次启动将进入 " + std::string(target->label) + "，确认后立即重启。";
        for (const auto& info : firmware::FirmwareInstall::Slots()) {
            if (info.part != nullptr && info.part->address == target->address &&
                !info.valid_image) {
                body += "\n警告：该分区没有有效镜像，切换后可能无法启动！";
            }
        }
        OpenAlert(MATERIAL_SYMBOLS_WARNING, "切换启动分区", body, kFwDialogMs);
        lv_obj_set_flex_flow(Obj(alert_bar_), LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(Obj(alert_bar_), 6, 0);
        AddTextButton(
            alert_bar_, "重启并切换",
            [this, target]() {
                CloseAlert();
                // A job may have started since the dialog opened; never write
                // otadata or reboot while the worker is using the flash.
                if (firmware_.busy()) {
                    ShowToast("任务进行中，稍后再试");
                    return;
                }
                std::string err;
                if (firmware::FirmwareInstall::Switch(target, err)) {
                    esp_restart();
                } else {
                    ShowToast(err);
                }
            },
            true);
        AddTextButton(alert_bar_, text::kCancel, [this]() { CloseAlert(); });
        building_buttons_ = &page_buttons_;
    };

    // TF-card repository.
    AddLabel(area, "TF 卡仓库", true);
    AddTextButton(area, "扫描仓库", [this]() {
        if (firmware_.busy()) {
            ShowToast("任务进行中，请稍候");
            return;
        }
        firmware_.RequestScan();
    });
    fw_list_area_ = Box(area);
    lv_obj_set_width(Obj(fw_list_area_), LV_PCT(100));
    lv_obj_set_height(Obj(fw_list_area_), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(Obj(fw_list_area_), LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(Obj(fw_list_area_), 6, 0);
    if (entries.empty()) {
        AddLabel(fw_list_area_,
                 storage_.IsMounted() ? "仓库为空：把固件放入 TF 卡 metaos/firmware 目录"
                                      : text::kNoCard,
                 true);
        if (!firmware_.last_error().empty()) {
            lv_obj_t* error_label = Obj(AddLabel(fw_list_area_, "错误: " + firmware_.last_error()));
            lv_obj_set_style_text_color(error_label, lv_color_hex(0xE53935), 0);
        }
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        std::string detail = FileBrowser::FormatFileSize(static_cast<size_t>(entry.size));
        detail += entry.has_signature ? " · [已签名]" : " · [未签名]";
        if (!entry.error.empty()) {
            detail += " · [无效]";
        }
        AddListRow(
            fw_list_area_, MATERIAL_SYMBOLS_DOWNLOAD, entry.name + "  " + entry.version, detail,
            [this, i, entry, confirm_target]() {
                fw_selected_index_ = static_cast<int>(i);
                LvglLock lock;
                if (!lock) {
                    return;
                }
                theme_ = CurrentTheme();
                if (theme_ == nullptr || root_ == nullptr || page_ != Page::kFirmware) {
                    return;
                }
                if (!entry.error.empty()) {
                    OpenAlert(MATERIAL_SYMBOLS_WARNING, "固件无效", entry.error, kFwDialogMs);
                    AddTextButton(alert_bar_, text::kGotIt, [this]() { CloseAlert(); }, true);
                    building_buttons_ = &page_buttons_;
                    return;
                }
                // First confirmation: choose the target slot. The running slot is
                // protected and its button is disabled.
                // The scan only records whether a signature is declared; the actual
                // signature check happens at install time, so say exactly that.
                std::string body =
                    entry.name + "  " + entry.version +
                    "\n大小: " + FileBrowser::FormatFileSize(static_cast<size_t>(entry.size)) +
                    "\n" + (entry.has_signature ? "已签名（安装时验签）" : "未签名") +
                    "\n安装时校验 SHA-256 与签名，无效签名一律拒绝";
#if CONFIG_METAOS_FW_REQUIRE_SIGNATURE
                if (!entry.has_signature) {
                    body += "\n当前策略要求签名，未签名将被拒绝";
                }
#endif
                body += "\n选择安装目标分区：";
                OpenAlert(MATERIAL_SYMBOLS_DOWNLOAD, "安装固件", body, kFwDialogMs);
                lv_obj_set_flex_flow(Obj(alert_bar_), LV_FLEX_FLOW_ROW_WRAP);
                lv_obj_set_style_pad_row(Obj(alert_bar_), 6, 0);
                const esp_partition_t* running = esp_ota_get_running_partition();
                for (int slot = 0; slot < 2; ++slot) {
                    const esp_partition_t* target = OtaPartition(slot);
                    std::string label = slot == 0 ? "装到 ota_0" : "装到 ota_1";
                    bool disabled = false;
                    if (target == nullptr) {
                        label += "（不存在）";
                        disabled = true;
                    } else if (running != nullptr && target->address == running->address) {
                        label += "（运行中）";
                        disabled = true;
                    }
                    lv_obj_t* button =
                        Obj(AddTextButton(alert_bar_, label, [this, entry, slot, confirm_target]() {
                            confirm_target(entry, slot);
                        }));
                    if (disabled) {
                        DisableButton(button, theme_);
                    }
                }
                AddTextButton(alert_bar_, text::kCancel, [this]() { CloseAlert(); });
                building_buttons_ = &page_buttons_;
            },
            static_cast<int>(i) == fw_selected_index_);
    }

    // Boot switching.
    AddLabel(area, "启动切换", true);
    lv_obj_t* switch_row = Box(area);
    lv_obj_set_width(switch_row, LV_PCT(100));
    lv_obj_set_height(switch_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(switch_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(switch_row, 8, 0);
    lv_obj_set_style_pad_row(switch_row, 6, 0);
    const esp_partition_t* running = esp_ota_get_running_partition();
    for (int slot = 0; slot < 2; ++slot) {
        const esp_partition_t* target = OtaPartition(slot);
        std::string label = slot == 0 ? "启动 ota_0" : "启动 ota_1";
        bool disabled = false;
        if (target == nullptr) {
            label += "（不存在）";
            disabled = true;
        } else if (running != nullptr && target->address == running->address) {
            label += "（运行中）";
            disabled = true;
        }
        lv_obj_t* button = Obj(AddTextButton(
            switch_row, label, [this, target, confirm_switch]() { confirm_switch(target); }));
        if (disabled) {
            DisableButton(button, theme_);
        }
    }
    // Enter the factory rescue system.
    const esp_partition_t* factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    std::string rescue_label = "进入救援模式";
    bool rescue_disabled = false;
    if (factory == nullptr) {
        rescue_label += "（不存在）";
        rescue_disabled = true;
    } else if (running != nullptr && factory->address == running->address) {
        rescue_label += "（运行中）";
        rescue_disabled = true;
    }
    lv_obj_t* rescue_button = Obj(AddTextButton(
        switch_row, rescue_label,
        [this, factory, confirm_switch]() { confirm_switch(factory); }));
    if (rescue_disabled) {
        DisableButton(rescue_button, theme_);
    }

    // Progress. The cancel button is tracked in fw_cancel_button_; RefreshFirmware
    // shows it only while verify/write is running (a scan cannot be cancelled).
    fw_progress_bar_ = lv_bar_create(area);
    lv_obj_set_width(Obj(fw_progress_bar_), LV_PCT(100));
    lv_obj_set_height(Obj(fw_progress_bar_), 10);
    lv_bar_set_range(Obj(fw_progress_bar_), 0, 100);
    lv_bar_set_value(Obj(fw_progress_bar_), 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(Obj(fw_progress_bar_), Accent(), LV_PART_INDICATOR);
    fw_progress_label_ = AddLabel(area, "空闲");
    fw_cancel_button_ = AddTextButton(area, text::kCancel, [this]() { firmware_.Cancel(); });
    lv_obj_add_flag(Obj(fw_cancel_button_), LV_OBJ_FLAG_HIDDEN);
}

void MetaPanel::RefreshFirmware() {
    if (fw_progress_bar_ == nullptr || fw_progress_label_ == nullptr || list_area_ == nullptr) {
        return;
    }
    // Consume a finished install exactly once (Poll already moved the worker result
    // into main-task state). An empty error means success.
    if (firmware_.has_result()) {
        firmware::FirmwareEntry entry = firmware_.installed();
        const esp_partition_t* target = firmware_.install_target();
        std::string error = firmware_.last_error();
        firmware_.AcknowledgeResult();
        if (error.empty()) {
            std::string message = "安装成功";
            if (!entry.name.empty()) {
                message += ": " + entry.name;
            }
            if (target != nullptr) {
                message += std::string("，已写入 ") + target->label;
            }
            ShowToast(message);
        } else {
            if (theme_ == nullptr) {
                theme_ = CurrentTheme();
            }
            if (theme_ != nullptr) {
                OpenAlert(MATERIAL_SYMBOLS_WARNING, "安装失败", error, kFwDialogMs);
                AddTextButton(alert_bar_, text::kGotIt, [this]() { CloseAlert(); }, true);
                building_buttons_ = &page_buttons_;
            } else {
                ShowToast("安装失败: " + error);
            }
        }
    }

    // Progress bar + phase label. Cancel is only offered during verify/write:
    // Cancel() cannot interrupt a scan, and the terminal states need no cancel.
    lv_obj_t* bar = Obj(fw_progress_bar_);
    lv_obj_t* cancel = Obj(fw_cancel_button_);
    std::string progress = "空闲";
    int percent = 0;
    bool cancellable = false;
    if (firmware_.busy()) {
        percent = firmware_.percent();
        switch (firmware_.phase()) {
            case FirmwareManager::Phase::kScanning:
                progress = "扫描中...";
                percent = 0;  // scans have no byte progress
                break;
            case FirmwareManager::Phase::kVerify:
                progress = "校验 SHA-256... " + std::to_string(percent) + "%";
                cancellable = true;
                break;
            case FirmwareManager::Phase::kWrite:
                progress = "写入 " + std::to_string(percent) + "%";
                cancellable = true;
                break;
            default:
                break;
        }
    }
    if (cancel != nullptr) {
        if (cancellable) {
            lv_obj_remove_flag(cancel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(cancel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (lv_bar_get_value(bar) != percent) {
        lv_bar_set_value(bar, percent, LV_ANIM_OFF);
    }
    SetLabel(fw_progress_label_, progress);

    // Rebuild only when a rescan changed the repository; Open keeps the scroll position
    // and re-runs RefreshFirmware with the new revision recorded, so this does not loop.
    unsigned revision = FirmwareRevision(firmware_.entries(), firmware_.last_error());
    if (revision != shown_revision_) {
        fw_selected_index_ = -1;
        Open(Page::kFirmware);
        return;
    }

    // Overview: reading slot state touches flash, so keep the previous text while
    // a job is running. Otherwise SetLabel skips unchanged text, so this is cheap.
    if (!firmware_.busy() && fw_status_area_ != nullptr) {
        SetLabel(lv_obj_get_child(Obj(fw_status_area_), 0), FirmwareOverview());
    }
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
    // Scroll the whole card so the action buttons stay reachable even when the
    // body text is tall (otherwise they get clipped below the card on 240px).
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
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

    // Body is a plain label container; the card itself scrolls (single scroll
    // container avoids nested-scroll touch conflicts and keeps buttons reachable).
    lv_obj_t* body_area = Box(card);
    lv_obj_set_width(body_area, LV_PCT(100));
    lv_obj_set_height(body_area, LV_SIZE_CONTENT);
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
    building_buttons_ = &viewer_buttons_;  // 查看器按钮单独存放，关闭时立即释放
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
    building_buttons_ = &page_buttons_;

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
        int64_t start_time = esp_timer_get_time();
        auto decoded = DecodeImage(path_copy, 320, 240, error);
        int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;

        if (elapsed_ms > 10000) {
            ESP_LOGW("MetaPanel", "Image decode took %lld ms: %s", elapsed_ms, path_copy.c_str());
        }

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
                    } else if (error == ImageError::kNoMemory) {
                        msg = "内存不足";
                    } else if (error == ImageError::kTooLarge) {
                        msg = "文件过大";
                    } else if (error == ImageError::kCorrupt) {
                        msg = "文件损坏";
                    }
                    lv_label_set_text(error_label, msg);
                    lv_obj_set_style_text_color(error_label, lv_color_hex(0xE53935), 0);
                    ESP_LOGE("MetaPanel", "Image decode failed: %s, error=%d", path_copy.c_str(),
                             (int)error);
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

    // 复位分页状态；会话号递增，用于丢弃上一次打开遗留的翻页结果。
    text_offset_ = 0;
    text_next_offset_ = 0;
    text_size_ = 0;
    text_history_.clear();
    text_loading_ = false;
    ++text_session_;

    // Create viewer UI with loading indicator first
    lv_obj_t* viewer = lv_obj_create(lv_layer_top());
    file_viewer_ = viewer;
    building_buttons_ = &viewer_buttons_;  // 查看器按钮单独存放，关闭时立即释放
    lv_obj_remove_style_all(viewer);
    lv_obj_set_size(viewer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(viewer, theme_->background_color(), 0);
    lv_obj_set_style_bg_opa(viewer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(viewer, 8, 0);
    lv_obj_set_style_pad_row(viewer, 6, 0);
    lv_obj_set_flex_flow(viewer, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(viewer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(viewer, theme_->text_font()->font(), 0);
    lv_obj_set_style_text_color(viewer, theme_->text_color(), 0);
    lv_obj_add_flag(viewer, LV_OBJ_FLAG_CLICKABLE);

    // Header
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

    // Content area with loading message
    lv_obj_t* scroll_area = Box(viewer);
    lv_obj_add_flag(scroll_area,
                    static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_width(scroll_area, LV_PCT(100));
    lv_obj_set_flex_grow(scroll_area, 1);
    lv_obj_set_scroll_dir(scroll_area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_area, LV_SCROLLBAR_MODE_ACTIVE);
    file_viewer_content_ = scroll_area;

    lv_obj_t* loading_label = lv_label_create(scroll_area);
    lv_label_set_text(loading_label, "加载中...");
    lv_obj_set_style_text_color(loading_label, SecondaryText(theme_), 0);
    loading_spinner_ = loading_label;

    // 底部导航栏：上一页 / 位置指示 / 下一页。首页禁用上一页，末页禁用下一页。
    lv_obj_t* bar = Box(viewer);
    lv_obj_set_size(bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    text_prev_button_ = AddTextButton(bar, text::kPrevPage, [this]() {
        if (text_loading_ || text_history_.empty()) {
            return;
        }
        long offset = text_history_.back();
        if (LoadTextPage(offset)) {
            text_history_.pop_back();
        }
    });
    text_position_label_ = AddLabel(bar, "...", true);
    // 让位置指示占据剩余宽度并可省略，避免大字体下把"下一页"按钮挤出右缘。
    lv_obj_set_flex_grow(Obj(text_position_label_), 1);
    lv_label_set_long_mode(Obj(text_position_label_), LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(Obj(text_position_label_), LV_TEXT_ALIGN_CENTER, 0);
    text_next_button_ = AddTextButton(bar, text::kNextPage, [this]() {
        if (text_loading_ || text_label_ == nullptr || text_next_offset_ >= text_size_) {
            return;
        }
        if (LoadTextPage(text_next_offset_)) {
            text_history_.push_back(text_offset_);
        }
    });
    DisableButton(text_prev_button_, theme_);
    DisableButton(text_next_button_, theme_);

    // Alerts and toasts stay above the viewer
    if (alert_ != nullptr) {
        lv_obj_move_foreground(Obj(alert_));
    }
    if (toast_ != nullptr) {
        lv_obj_move_foreground(Obj(toast_));
    }

    building_buttons_ = &page_buttons_;
    // 异步读取第一页；队列满/读取失败的提示与关闭逻辑在 LoadTextPage 内。
    LoadTextPage(0);
}

// 主任务。持（递归）LVGL 锁：翻页按钮回调不带锁调用这里。
// 在工作线程读取 offset 处的一页，完成后回主任务更新界面。队列满时返回 false。
bool MetaPanel::LoadTextPage(long offset) {
    LvglLock lock;
    if (!lock || text_loading_) {
        return false;
    }
    std::string path = viewing_file_;
    unsigned session = text_session_;
    text_loading_ = true;
    UpdateTextPager();  // 读取期间禁用两个翻页按钮
    bool posted = PostWork([this, path, offset, session]() mutable {
        auto page = std::make_shared<TextPage>(ReadTextPage(path, offset));
        // apply 自引用以支持取锁失败重试；每个终止路径异步 reset 断开引用环，
        // 避免在执行中销毁自身，也避免泄漏。
        auto apply = std::make_shared<std::function<void()>>();
        *apply = [this, path, offset, session, page, apply]() {
            // 查看器已关闭、换了文件或重新打开过：丢弃过期结果。
            if (file_viewer_ == nullptr || viewing_file_ != path || session != text_session_) {
                Application::GetInstance().Schedule([apply]() { (*apply) = nullptr; });
                return;
            }
            // 先取锁再复位 text_loading_：取锁失败时保持 loading 并重试，
            // 否则按钮会停留在禁用态且无人重新启用，翻页永久卡死。
            LvglLock lock;
            if (!lock) {
                Application::GetInstance().Schedule([apply]() { (*apply)(); });
                return;
            }
            text_loading_ = false;
            if (page->error != TextPageError::kNone) {
                if (page->error == TextPageError::kTooLarge) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "文件太大 (%.1f MB)", page->size / 1048576.0);
                    ShowToast(msg);
                } else if (page->error == TextPageError::kOpen) {
                    ShowToast("无法打开文件");
                } else {
                    ShowToast("读取失败");
                }
                CloseFileViewer();
                Application::GetInstance().Schedule([apply]() { (*apply) = nullptr; });
                return;
            }
            if (loading_spinner_ != nullptr) {
                lv_obj_delete(Obj(loading_spinner_));
                loading_spinner_ = nullptr;
            }
            if (file_viewer_content_ == nullptr) {
                Application::GetInstance().Schedule([apply]() { (*apply) = nullptr; });
                return;
            }
            if (text_label_ == nullptr) {
                lv_obj_t* label = lv_label_create(Obj(file_viewer_content_));
                lv_obj_set_width(label, LV_PCT(100));
                lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
                text_label_ = label;
            }
            lv_label_set_text(Obj(text_label_),
                              page->content.empty() ? "(空文件)" : page->content.c_str());
            // 每页从滚动区顶部开始显示。
            lv_obj_scroll_to_y(Obj(file_viewer_content_), 0, LV_ANIM_OFF);
            text_offset_ = offset;
            text_next_offset_ = page->next_offset;
            text_size_ = page->size;
            if (offset == 0) {
                ESP_LOGI(TAG, "Text file opened: %s, %ld bytes", path.c_str(), page->size);
            }
            UpdateTextPager();
            Application::GetInstance().Schedule([apply]() { (*apply) = nullptr; });
        };
        Application::GetInstance().Schedule([apply]() { (*apply)(); });
    });
    if (!posted) {
        text_loading_ = false;
        UpdateTextPager();
        ShowToast("任务队列已满");
        if (text_label_ == nullptr) {
            // 第一页就发不出去：没有内容可显示，直接关闭。
            CloseFileViewer();
        }
    }
    return posted;
}

// 主任务，需持 LVGL 锁。刷新位置指示与上一页/下一页按钮的可用状态。
void MetaPanel::UpdateTextPager() {
    if (text_position_label_ == nullptr) {
        return;
    }
    if (text_label_ != nullptr) {
        char position[48];
        if (text_size_ < 1024 * 1024) {
            snprintf(position, sizeof(position), "%.1f / %.1f KB", text_offset_ / 1024.0,
                     text_size_ / 1024.0);
        } else {
            snprintf(position, sizeof(position), "%.1f / %.1f MB", text_offset_ / 1048576.0,
                     text_size_ / 1048576.0);
        }
        SetLabel(text_position_label_, position);
    }
    bool prev_enabled = !text_loading_ && !text_history_.empty();
    bool next_enabled = !text_loading_ && text_label_ != nullptr && text_next_offset_ < text_size_;
    if (text_prev_button_ != nullptr) {
        if (prev_enabled) {
            lv_obj_remove_state(Obj(text_prev_button_), LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(Obj(text_prev_button_), LV_STATE_DISABLED);
        }
    }
    if (text_next_button_ != nullptr) {
        if (next_enabled) {
            lv_obj_remove_state(Obj(text_next_button_), LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(Obj(text_next_button_), LV_STATE_DISABLED);
        }
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
        FreeButtons(viewer_buttons_);  // 立即释放查看器按钮，避免文件页内反复开合累积
        file_viewer_ = nullptr;
        file_viewer_content_ = nullptr;
        loading_spinner_ = nullptr;
        viewing_file_.clear();
        // 文本分页：控件已随查看器销毁，复位状态并释放翻页历史占用的内存。
        text_label_ = nullptr;
        text_prev_button_ = nullptr;
        text_next_button_ = nullptr;
        text_position_label_ = nullptr;
        text_offset_ = 0;
        text_next_offset_ = 0;
        text_size_ = 0;
        text_history_.clear();
        text_history_.shrink_to_fit();
        text_loading_ = false;
    }
}

#else  // !METAOS_PANEL_ENABLED

struct MetaPanel::Button {};

MetaPanel::MetaPanel(Storage& storage, MusicPlayer& music, ReminderStore& reminders,
                     MetaSettings& settings, FirmwareManager& firmware, Hooks hooks)
    : storage_(storage),
      music_(music),
      reminders_(reminders),
      settings_(settings),
      firmware_(firmware),
      hooks_(std::move(hooks)) {}
MetaPanel::~MetaPanel() = default;
void MetaPanel::Toggle() {}
void MetaPanel::Open(Page) {}
void MetaPanel::Hide() {}
void MetaPanel::Refresh() {}
void MetaPanel::ShowReminderAlert(const std::vector<Reminder>&) {}
void MetaPanel::CloseAlert() {}
void MetaPanel::ShowToast(const std::string&) {}
void MetaPanel::BuildFirmware() {}
void MetaPanel::RefreshFirmware() {}

#endif  // METAOS_PANEL_ENABLED

}  // namespace metaos
