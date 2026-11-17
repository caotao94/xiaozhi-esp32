#include "lichuang_display.h"

#if CONFIG_LV_FONT_MONTSERRAT_10

#include "board.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_log.h>

#define TAG "LichuangDisplay"

namespace {

// Widget geometry: 30x16 outer, 1 px border + 1 px padding -> 26x12 content.
// Matches the 20 px icon-font status bar (network/mute icons) in height.
constexpr int kWidgetWidth = 30;
constexpr int kWidgetHeight = 16;
constexpr int kContentWidth = kWidgetWidth - 4;
constexpr int kLowBatteryPercent = 20;

// Lightning bolt zigzag (~6x10 px), drawn with lv_line so no extra font glyph
// is needed (the vendor icon-font subset has no standalone bolt symbol).
const lv_point_precise_t kBoltPoints[] = {{5, 0}, {1, 5}, {4, 5}, {0, 10}};

}  // namespace

void LichuangLcdDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    CreateBatteryWidget();
}

void LichuangLcdDisplay::CreateBatteryWidget() {
    if (battery_widget_ != nullptr || battery_label_ == nullptr) {
        return;  // already created, or base UI has no battery label
    }
    DisplayLockGuard lock(this);
    lv_obj_t* right_icons = lv_obj_get_parent(battery_label_);

    // Keep battery_label_ alive (SetTheme restyles it; low-battery popup logic
    // in the base class still tracks its icon) but hide it visually.
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);

    battery_widget_ = lv_obj_create(right_icons);
    lv_obj_remove_style_all(battery_widget_);
    lv_obj_set_size(battery_widget_, kWidgetWidth, kWidgetHeight);
    lv_obj_set_style_border_width(battery_widget_, 1, 0);
    lv_obj_set_style_radius(battery_widget_, 3, 0);
    lv_obj_set_style_bg_opa(battery_widget_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(battery_widget_, 1, 0);
    lv_obj_set_style_margin_left(battery_widget_, 4, 0);
    lv_obj_clear_flag(battery_widget_, LV_OBJ_FLAG_SCROLLABLE);

    battery_fill_ = lv_obj_create(battery_widget_);
    lv_obj_remove_style_all(battery_fill_);
    lv_obj_set_size(battery_fill_, 0, LV_PCT(100));
    lv_obj_set_style_radius(battery_fill_, 2, 0);
    lv_obj_set_style_bg_opa(battery_fill_, LV_OPA_40, 0);
    lv_obj_align(battery_fill_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(battery_fill_, LV_OBJ_FLAG_SCROLLABLE);

    battery_num_ = lv_label_create(battery_widget_);
    lv_obj_set_style_text_font(battery_num_, &lv_font_montserrat_10, 0);
    lv_label_set_text(battery_num_, "");
    lv_obj_align(battery_num_, LV_ALIGN_CENTER, 0, 0);

    battery_bolt_ = lv_line_create(battery_widget_);
    lv_line_set_points(battery_bolt_, kBoltPoints, sizeof(kBoltPoints) / sizeof(kBoltPoints[0]));
    lv_obj_set_style_line_width(battery_bolt_, 2, 0);
    lv_obj_set_style_line_rounded(battery_bolt_, true, 0);
    lv_obj_align(battery_bolt_, LV_ALIGN_RIGHT_MID, -1, 0);
    lv_obj_add_flag(battery_bolt_, LV_OBJ_FLAG_HIDDEN);

    // Terminal nub as a flex sibling so it is not clipped by the widget bounds.
    battery_nub_ = lv_obj_create(right_icons);
    lv_obj_remove_style_all(battery_nub_);
    lv_obj_set_size(battery_nub_, 2, 6);
    lv_obj_set_style_radius(battery_nub_, 1, 0);
    lv_obj_set_style_bg_opa(battery_nub_, LV_OPA_COVER, 0);

    ApplyWidgetTheme();
    // Hidden until the first valid gauge reading arrives.
    lv_obj_add_flag(battery_widget_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(battery_nub_, LV_OBJ_FLAG_HIDDEN);
}

void LichuangLcdDisplay::ApplyWidgetTheme() {
    if (battery_widget_ == nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (theme == nullptr) {
        return;
    }
    lv_color_t fg = theme->text_color();
    lv_obj_set_style_border_color(battery_widget_, fg, 0);
    lv_obj_set_style_bg_color(battery_nub_, fg, 0);
    lv_obj_set_style_text_color(battery_num_, fg, 0);
    lv_obj_set_style_line_color(battery_bolt_, fg, 0);
    lv_obj_set_style_bg_color(battery_fill_, fg, 0);
}

void LichuangLcdDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    ApplyWidgetTheme();
    RenderBattery();
}

void LichuangLcdDisplay::UpdateStatusBar(bool update_all) {
    SpiLcdDisplay::UpdateStatusBar(update_all);
    RenderBattery();
}

void LichuangLcdDisplay::RenderBattery() {
    if (battery_widget_ == nullptr) {
        return;
    }
    // Cached telemetry ONLY (no I2C, no driver state transitions): this render
    // path also runs from SetTheme, which may execute on the LVGL task, while
    // the gauge driver is owned by the main task's UpdateStatusBar tick.
    BatteryInfo battery;
    bool ok = Board::GetInstance().GetBatteryInfo(battery);

    DisplayLockGuard lock(this);
    if (!ok) {
        lv_obj_add_flag(battery_widget_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_nub_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(battery_widget_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(battery_nub_, LV_OBJ_FLAG_HIDDEN);

    int level = battery.level;
    if (level < 0) {
        level = 0;
    } else if (level > 100) {
        level = 100;
    }
    lv_obj_set_width(battery_fill_, kContentWidth * level / 100);

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    lv_color_t fg = theme != nullptr ? theme->text_color() : lv_color_white();
    if (battery.charging) {
        lv_obj_set_style_bg_color(battery_fill_, lv_palette_main(LV_PALETTE_GREEN), 0);
        lv_obj_set_style_bg_opa(battery_fill_, LV_OPA_70, 0);
    } else if (battery.discharging && level < kLowBatteryPercent) {
        lv_obj_set_style_bg_color(battery_fill_, lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_bg_opa(battery_fill_, LV_OPA_70, 0);
    } else {
        lv_obj_set_style_bg_color(battery_fill_, fg, 0);
        lv_obj_set_style_bg_opa(battery_fill_, LV_OPA_40, 0);
    }

    lv_label_set_text_fmt(battery_num_, "%d", level);
    if (battery.charging) {
        lv_obj_clear_flag(battery_bolt_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(battery_num_, LV_ALIGN_LEFT_MID, 1, 0);
    } else {
        lv_obj_add_flag(battery_bolt_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(battery_num_, LV_ALIGN_CENTER, 0, 0);
    }
}

#endif  // CONFIG_LV_FONT_MONTSERRAT_10
