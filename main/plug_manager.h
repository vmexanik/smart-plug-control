#pragma once

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t plug_manager_init(const app_config_t *initial_cfg);

/* Starts the metrics-polling and initial-status-check background tasks --
 * split out from plug_manager_init() deliberately so app_main() can call
 * this only after wifi_manager_start() has already claimed its internal-DRAM
 * buffers (see that function's doc comment). Both tasks wait for Wi-Fi
 * before doing anything useful anyway, so nothing is lost by starting them
 * a little later; not calling this at all just means neither ever runs. */
esp_err_t plug_manager_start_background_tasks(void);

/* Re-reads current input levels and (re)applies plug state to match them.
 * Call once shortly after boot, and any time inputs settings change. */
void plug_manager_sync_all_from_inputs(void);

/* gpio_inputs callback target. */
void plug_manager_on_input_change(input_id_t id, bool level);

/* ac_monitor callback target -- see ac_mon_change_cb_t in ac_monitor.h.
 * AC1 has no physical GPIO on this board revision: this IS the AC1 input's
 * level (see input_id_t's comment in app_types.h) -- in_band drives every
 * AC1-bound plug directly (^ its own invert flag), on every actual
 * in-band/out-of-band transition. */
void plug_manager_on_ac_mon_change(bool in_band);

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

/* Bounded-wait probe of the internal lock -- for system_health.c's deadlock
 * detection ONLY, not a general-purpose accessor. Every other function
 * above intentionally blocks forever (portMAX_DELAY) because a plug
 * command must never be silently dropped; this one instead returns false
 * if the lock isn't free within timeout_ms, which is itself the useful
 * signal (something else is stuck holding it). On success the lock is
 * acquired and immediately released -- nothing is read or changed. */
bool plug_manager_is_responsive(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
