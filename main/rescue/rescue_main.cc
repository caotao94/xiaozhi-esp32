#include "sdkconfig.h"

#if CONFIG_METAOS_RESCUE_MODE

#include "rescue_main.h"

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "lichuang_rescue_board.h"
#include "rescue_ui.h"

#define TAG "rescue"

// Defined by DECLARE_BOARD(RescueBoard) in lichuang_rescue_board.cc.
void* create_board();

void rescue_main() {
    ESP_LOGI(TAG, "MetaOS factory rescue starting (board=%s)", BOARD_NAME);
    auto* board = static_cast<RescueBoard*>(create_board());
    if (!board->Initialize()) {
        ESP_LOGE(TAG, "board init failed; rescue has no display, halting");
        vTaskDelete(nullptr);
        return;
    }
    // If rescue itself was installed into an OTA slot, confirm it so the next
    // reboot does not roll it back. The factory slot is not rollback-managed.
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr && running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "rescue running from OTA slot, marked valid");
        }
    }

    rescue_ui_run(board);
    ESP_LOGI(TAG, "rescue UI running");
    // LVGL is serviced by the esp_lvgl_port task; keep the main task alive.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif  // CONFIG_METAOS_RESCUE_MODE
