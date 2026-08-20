#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Latest AC mains reading -- runtime only, never persisted (compare
 * plug_metrics_t in app_types.h, same idea). */
typedef struct {
    bool  have_reading;     /* false until the first full sample window has been processed */
    bool  mains_present;    /* raw signal amplitude above the noise floor -- see ac_monitor.c */
    float vrms;             /* volts, meaningless (0) if !mains_present */
    float freq_hz;          /* Hz, meaningless (0) if !mains_present */
    bool  in_band;          /* vrms/freq_hz both within settings.ac_mon's [min,max] --
                              * always true if settings.ac_mon.enabled is false or
                              * !mains_present (nothing to protect against) */
    int64_t updated_us;     /* esp_timer_get_time() at the last processed window */
} ac_mon_reading_t;

/* Fired whenever the in-band status changes (in-band -> out-of-band or back),
 * from ac_monitor_task's own context -- not an ISR, safe to call regular
 * FreeRTOS/plug-manager APIs from here, but keep it quick (it runs inline in
 * the sampling loop, same as gpio_inputs.c's input-change callback). Not
 * fired on every window, only on an actual transition. */
typedef void (*ac_mon_change_cb_t)(bool in_band);

/* Starts continuous ADC sampling of the ZMPT101B output (see ac_monitor.c
 * for the GPIO/channel and the full measurement approach) and a background
 * task that processes it a couple times a second. settings is copied, not
 * retained -- call ac_monitor_update_settings() after any settings change
 * (same pattern as event_log_update_limits()/gpio_inputs_update_settings()).
 * on_change may be NULL if nothing needs to react to in-band transitions
 * (e.g. status-only builds); ac_monitor.c deliberately does not #include
 * plug_manager.h itself, same decoupling pattern as gpio_inputs.c. */
esp_err_t ac_monitor_init(const ac_mon_settings_t *settings, ac_mon_change_cb_t on_change);

void ac_monitor_update_settings(const ac_mon_settings_t *settings);

/* Fills *out with the latest reading. Always succeeds once ac_monitor_init()
 * has been called (out->have_reading is what tells you whether it's real
 * data yet or still zeroed). */
void ac_monitor_get_reading(ac_mon_reading_t *out);

#ifdef __cplusplus
}
#endif
