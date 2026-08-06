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

#ifdef __cplusplus
}
#endif
