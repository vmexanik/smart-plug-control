#pragma once

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts the LittleFS "storage" partition at STORAGE_MOUNT_POINT. */
esp_err_t storage_init(void);

/* Fills cfg with built-in defaults (input GPIOs from Kconfig, no plugs). */
void storage_default_config(app_config_t *cfg);

/* Loads /storage/config.json into cfg. If the file is missing or corrupt,
 * cfg is filled with defaults and the defaults are written back to flash. */
esp_err_t storage_load_config(app_config_t *cfg);

/* Serializes cfg to JSON and writes it atomically (tmp file + rename). */
esp_err_t storage_save_config(const app_config_t *cfg);

/* Overwrites inputs[].id / label / gpio, plus debounce_ms / poll_interval_ms,
 * with the fixed, Kconfig-defined values (AC1/DC1/DC2, their default GPIOs,
 * and input timing). None of these are user-configurable -- only "enabled"
 * per input is. storage.c already never reads gpio/debounce/poll back from
 * JSON, but call this on any system_settings_t obtained from elsewhere
 * (e.g. a web request) as a cheap belt-and-braces guarantee it can never
 * drift from the compiled values, including from config.json written by an
 * older firmware. */
void storage_force_fixed_inputs(system_settings_t *settings);

/* Resets the persisted plug/settings config to firmware defaults (the same
 * shape storage_default_config() produces -- no plugs, Kconfig-default
 * inputs/log limits/AC-monitor band) and writes it to config.json,
 * discarding whatever was there before. Does NOT touch the NVS Wi-Fi
 * credentials or the event log -- see wifi_manager_erase_credentials() /
 * event_log_clear() for those. POST /api/system/factory_reset in
 * web_server.c calls all three together for a full device reset. */
esp_err_t storage_erase_all(void);

#ifdef __cplusplus
}
#endif
