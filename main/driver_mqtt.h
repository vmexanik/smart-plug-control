#pragma once

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts (or, if already running, reconfigures + restarts) the shared MQTT
 * client from settings->mqtt_uri/user/pass/client_id. A blank mqtt_uri
 * disables MQTT (driver_mqtt_set will then fail for MQTT-driven plugs). */
esp_err_t driver_mqtt_apply_settings(const system_settings_t *settings);

bool driver_mqtt_is_connected(void);

/* Publishes payload_on/payload_off to cmd_topic. */
esp_err_t driver_mqtt_set(const plug_t *plug, bool on, char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif
