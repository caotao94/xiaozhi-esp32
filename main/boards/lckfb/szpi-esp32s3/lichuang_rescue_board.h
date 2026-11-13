#pragma once

#include "sdkconfig.h"

#if CONFIG_METAOS_RESCUE_MODE

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <lvgl.h>

#include "backlight.h"
#include "i2c_device.h"
#include "sd_card.h"

// Minimal PCA9557 IO expander (address 0x19 on this board). bit0 = LCD CS,
// bit1 = PA enable, bit2 = camera power. Copied from the main board so the
// rescue firmware stays independent of the full board class.
class RescuePca9557 : public I2cDevice {
public:
    RescuePca9557(i2c_master_bus_handle_t bus, uint8_t addr) : I2cDevice(bus, addr) {
        WriteReg(0x01, 0x03);
        WriteReg(0x03, 0xf8);
    }
    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};

// Standalone rescue board: I2C + PCA9557 + ST7789 LCD + FT5x06 touch + LVGL +
// backlight + SD card. Does NOT use the Board/Application framework, audio,
// camera, networking or assets. One instance per rescue build (DECLARE_BOARD).
class RescueBoard {
public:
    RescueBoard() = default;
    ~RescueBoard();

    // Initializes all hardware. Returns false on a fatal error (LCD/LVGL);
    // touch and SD failures are non-fatal and reported via the accessors.
    bool Initialize();

    lv_display_t* display() const { return lv_disp_; }
    bool touch_ready() const { return lv_touch_ != nullptr; }
    SdmmcCard& storage() { return *sd_; }
    bool MountStorage(bool force = false) { return sd_->Mount(force); }

private:
    bool InitI2c();
    bool InitDisplay();
    void InitTouch();

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    RescuePca9557* pca9557_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    lv_display_t* lv_disp_ = nullptr;
    lv_indev_t* lv_touch_ = nullptr;
    PwmBacklight* backlight_ = nullptr;
    SdmmcCard* sd_ = nullptr;
};

#endif  // CONFIG_METAOS_RESCUE_MODE
