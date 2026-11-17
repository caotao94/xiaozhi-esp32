#ifndef BOARDS_LCKFB_SZPI_ESP32S3_LICHUANG_DISPLAY_H_
#define BOARDS_LCKFB_SZPI_ESP32S3_LICHUANG_DISPLAY_H_

#include "sdkconfig.h"

#if CONFIG_LV_FONT_MONTSERRAT_10

#include "display/lcd_display.h"

// Board-local LCD display with a custom status-bar battery widget:
// battery outline + proportional fill + embedded percentage number; green fill
// and a small drawn lightning bolt while charging, red fill when low.
//
// All customization lives in the board layer: core display classes are NOT
// modified (battery_label_ is shared by other boards). The base battery_label_
// is still created by LcdDisplay::SetupUI and restyled by SetTheme — we only
// hide it and render our widget next to it.
//
// Requires CONFIG_LV_FONT_MONTSERRAT_10 for the small embedded digits; boards
// without it fall back to the standard icon-only battery label.
class LichuangLcdDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void UpdateStatusBar(bool update_all = false) override;

private:
    void CreateBatteryWidget();
    void ApplyWidgetTheme();
    void RenderBattery();

    lv_obj_t* battery_widget_ = nullptr;
    lv_obj_t* battery_nub_ = nullptr;
    lv_obj_t* battery_fill_ = nullptr;
    lv_obj_t* battery_num_ = nullptr;
    lv_obj_t* battery_bolt_ = nullptr;
};

#endif  // CONFIG_LV_FONT_MONTSERRAT_10
#endif  // BOARDS_LCKFB_SZPI_ESP32S3_LICHUANG_DISPLAY_H_
