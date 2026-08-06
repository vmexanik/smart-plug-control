#pragma once

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t plug_manager_init(const app_config_t *initial_cfg);

/* Re-reads current input levels and (re)applies plug state to match them.
 * Call once shortly after boot, and any time inputs settings change. */
void plug_manager_sync_all_from_inputs(void);

/* gpio_inputs callback target. */
void plug_manager_on_input_change(input_id_t id, bool level);

/* ---- Plug CRUD (thread-safe, persists to flash on every mutation) ---- */
size_t plug_manager_get_all(plug_t *out, size_t max_out);
esp_err_t plug_manager_get(uint32_t id, plug_t *out);
esp_err_t plug_manager_add(plug_t *in_out);       /* in_out->id is assigned */
esp_err_t plug_manager_update(uint32_t id, const plug_t *in);
esp_err_t plug_manager_delete(uint32_t id);

/* Manual "test" toggle from the web UI / REST API. Blocks until the driver
 * call completes (HTTP/MQTT), so the UI gets an immediate result. */
esp_err_t plug_manager_test_action(uint32_t id, bool on, char *detail, size_t detail_size, bool *ok_out);

/* ---- System settings ---- */
esp_err_t plug_manager_get_settings(system_settings_t *out);
esp_err_t plug_manager_update_settings(const system_settings_t *in);

#ifdef __cplusplus
}
#endif
