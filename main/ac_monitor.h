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
    bool  in_band;          /* INSTANT verdict of the last window: vrms/freq_hz both within
                              * settings.ac_mon's [min,max] -- always false if !mains_present
                              * (no mains = 0 V = out of band). */
    bool  level;            /* The AC1 input level that plugs follow: same as in_band
                              * (no hold time -- the per-plug turn-ON delay handles settling). */
    int64_t updated_us;     /* esp_timer_get_time() at the last processed window */
} ac_mon_reading_t;

/* Fired whenever `level` (the AC1 level, see above) changes, from
 * ac_monitor_task's own context -- not an ISR, safe to call regular
 * FreeRTOS/plug-manager APIs from here, but keep it quick. Not fired on every
 * window, only on an actual transition. */
typedef void (*ac_mon_change_cb_t)(bool level);

/* Current AC1 level (mains in band). */
bool ac_monitor_get_level(void);

/* Starts continuous ADC sampling of the ZMPT101B output (see ac_monitor.c
 * for the GPIO/channel and the full measurement approach) and a background
 * task that processes it ten times a second. settings is copied, not
 * retained -- call ac_monitor_update_settings() after any settings change
 * (same pattern as gpio_inputs_update_settings()). Uses settings->ac_mon
 * (band, calibration).
 * on_change may be NULL if nothing needs to react to level transitions. */
esp_err_t ac_monitor_init(const system_settings_t *settings, ac_mon_change_cb_t on_change);

void ac_monitor_update_settings(const system_settings_t *settings);

/* Fills *out with the latest reading. Always succeeds once ac_monitor_init()
 * has been called (out->have_reading is what tells you whether it's real
 * data yet or still zeroed). */
void ac_monitor_get_reading(ac_mon_reading_t *out);

/* Voltmeter calibration. ac_monitor_calibrate() averages ~1.5 s of raw
 * windows (BLOCKS for that long) and sets the coefficient so the displayed
 * value equals vref_v (the multimeter reading); the new coefficient (volts
 * per mV RMS) is returned in *k_out and has to be persisted by the caller
 * (settings->ac_mon.cal_k). Fails with ESP_FAIL if there is no mains signal.
 * ac_monitor_reset_calibration() goes back to the Kconfig default
 * (returned in *k_out). ac_monitor_get_cal_k() is the coefficient in use. */
esp_err_t ac_monitor_calibrate(float vref_v, float *k_out);
esp_err_t ac_monitor_reset_calibration(float *k_out);
float ac_monitor_get_cal_k(void);

#ifdef __cplusplus
}
#endif
