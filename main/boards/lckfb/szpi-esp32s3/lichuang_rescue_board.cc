#include "sdkconfig.h"

#if CONFIG_METAOS_RESCUE_MODE

#include "lichuang_rescue_board.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_lvgl_port_disp.h>
#include <esp_lvgl_port_touch.h>

#include "config.h"

#define TAG "RescueBoard"

RescueBoard::~RescueBoard() {
    delete sd_;
    delete pca9557_;
    // backlight_ is a static instance (non-virtual destructor), not owned here.
}

bool RescueBoard::Initialize() {
    if (!InitI2c()) {
        return false;
    }
    if (!InitDisplay()) {
        return false;
    }
    InitTouch();

    static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
    backlight_ = &backlight;
    backlight_->RestoreBrightness();

    sd_ = new SdmmcCard({
        .mount_point = SD_CARD_MOUNT_POINT,
        .clk = SD_CARD_PIN_CLK,
        .cmd = SD_CARD_PIN_CMD,
        .d0 = SD_CARD_PIN_D0,
    });
    // Mounting is done exclusively by the rescue worker task (SdmmcCard requires
    // Mount() to stay on a single task); see rescue_ui.cc WorkerTask.
    return true;
}

bool RescueBoard::InitI2c() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_1;
    bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
    bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_cfg, &i2c_bus_) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }
    pca9557_ = new RescuePca9557(i2c_bus_, 0x19);
    // Rescue uses no audio/camera: disable PA (bit1) and camera power (bit2).
    pca9557_->SetOutputState(1, 0);
    pca9557_->SetOutputState(2, 1);
    return true;
}

bool RescueBoard::InitDisplay() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = GPIO_NUM_40;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = GPIO_NUM_41;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    if (spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = GPIO_NUM_NC;  // CS driven by PCA9557 bit0
    io_config.dc_gpio_num = GPIO_NUM_39;
    io_config.spi_mode = 2;
    io_config.pclk_hz = 80 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_) != ESP_OK) {
        ESP_LOGE(TAG, "panel IO init failed");
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_) != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 init failed");
        return false;
    }

    esp_lcd_panel_reset(panel_);
    pca9557_->SetOutputState(0, 0);  // LCD CS low
    esp_lcd_panel_init(panel_);
    esp_lcd_panel_invert_color(panel_, true);
    esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel_, true);

    // LVGL via esp_lvgl_port (same stack as the main app, no LcdDisplay/theme).
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl port init failed");
        return false;
    }
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(DISPLAY_WIDTH * 20),
        .double_buffer = false,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation =
            {
                .swap_xy = DISPLAY_SWAP_XY,
                .mirror_x = DISPLAY_MIRROR_X,
                .mirror_y = DISPLAY_MIRROR_Y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = false,
                .swap_bytes = true,
            },
    };
    lv_disp_ = lvgl_port_add_disp(&disp_cfg);
    if (lv_disp_ == nullptr) {
        ESP_LOGE(TAG, "lvgl display add failed");
        return false;
    }
    return true;
}

void RescueBoard::InitTouch() {
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_HEIGHT,
        .y_max = DISPLAY_WIDTH,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 1, .mirror_x = 1, .mirror_y = 0},
    };
    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .flags = {.disable_control_phase = 1},
    };
    tp_io_config.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io) != ESP_OK) {
        ESP_LOGW(TAG, "touch IO failed, continuing without touch");
        return;
    }
    if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &touch_) != ESP_OK || touch_ == nullptr) {
        ESP_LOGW(TAG, "FT5x06 not found, continuing without touch");
        esp_lcd_panel_io_del(tp_io);
        return;
    }
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lv_disp_,
        .handle = touch_,
    };
    lv_touch_ = lvgl_port_add_touch(&touch_cfg);
    if (lv_touch_ == nullptr) {
        ESP_LOGW(TAG, "lvgl touch add failed");
        esp_lcd_touch_del(touch_);
        touch_ = nullptr;
    }
}

// Exports exactly one board factory for this build (AGENTS.md). RescueBoard is
// standalone; it does not inherit the full Board framework. Defined locally to
// avoid pulling board.h (and the main app's protocol/assets API surface) in.
void* create_board() { return new RescueBoard(); }

#endif  // CONFIG_METAOS_RESCUE_MODE
