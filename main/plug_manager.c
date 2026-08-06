#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "plug_manager.h"
#include "storage.h"
#include "event_log.h"
#include "gpio_inputs.h"
#include "driver_http.h"
#include "driver_mqtt.h"
#include "driver_zigbee.h"

static const char *TAG = "plug_manager";

static app_config_t s_cfg;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_action_queue;
static uint32_t s_next_id = 1;

typedef struct {
    uint32_t plug_id;
    bool desired_on;
    char source[16];
} plug_action_t;

/* Caller must hold s_lock. */
static int find_index_locked(uint32_t id)
{
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        if (s_cfg.plugs[i].id == id) return (int)i;
    }
    return -1;
}

static esp_err_t save_locked(void)
{
    return storage_save_config(&s_cfg);
}

/* Runs the actual driver call for a plug snapshot (no locks held), then
 * updates runtime state under lock and writes an event log entry. */
static esp_err_t do_dispatch(plug_t snapshot, bool on, const char *source,
                              char *detail_out, size_t detail_out_size, bool *ok_out)
{
    char detail[96] = {0};
    esp_err_t err;

    if (!snapshot.enabled) {
        snprintf(detail, sizeof(detail), "plug disabled");
        err = ESP_ERR_INVALID_STATE;
    } else {
        switch (snapshot.driver) {
            case DRIVER_HTTP:
                err = driver_http_set(&snapshot, on, detail, sizeof(detail));
                break;
            case DRIVER_MQTT:
                err = driver_mqtt_set(&snapshot, on, detail, sizeof(detail));
                break;
            case DRIVER_ZIGBEE:
                err = driver_zigbee_set(&snapshot, on, detail, sizeof(detail));
                break;
            default:
                snprintf(detail, sizeof(detail), "unknown driver type");
                err = ESP_ERR_INVALID_ARG;
                break;
        }
    }

    bool ok = (err == ESP_OK);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(snapshot.id);
    if (idx >= 0) {
        s_cfg.plugs[idx].last_state = on;
        s_cfg.plugs[idx].last_cmd_ok = ok;
        s_cfg.plugs[idx].last_change_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);

    event_log_append(source, snapshot.name, on ? "on" : "off", ok, detail);

    if (detail_out) snprintf(detail_out, detail_out_size, "%s", detail);
    if (ok_out) *ok_out = ok;
    return err;
}

static void worker_task(void *arg)
{
    plug_action_t act;
    for (;;) {
        if (xQueueReceive(s_action_queue, &act, portMAX_DELAY) != pdTRUE) continue;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        int idx = find_index_locked(act.plug_id);
        plug_t snapshot;
        bool have = false;
        if (idx >= 0) {
            snapshot = s_cfg.plugs[idx];
            have = true;
        }
        xSemaphoreGive(s_lock);

        if (have) {
            do_dispatch(snapshot, act.desired_on, act.source, NULL, 0, NULL);
        }
    }
}

static void enqueue_action(uint32_t plug_id, bool on, const char *source)
{
    plug_action_t act = { .plug_id = plug_id, .desired_on = on };
    strncpy(act.source, source, sizeof(act.source) - 1);
    if (xQueueSend(s_action_queue, &act, 0) != pdTRUE) {
        ESP_LOGW(TAG, "action queue full, dropping command for plug %u", (unsigned)plug_id);
    }
}

esp_err_t plug_manager_init(const app_config_t *initial_cfg)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_cfg = *initial_cfg;

    uint32_t max_id = 0;
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        if (s_cfg.plugs[i].id > max_id) max_id = s_cfg.plugs[i].id;
        s_cfg.plugs[i].last_state = false;
        s_cfg.plugs[i].last_cmd_ok = true;
        s_cfg.plugs[i].last_change_us = 0;
    }
    s_next_id = max_id + 1;

    s_action_queue = xQueueCreate(32, sizeof(plug_action_t));
    if (!s_action_queue) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(worker_task, "plug_worker", 4096, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    driver_mqtt_apply_settings(&s_cfg.settings);

    return ESP_OK;
}

