#include <string.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "gpio_inputs.h"

static const char *TAG = "gpio_inputs";

static system_settings_t s_settings;
static SemaphoreHandle_t s_lock;
static gpio_input_change_cb_t s_cb;
static TaskHandle_t s_task_handle;

static bool s_stable_level[INPUT_COUNT_PHYSICAL];
static bool s_last_raw[INPUT_COUNT_PHYSICAL];
static uint32_t s_debounce_counter[INPUT_COUNT_PHYSICAL];
static bool s_have_sample[INPUT_COUNT_PHYSICAL];

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
    for (int i = 0; i < INPUT_COUNT_PHYSICAL; i++) {
        if (s_settings.inputs[i].enabled) {
            configure_gpio_input(s_settings.inputs[i].gpio);
        }
        s_have_sample[i] = false;
        s_debounce_counter[i] = 0;
    }
}

static void poll_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        system_settings_t snap = s_settings;
        xSemaphoreGive(s_lock);

        uint32_t threshold = snap.debounce_ms / (snap.poll_interval_ms ? snap.poll_interval_ms : 1);
        if (threshold < 1) threshold = 1;

        for (int i = 0; i < INPUT_COUNT_PHYSICAL; i++) {
            if (!snap.inputs[i].enabled || snap.inputs[i].gpio < 0) continue;

            bool raw = gpio_get_level(snap.inputs[i].gpio) != 0;

            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!s_have_sample[i]) {
                s_have_sample[i] = true;
                s_last_raw[i] = raw;
                s_stable_level[i] = raw;
                s_debounce_counter[i] = 0;
                xSemaphoreGive(s_lock);
                continue;
            }

            if (raw == s_last_raw[i]) {
                if (s_debounce_counter[i] < threshold) {
                    s_debounce_counter[i]++;
                }
            } else {
                s_last_raw[i] = raw;
                s_debounce_counter[i] = 0;
            }

            bool changed = false;
            input_id_t id = snap.inputs[i].id;
            if (s_debounce_counter[i] >= threshold && s_stable_level[i] != raw) {
                s_stable_level[i] = raw;
                changed = true;
            }
            xSemaphoreGive(s_lock);

            if (changed) {
                ESP_LOGI(TAG, "Input %s (gpio %d) -> %d", snap.inputs[i].label,
                         snap.inputs[i].gpio, raw ? 1 : 0);
                if (s_cb) {
                    s_cb(id, raw);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(snap.poll_interval_ms ? snap.poll_interval_ms : 100));
    }
}

esp_err_t gpio_inputs_init(const system_settings_t *settings, gpio_input_change_cb_t cb)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_settings = *settings;
    s_cb = cb;

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
    v = s_stable_level[i];
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
