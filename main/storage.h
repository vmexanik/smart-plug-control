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

/* Reads config.json's raw bytes into out (NUL-terminated), unchanged --
 * deliberately not a re-serialize-from-struct round trip, so an exported
 * file is byte-identical to what's actually on flash. ESP_ERR_NOT_FOUND if
 * nothing has ever been saved yet, ESP_ERR_NO_MEM if it doesn't fit in
 * out_size. Used by GET /api/config/export. */
esp_err_t storage_read_raw_config(char *out, size_t out_size, size_t *out_len);

/* Validates json (must parse and contain top-level "plugs" and "settings"
 * keys -- the shape storage_load_config()/storage_save_config() both use)
 * and, if valid, writes it byte-for-byte to config.json (same atomic
 * tmp-file-then-rename as storage_save_config()). Returns ESP_ERR_INVALID_ARG
 * if json doesn't look like a config file. Does NOT touch any already-
 * running in-memory state -- plug_manager.c loads its config once at boot,
 * so an import only takes effect after a reboot. Used by POST
 * /api/config/import. */
esp_err_t storage_write_raw_config(const char *json, size_t len);

#ifdef __cplusplus
}
#endif