void plug_manager_on_input_change(input_id_t id, bool level)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t targets[MAX_PLUGS];
    bool desired[MAX_PLUGS];
    size_t n = 0;
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        plug_t *p = &s_cfg.plugs[i];
        if (!p->enabled || p->input != id) continue;
        targets[n] = p->id;
        desired[n] = level ^ p->invert;
        n++;
    }
    xSemaphoreGive(s_lock);

    for (size_t i = 0; i < n; i++) {
        enqueue_action(targets[i], desired[i], LOG_SRC_INPUT);
    }
}

void plug_manager_sync_all_from_inputs(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t targets[MAX_PLUGS];
    bool desired[MAX_PLUGS];
    size_t n = 0;
    for (size_t i = 0; i < s_cfg.plug_count; i++) {
        plug_t *p = &s_cfg.plugs[i];
        if (!p->enabled || p->input == INPUT_NONE) continue;
        if (!gpio_inputs_is_enabled(p->input)) continue;
        bool level = gpio_inputs_get_level(p->input);
        targets[n] = p->id;
        desired[n] = level ^ p->invert;
        n++;
    }
    xSemaphoreGive(s_lock);

    for (size_t i = 0; i < n; i++) {
        enqueue_action(targets[i], desired[i], LOG_SRC_BOOT);
    }
}

size_t plug_manager_get_all(plug_t *out, size_t max_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_cfg.plug_count < max_out ? s_cfg.plug_count : max_out;
    memcpy(out, s_cfg.plugs, n * sizeof(plug_t));
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t plug_manager_get(uint32_t id, plug_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(id);
    if (idx >= 0) *out = s_cfg.plugs[idx];
    xSemaphoreGive(s_lock);
    return idx >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t plug_manager_add(plug_t *in_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cfg.plug_count >= MAX_PLUGS) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    in_out->id = s_next_id++;
    in_out->last_state = false;
    in_out->last_cmd_ok = true;
    in_out->last_change_us = 0;
    s_cfg.plugs[s_cfg.plug_count] = *in_out;
    s_cfg.plug_count++;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);

    event_log_append(LOG_SRC_API, in_out->name, "created", true, NULL);
    return err;
}

esp_err_t plug_manager_update(uint32_t id, const plug_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(id);
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    plug_t updated = *in;
    updated.id = id;
    updated.last_state = s_cfg.plugs[idx].last_state;
    updated.last_cmd_ok = s_cfg.plugs[idx].last_cmd_ok;
    updated.last_change_us = s_cfg.plugs[idx].last_change_us;
    s_cfg.plugs[idx] = updated;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);

    event_log_append(LOG_SRC_API, updated.name, "updated", true, NULL);
    return err;
}

esp_err_t plug_manager_delete(uint32_t id)
{
    char name[PLUG_NAME_LEN] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(id);
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    strncpy(name, s_cfg.plugs[idx].name, sizeof(name) - 1);
    for (size_t i = idx; i + 1 < s_cfg.plug_count; i++) {
        s_cfg.plugs[i] = s_cfg.plugs[i + 1];
    }
    s_cfg.plug_count--;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);

    event_log_append(LOG_SRC_API, name, "deleted", true, NULL);
    return err;
}

esp_err_t plug_manager_test_action(uint32_t id, bool on, char *detail, size_t detail_size, bool *ok_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_index_locked(id);
    plug_t snapshot;
    if (idx >= 0) snapshot = s_cfg.plugs[idx];
    xSemaphoreGive(s_lock);

    if (idx < 0) {
        if (detail) snprintf(detail, detail_size, "plug not found");
        if (ok_out) *ok_out = false;
        return ESP_ERR_NOT_FOUND;
    }

    return do_dispatch(snapshot, on, LOG_SRC_MANUAL, detail, detail_size, ok_out);
}

esp_err_t plug_manager_get_settings(system_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg.settings;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t plug_manager_update_settings(const system_settings_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg.settings = *in;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_lock);

    gpio_inputs_update_settings(in);
    event_log_update_limits(in->log_max_file_size, in->log_max_files);
    driver_mqtt_apply_settings(in);

    event_log_append(LOG_SRC_SYSTEM, "-", "settings_updated", true, NULL);

    plug_manager_sync_all_from_inputs();
    return err;
}
