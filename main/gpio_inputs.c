#include <string.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "gpio_inputs.h"

static const char *TAG = "gpio_inputs";

/* DC1/DC2 are hardwired inverted (active-low): a LOW pin means "input
 * normal" (level 1), a HIGH pin means out of range. Fixed in firmware, not
 * exposed in the web UI. The per-plug "invert" option is separate and is
 * applied later in plug_manager. */
#define DC_INPUTS_INVERTED 1

static system_settings_t s_settings;
static SemaphoreHandle_t s_lock;
static gpio_input_change_cb_t s_cb;
static TaskHandle_t s_task_handle;

/* Last reported level per input (1 = normal). Starts at 0, so an input that
 * is normal at boot is reported once on the first poll. */
static bool s_level[INPUT_COUNT_PHYSICAL];

static inline int idx_of(input_id_t id) { return (int)id - 1; }

static void configure_gpio_input(int gpio_num)
{
    if (gpio_num < 0) return;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) failed: %s", gpio_num, esp_err_to_name(err));
    }
}

static void apply_gpio_config_locked(void)
{
    /* Only (re)configures the pins.
     * An input that gets disabled has its level cleared by poll_task. */
    for (int i = 0; i < INPUT_COUNT_PHYSICAL; i++) {
        if (s_settings.inputs[i].enabled) {
            configure_gpio_input(s_settings.inputs[i].gpio);
        }
    }
}

static void poll_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        system_settings_t snap = s_settings;
        xSemaphoreGive(s_lock);

        const uint32_t poll_ms = snap.poll_interval_ms ? snap.poll_interval_ms : 100;

        for (int i = 0; i < INPUT_COUNT_PHYSICAL; i++) {
            if (!snap.inputs[i].enabled || snap.inputs[i].gpio < 0) {
                /* Disabled (or virtual) input: forget its level so that
                 * re-enabling it starts from "not normal" again. */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_level[i] = false;
                xSemaphoreGive(s_lock);
                continue;
            }

            bool level = gpio_get_level(snap.inputs[i].gpio) != 0;
#if DC_INPUTS_INVERTED
            level = !level;
#endif
            const input_id_t id = snap.inputs[i].id;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            const bool changed = (s_level[i] != level);
            s_level[i] = level;
            xSemaphoreGive(s_lock);

            if (changed) {
                ESP_LOGI(TAG, "Input %s (gpio %d) -> %d", snap.inputs[i].label,
                         snap.inputs[i].gpio, level ? 1 : 0);
                if (s_cb) {
                    s_cb(id, level);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}

esp_err_t gpio_inputs_init(const system_settings_t *settings, gpio_input_change_cb_t cb)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_settings = *settings;
    s_cb = cb;
    memset(s_level, 0, sizeof(s_level));

    apply_gpio_config_locked();

    BaseType_t ok = xTaskCreate(poll_task, "gpio_input_poll", 3072, NULL, 5, &s_task_handle);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t gpio_inputs_update_settings(const system_settings_t *settings)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings = *settings;
    apply_gpio_config_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool gpio_inputs_get_level(input_id_t id)
{
    int i = idx_of(id);
    if (i < 0 || i >= INPUT_COUNT_PHYSICAL) return false;
    bool v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v = s_level[i];
    xSemaphoreGive(s_lock);
    return v;
}

bool gpio_inputs_is_enabled(input_id_t id)
{
    int i = idx_of(id);
    if (i < 0 || i >= INPUT_COUNT_PHYSICAL) return false;
    bool v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v = s_settings.inputs[i].enabled;
    xSemaphoreGive(s_lock);
    return v;
}
