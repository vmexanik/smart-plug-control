#pragma once

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Zigbee driver placeholder.
 *
 * A standard ESP32 WROOM has no 802.15.4 radio, so Zigbee control cannot run
 * on-chip; it needs either a companion radio (ESP32-H2/C6 acting as a
 * Zigbee coordinator, connected over UART/SPI) or an external Zigbee
 * gateway that this firmware talks to (e.g. via its HTTP/MQTT API, in which
 * case DRIVER_HTTP/DRIVER_MQTT already cover it today).
 *
 * This stub exists so the rest of the system (config schema, storage,
 * web UI, plug_manager dispatch table) already has a third driver slot.
 * When real Zigbee support is added, keep this signature and swap the
 * body for calls into esp-zigbee-sdk / a UART bridge protocol -- no other
 * module needs to change.
 */
esp_err_t driver_zigbee_set(const plug_t *plug, bool on, char *detail, size_t detail_size);

#ifdef __cplusplus
}
#endif
