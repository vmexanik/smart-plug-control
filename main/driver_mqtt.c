#include <string.h>
#include <stdio.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver_mqtt.h"

static const char *TAG = "driver_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static SemaphoreHandle_t s_lock = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base; (void)event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            s_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            s_connected = false;
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT error event");
            break;
        default:
            break;
    }
}

esp_err_t driver_mqtt_apply_settings(const system_settings_t *settings)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }

    if (settings->mqtt_uri[0] == '\0') {
        ESP_LOGI(TAG, "MQTT disabled (no broker URI configured)");
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = settings->mqtt_uri,
        .credentials.client_id = settings->mqtt_client_id[0] ? settings->mqtt_client_id : NULL,
        .credentials.username = settings->mqtt_user[0] ? settings->mqtt_user : NULL,
        .credentials.authentication.password = settings->mqtt_pass[0] ? settings->mqtt_pass : NULL,
        .network.reconnect_timeout_ms = 5000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_lock);
    return err;
}

bool driver_mqtt_is_connected(void)
{
    return s_connected;
}

esp_err_t driver_mqtt_set(const plug_t *plug, bool on, char *detail, size_t detail_size)
{
    if (!s_client || !s_connected) {
        snprintf(detail, detail_size, "mqtt broker not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (plug->mqtt.cmd_topic[0] == '\0') {
        snprintf(detail, detail_size, "no cmd_topic configured");
        return ESP_ERR_INVALID_ARG;
    }

    const char *payload = on ? plug->mqtt.payload_on : plug->mqtt.payload_off;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int msg_id = esp_mqtt_client_publish(s_client, plug->mqtt.cmd_topic, payload, 0,
                                          plug->mqtt.qos, plug->mqtt.retain ? 1 : 0);
    xSemaphoreGive(s_lock);

    if (msg_id < 0) {
        snprintf(detail, detail_size, "publish failed");
        return ESP_FAIL;
    }
    snprintf(detail, detail_size, "published msg_id=%d", msg_id);
    return ESP_OK;
}
