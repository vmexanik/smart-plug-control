#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ac_monitor.h"
#include "zmpt101b.h"

static const char *TAG = "ac_monitor";

/* ------------------------------------------------------------------ */
/* Wiring / hardware                                                    */
/* ------------------------------------------------------------------ */

/* The measurement itself (adc_continuous DMA on ADC1, 20 kHz oversampled
 * down to 5 kHz, eFuse curve-fitting calibration, RMS and frequency over
 * whole mains periods, noise subtraction, EMA) lives in zmpt101b.c/.h --
 * this file is only the glue between that driver and the rest of the
 * firmware: it owns the AC1 pin choice, turns zmpt101b_data_t into
 * ac_mon_reading_t, applies the v_min/v_max/f_min/f_max band from the
 * settings and fires the in-band change callback.
 *
 * adc_continuous on ESP32-S3 only works on ADC1, so the ZMPT101B output
 * must be on GPIO1-10 (ADC1 channel = GPIO - 1). The default is GPIO10
 * (ADC1_CH9) -- on boards routed to GPIO11 (ADC2) bridge GPIO11 to GPIO10.
 * CONFIG_AC_MON_ADC_GPIO_DEFAULT is resolved at compile time. */
#define AC_MON_ADC_GPIO CONFIG_AC_MON_ADC_GPIO_DEFAULT

#if AC_MON_ADC_GPIO >= 1 && AC_MON_ADC_GPIO <= 10
#define AC_MON_ADC_CHANNEL  (AC_MON_ADC_GPIO - 1)
#else
#error "CONFIG_AC_MON_ADC_GPIO_DEFAULT must be an ADC1 GPIO (1-10): ADC continuous mode does not work on ADC2 on ESP32-S3"
#endif

/* Calibration: volts of mains per millivolt RMS at the ADC pin (after
 * adc_cali). Kconfig has no float type, so the option is coefficient x10000.
 * Recalibrate on hardware with zmpt101b_auto_calibrate(handle, <multimeter V>, 2000, &k)
 * and put k*10000 into CONFIG_AC_MON_VOLTS_PER_RMS_MV_X10000. */
#define AC_MON_CAL_FACTOR (CONFIG_AC_MON_VOLTS_PER_RMS_MV_X10000 / 10000.0f)

/* Debug aid: 1 = periodically log the driver's full diagnostic record. */
#define AC_MON_DEBUG_LOG 1
#define AC_MON_DEBUG_LOG_EVERY_N 20 /* ~2s at the 100ms poll below */

/* ------------------------------------------------------------------ */
/* Processing                                                           */
/* ------------------------------------------------------------------ */

/* The driver publishes a fresh result every window_ms (100 ms = 5 mains
 * periods at 50 Hz); polling at the same rate keeps outage/fault detection
 * latency at ~0.1-0.2 s. */
#define POLL_MS     100
#define WINDOW_MS   100

/* Peak-to-peak below this (mV at the ADC pin, raw, un-smoothed) = "just
 * sensor bias/noise, no real mains" -- a ZMPT101B with no mains still
 * outputs a flat level around its bias point, a real signal swings
 * ~1000-1400 mV p-p. Uses vpp_mv rather than voltage_rms on purpose: it is
 * per-window and instant, so a mains outage is seen on the very next window. */
#define NO_MAINS_AMPLITUDE_MV 40.0f

/* Raw code limits for a near-clip warning (12-bit, ADC_ATTEN_DB_12). */
#define CLIP_RAW_HI 4050
#define CLIP_RAW_LO 45

static SemaphoreHandle_t s_lock;
static ac_mon_settings_t s_settings;
static ac_mon_reading_t s_reading;
static ac_mon_change_cb_t s_on_change;
static zmpt101b_handle_t s_dev;
/* Tracks the in_band value last reported to s_on_change, so the callback
 * only fires on an actual transition. Starts true (matches the "nothing to
 * protect against yet" default) so plugs aren't gated off before the first
 * window completes. */
static bool s_last_in_band = true;

