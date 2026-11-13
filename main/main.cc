#include "sdkconfig.h"

#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if CONFIG_METAOS_RESCUE_MODE
#include "rescue/rescue_main.h"
#else
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "application.h"
#include "ota.h"
#include "settings.h"
#endif

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if CONFIG_METAOS_RESCUE_MODE
    // Minimal factory rescue application; independent of Application/音频/MCP/云.
    rescue_main();
#else
    // 启动失败计数（仅对"待验证的新镜像 + 异常复位"生效）：
    // - 只对 PENDING_VERIFY 镜像计数：已确认 VALID 的健康固件绝不计数，因此用户快速
    //   重启/断电不会把它误回滚（修复 BOOT-1/BOOT-2）。
    // - 只在异常复位（panic/看门狗/掉电）时累加：正常重启/切换不累加。
    // bootloader 本身已会在任意重启时回滚 PENDING_VERIFY 镜像，此计数是崩溃循环的
    // 额外兜底。救援固件不走这段逻辑，不污染计数。
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t img_state = ESP_OTA_IMG_UNDEFINED;
    const bool pending_verify =
        running != nullptr && running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY &&
        esp_ota_get_state_partition(running, &img_state) == ESP_OK &&
        img_state == ESP_OTA_IMG_PENDING_VERIFY;
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool abnormal_reset = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                                reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
                                reason == ESP_RST_BROWNOUT;
    int32_t boot_fail = 0;
    {
        Settings boot_settings("boot", true);
        if (pending_verify && abnormal_reset) {
            boot_fail = boot_settings.GetInt("boot_fail", 0) + 1;
        }
        // 达到阈值时先清零再触发回滚：Settings 析构会 nvs_commit，必须赶在回滚重启之前落盘，
        // 否则回滚后的固件下次启动会再次命中阈值，造成两个槽位之间来回回滚的乒乓循环。
        boot_settings.SetInt("boot_fail", boot_fail >= 3 ? 0 : boot_fail);
    }
    if (boot_fail >= 3) {
        ESP_LOGE(TAG, "Boot failed %ld times in a row, rolling back", static_cast<long>(boot_fail));
        Ota ota;
        // 成功则直接重启、不返回；factory 分区或无可回滚槽位时跳过，继续正常启动。
        ota.MarkCurrentVersionInvalid();
    }

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
#endif
}
