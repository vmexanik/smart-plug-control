#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_types.h"
#include "storage.h"
#include "event_log.h"
#include "wifi_manager.h"
#include "gpio_inputs.h"
#include "plug_manager.h"
#include "web_server.h"
#include "time_sync.h"
#include "net_scan.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(storage_init());

    /* app_config_t is ~26KB (MAX_PLUGS * sizeof(plug_t)) -- far too big for
     * a stack local given the main task's stack size, so it lives in .bss. */
    static app_config_t cfg;
    ESP_ERROR_CHECK(storage_load_config(&cfg));

    ESP_ERROR_CHECK(event_log_init(&cfg.settings));
    event_log_append(LOG_SRC_BOOT, "-", "boot", true, "firmware started");

    ESP_ERROR_CHECK(wifi_manager_init());

    /* NTP server is fixed at build time (Kconfig), not web-editable. This
     * call is non-blocking: arms the SNTP client, actual sync happens once
     * STA has an IP (or never, if AP-only -- the web UI's manual time set
     * via POST /api/time covers that case). */
    time_sync_init(CONFIG_NTP_SERVER_DEFAULT);

    ESP_ERROR_CHECK(plug_manager_init(&cfg));

    ESP_ERROR_CHECK(gpio_inputs_init(&cfg.settings, plug_manager_on_input_change));

    ESP_ERROR_CHECK(net_scan_init());

    ESP_ERROR_CHECK(web_server_start());

    /* Give the input poller a couple of sample cycles to establish stable
     * levels, then push the current physical input state to bound plugs. */
    vTaskDelay(pdMS_TO_TICKS(500));
    plug_manager_sync_all_from_inputs();

    ESP_LOGI(TAG, "Smart Plug Control ready (%d plug(s) configured)", (int)cfg.plug_count);
}
