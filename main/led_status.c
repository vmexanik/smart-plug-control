#include <stdbool.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_status.h"

static const char *TAG = "led_status";

/* Free, general-purpose GPIO on the ESP32-S3-WROOM-1-N16R8 (not shared with
 * flash/PSRAM, USB-JTAG, console UART or any strapping pin -- see
 * Kconfig.projbuild's pin notes) and not used anywhere else in this
 * firmware (AC1/DC1/DC2 default to GPIO4/5/6). */
#define LED_STATUS_GPIO       2
#define LED_HALF_PERIOD_MS    500   /* 500 on + 500 off = one blink per second */

/* Tiny, self-contained loop -- no network/lock calls, so a 1536-byte stack
 * is already generous. Deliberately NOT subscribed to the task watchdog:
 * the LED itself freezing (stuck on or off, no longer blinking) is already
 * a visible-to-a-human "something's wrong" signal, which is this task's
 * entire purpose -- there is no other subsystem depending on it staying
 * alive, so there's nothing extra a watchdog reset would protect here. */
static void led_status_task(void *arg)
{
    (void)arg;
    bool on = false;
    for (;;) {
        on = !on;
        gpio_set_level(LED_STATUS_GPIO, on ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(LED_HALF_PERIOD_MS));
    }
}

esp_err_t led_status_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_STATUS_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) failed: %s", LED_STATUS_GPIO, esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(led_status_task, "led_status", 1536, NULL, 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start led_status_task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