static void build_reading(const zmpt101b_data_t *d, const ac_mon_settings_t *st, ac_mon_reading_t *out)
{
    memset(out, 0, sizeof(*out));
    out->updated_us = esp_timer_get_time();
    out->have_reading = true;

    if (d->vpp_mv < NO_MAINS_AMPLITUDE_MV) {
        out->mains_present = false;
        out->in_band = true; /* nothing to protect against if there's no signal at all */
        return;
    }

    out->mains_present = true;
    out->vrms = d->voltage_rms;
    out->freq_hz = d->frequency_hz;

    /* AC1 monitoring has no enable flag -- the band check is always active
     * whenever there is a real reading. freq_hz == 0 means the driver has
     * not got a reliable frequency yet; don't flag that as out-of-band. */
    if (out->freq_hz <= 0.0f) {
        out->in_band = true;
    } else {
        const bool v_ok = (out->vrms >= (float)st->v_min) && (out->vrms <= (float)st->v_max);
        const bool f_ok = (out->freq_hz >= st->f_min) && (out->freq_hz <= st->f_max);
        out->in_band = v_ok && f_ok;
    }
}

static void ac_monitor_task(void *arg)
{
    (void)arg;
#if AC_MON_DEBUG_LOG
    uint32_t dbg_n = 0;
#endif
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        zmpt101b_data_t d;
        if (zmpt101b_get_data(s_dev, &d) != ESP_OK || !d.is_valid) continue;

        ac_mon_settings_t settings_snapshot;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        settings_snapshot = s_settings;
        xSemaphoreGive(s_lock);

        ac_mon_reading_t reading;
        build_reading(&d, &settings_snapshot, &reading);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_reading = reading;
        xSemaphoreGive(s_lock);

#if AC_MON_DEBUG_LOG
        if (++dbg_n % AC_MON_DEBUG_LOG_EVERY_N == 0) {
            const bool clip = d.adc_raw_max >= CLIP_RAW_HI || d.adc_raw_min <= CLIP_RAW_LO;
            ESP_LOGI(TAG, "V=%.2f F=%.2f (raw %.2f, zc=%lu) DC=%.1fmV Vpp=%.1fmV noise=%.1fmV "
                          "fs_adc=%.0fHz ovf=%lu mains=%d in_band=%d%s",
                     (double)d.voltage_rms, (double)d.frequency_hz, (double)d.freq_raw_hz,
                     (unsigned long)d.zc_count, (double)d.dc_offset_mv, (double)d.vpp_mv,
                     (double)d.noise_mv, (double)d.adc_rate_hz, (unsigned long)d.pool_overflows,
                     (int)reading.mains_present, (int)reading.in_band, clip ? "  !CLIP" : "");
        }
#endif

        /* Callback outside s_lock (plug_manager takes its own lock; never
         * nest locks across modules). Only on an actual transition. */
        if (reading.in_band != s_last_in_band) {
            s_last_in_band = reading.in_band;
            if (s_on_change) s_on_change(reading.in_band);
        }
    }
}

esp_err_t ac_monitor_init(const ac_mon_settings_t *settings, ac_mon_change_cb_t on_change)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    s_settings = *settings;
    memset(&s_reading, 0, sizeof(s_reading));
    s_on_change = on_change;
    s_last_in_band = true;

    const zmpt101b_config_t cfg = {
        .adc_unit = ADC_UNIT_1,
        .adc_channel = (adc_channel_t)AC_MON_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .calibration_factor = AC_MON_CAL_FACTOR,
        .sample_rate_hz = 5000,
        .window_ms = WINDOW_MS,       /* 5 periods of 50 Hz */
        .filter_alpha_v = 0.15f,
        .filter_alpha_f = 0.08f,
        .min_voltage_threshold = 15.0f,
    };

    esp_err_t err = zmpt101b_init(&cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zmpt101b_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = zmpt101b_start(s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zmpt101b_start failed: %s", esp_err_to_name(err));
        zmpt101b_deinit(s_dev);
        s_dev = NULL;
        return err;
    }

    if (xTaskCreate(ac_monitor_task, "ac_monitor", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start ac_monitor_task");
        zmpt101b_deinit(s_dev);
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AC mains monitor started (GPIO%d, ADC1_CH%d, adc_continuous 5 kHz x4, %d ms window, K=%.4f V/mV)",
             AC_MON_ADC_GPIO, (int)AC_MON_ADC_CHANNEL, WINDOW_MS, (double)AC_MON_CAL_FACTOR);
    return ESP_OK;
}

void ac_monitor_update_settings(const ac_mon_settings_t *settings)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings = *settings;
    xSemaphoreGive(s_lock);
}

void ac_monitor_get_reading(ac_mon_reading_t *out)
{
    if (!out) return;
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_reading;
    xSemaphoreGive(s_lock);
}
