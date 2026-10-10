#pragma once

#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called (from the polling task) whenever the level of an enabled input
 * changes. level is the (non-inverted) level: 1 = input normal. */
typedef void (*gpio_input_change_cb_t)(input_id_t id, bool level);

esp_err_t gpio_inputs_init(const system_settings_t *settings, gpio_input_change_cb_t cb);

/* Re-applies GPIO config / enabled set / poll timing after the
 * system settings were changed via the web UI. */
esp_err_t gpio_inputs_update_settings(const system_settings_t *settings);

/* Last known stable raw level of an input (false if never sampled / disabled). */
bool gpio_inputs_get_level(input_id_t id);

/* Is this input currently enabled (per system settings)? */
bool gpio_inputs_is_enabled(input_id_t id);

#ifdef __cplusplus
}
#endif
