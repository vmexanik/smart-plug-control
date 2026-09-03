#include <string.h>
#include <math.h>
#include "sdkconfig.h"
#include "esp_adc/adc_continuous.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ac_monitor.h"

static const char *TAG = "ac_monitor";

/* ------------------------------------------------------------------ */
/* Wiring / hardware                                                    */
/* ------------------------------------------------------------------ */

/* Which physical GPIO the ZMPT101B output is wired to -- this IS the "AC1"
 * pin (see input_id_t's comment in app_types.h: AC1 has no separate digital
 * line, only this analog one). Configurable via Kconfig (menuconfig ->
 * Smart Plug Control -> AC mains monitor (ZMPT101B)), same pattern as
 * DC1/DC2's GPIOs in Kconfig.projbuild.
 *
 * ESP32-S3 ADC-capable pins split into two independent units: GPIO1-10 =
 * ADC1 (channel = GPIO - 1), GPIO11-20 = ADC2 (channel = GPIO - 11). ADC1
 * is strongly preferred when there's a choice -- ADC2 shares hardware with
 * the Wi-Fi radio and can give delayed/noisier readings while Wi-Fi is
 * active, which on this device is always. CONFIG_AC_MON_ADC_GPIO_DEFAULT
 * resolves to whichever unit/channel/conv_mode its value falls into, at
 * compile time; a build-time #error catches anything outside GPIO1-20. */
#define AC_MON_ADC_GPIO CONFIG_AC_MON_ADC_GPIO_DEFAULT

#if AC_MON_ADC_GPIO >= 1 && AC_MON_ADC_GPIO <= 10
#define AC_MON_ADC_UNIT       ADC_UNIT_1
#define AC_MON_ADC_CHANNEL    (AC_MON_ADC_GPIO - 1)
#define AC_MON_ADC_CONV_MODE  ADC_CONV_SINGLE_UNIT_1
#elif AC_MON_ADC_GPIO >= 11 && AC_MON_ADC_GPIO <= 20
#define AC_MON_ADC_UNIT       ADC_UNIT_2
#define AC_MON_ADC_CHANNEL    (AC_MON_ADC_GPIO - 11)
#define AC_MON_ADC_CONV_MODE  ADC_CONV_SINGLE_UNIT_2
#else
#error "CONFIG_AC_MON_ADC_GPIO_DEFAULT must be an ADC-capable ESP32-S3 GPIO (1-20)"
#endif

#define AC_MON_ADC_ATTEN    ADC_ATTEN_DB_12 /* full ~0-3.3V input range */
#define AC_MON_ADC_BITWIDTH SOC_ADC_DIGI_MAX_BITWIDTH /* 12-bit on S3: 0-4095 counts */

/* ZMPT101B's own onboard op-amp biases and scales the mains waveform onto
 * this range -- exact gain depends on the module's trim pot, which is why
 * this needs a real calibration pass (see AC_CAL_ENABLE below) rather than
 * a fixed textbook constant.
 *
 * This is board-specific (every physical ZMPT101B module needs its own
 * value), so unlike most other tunables in this project it lives in
 * Kconfig (`menuconfig -> Smart Plug Control -> AC mains monitor
 * (ZMPT101B)`), not the web UI or a plain #define -- same reasoning as the
 * input GPIOs: something you set once per physical build, reflash, and
 * never touch from a phone browser. Kconfig has no float type, so the
 * option is the real coefficient x10000 (e.g. 5500 = 0.5500); divided back
 * down here at compile time. */
#define AC_MON_DEFAULT_VOLTS_PER_ADC_RMS_COUNT (CONFIG_AC_MON_VOLTS_PER_ADC_RMS_COUNT_X10000 / 10000.0f)

/* Calibration aid: set to 1, power the board from a known-good AC_CAL_VRMS
 * (read off a multimeter on the real mains line), reflash, watch the log
 * for "CAL: volts_per_adc_rms_count = ...", multiply that number by 10000
 * and put it in CONFIG_AC_MON_VOLTS_PER_ADC_RMS_COUNT_X10000 (menuconfig),
 * set this back to 0. */
#define AC_CAL_ENABLE 0
#define AC_CAL_VRMS    230.0f

/* ------------------------------------------------------------------ */
/* Sampling / processing                                                */
/* ------------------------------------------------------------------ */

#define SAMPLE_RATE_HZ       10000
/* ~410ms of mains at 10kHz -- ~20 cycles at 50Hz, ~24 at 60Hz. Long enough
 * for the whole-window zero-crossing average (see process_window()) to
 * average out per-crossing jitter, short enough that a real fault (mains
 * gone, way out of band) shows up within half a second. */
#define WINDOW_SAMPLES       4096
/* adc_continuous driver frame/read chunk -- kept modest and read
 * repeatedly to fill WINDOW_SAMPLES, rather than one giant DMA buffer (see
 * the ESP-IDF continuous_read example this is modeled on). */
#define READ_CHUNK_BYTES     256

/* Below this many raw ADC counts of amplitude (peak deviation from the
 * window's own mean), treat the line as "just sensor noise/bias, no real
 * mains signal" -- a ZMPT101B with nothing connected still outputs a flat
 * near-constant voltage around its bias point, not a full-scale swing. */
#define NO_MAINS_AMPLITUDE_COUNTS 8

#define MAX_CROSSINGS  64 /* generous headroom over ~20-24 expected per window */

static SemaphoreHandle_t s_lock;
static ac_mon_settings_t s_settings;
static ac_mon_reading_t s_reading;
static float s_volts_per_adc_rms_count = AC_MON_DEFAULT_VOLTS_PER_ADC_RMS_COUNT;
static ac_mon_change_cb_t s_on_change;
/* Tracks the in_band value last reported to s_on_change, so the callback
 * only fires on an actual transition, not once per ~0.4s window. Starts
 * true (matches process_window()'s own "nothing to protect against yet"
 * default before the first real reading), so plugs aren't gated off during
 * the brief window before the first sample completes. */
static bool s_last_in_band = true;

static adc_continuous_handle_t s_adc_handle;
static uint16_t s_window[WINDOW_SAMPLES];

/* Pulls samples from the continuous-ADC driver until either s_window is
 * full or we give up waiting (mains sampling should never actually be this
 * slow -- the timeout just keeps this task from blocking forever if the
 * ADC driver itself is somehow stuck). Returns the number of samples
 * actually collected. */
static size_t fill_window(void)
{
    uint8_t raw[READ_CHUNK_BYTES];
    size_t collected = 0;
    int consecutive_timeouts = 0;

    while (collected < WINDOW_SAMPLES && consecutive_timeouts < 20) {
        uint32_t ret_num = 0;
        esp_err_t err = adc_continuous_read(s_adc_handle, raw, sizeof(raw), &ret_num, 50 /* ms */);
        if (err == ESP_ERR_TIMEOUT) {
            consecutive_timeouts++;
            continue;
        }
        if (err != ESP_OK || ret_num == 0) {
            consecutive_timeouts++;
            continue;
        }
        consecutive_timeouts = 0;

        if (ret_num / SOC_ADC_DIGI_RESULT_BYTES == 0) continue;

        /* static, not a stack VLA: worst case ret_num == sizeof(raw), and a
         * variable-length array sized off driver output at runtime is
         * exactly the kind of thing that bit this project before (see
         * README/task history on the S3 stack-overflow scheduler panic) --
         * a fixed, generously-sized static buffer sidesteps that risk
         * entirely instead of trying to get a task's stack size to cover
         * an unpredictable worst case. */
        static adc_continuous_data_t parsed[READ_CHUNK_BYTES];
        uint32_t n_parsed = 0;
        if (adc_continuous_parse_data(s_adc_handle, raw, ret_num, parsed, &n_parsed) != ESP_OK) {
            continue;
        }

        for (uint32_t i = 0; i < n_parsed && collected < WINDOW_SAMPLES; i++) {
            if (!parsed[i].valid) continue;
            if (parsed[i].unit != AC_MON_ADC_UNIT || parsed[i].channel != AC_MON_ADC_CHANNEL) continue;
            s_window[collected++] = (uint16_t)parsed[i].raw_data;
        }
    }
    return collected;
}

/* Runs the RMS + zero-crossing math over s_window[0..n), writes the result
 * into *out. See ac_monitor.h for field meanings. */
static void process_window(size_t n, const ac_mon_settings_t *settings, ac_mon_reading_t *out)
{
    memset(out, 0, sizeof(*out));
    out->updated_us = esp_timer_get_time();

    if (n < WINDOW_SAMPLES / 2) {
        /* Badly short read -- treat like "no reading yet" rather than
         * computing garbage statistics over a half-empty window. */
        out->have_reading = false;
        return;
    }
    out->have_reading = true;

    /* --- mean (DC bias) and RMS of the AC component --- */
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += s_window[i];
    double mean = sum / (double)n;

    double sum_sq = 0;
    float min_v = s_window[0], max_v = s_window[0];
    for (size_t i = 0; i < n; i++) {
        double d = (double)s_window[i] - mean;
        sum_sq += d * d;
        if (s_window[i] < min_v) min_v = s_window[i];
        if (s_window[i] > max_v) max_v = s_window[i];
    }
    double rms_counts = sqrt(sum_sq / (double)n);
    float amplitude = max_v - min_v;

    if (amplitude < NO_MAINS_AMPLITUDE_COUNTS) {
        out->mains_present = false;
        out->in_band = true; /* nothing to protect against if there's no signal at all */
        return;
    }
    out->mains_present = true;
    out->vrms = (float)(rms_counts * s_volts_per_adc_rms_count);

    /* --- zero crossings (rising, AC component only) --- */
    static float crossings[MAX_CROSSINGS];
    int n_cross = 0;
    for (size_t i = 0; i + 1 < n && n_cross < MAX_CROSSINGS; i++) {
        double a = (double)s_window[i] - mean;
        double b = (double)s_window[i + 1] - mean;
        if (a < 0 && b >= 0) {
            /* linear-interpolate the exact (fractional-sample) crossing point */
            double frac = (b > a) ? (-a / (b - a)) : 0.0;
            crossings[n_cross++] = (float)i + (float)frac;
        }
    }

    if (n_cross >= 2) {
        float span_samples = crossings[n_cross - 1] - crossings[0];
        float span_s = span_samples / (float)SAMPLE_RATE_HZ;
        if (span_s > 0.0f) {
            out->freq_hz = (float)(n_cross - 1) / span_s;
        }
    }

    /* --- in-band check --- */
    if (!settings->enabled || out->freq_hz <= 0.0f) {
        out->in_band = true;
    } else {
        bool v_ok = (out->vrms >= (float)settings->v_min) && (out->vrms <= (float)settings->v_max);
        bool f_ok = (out->freq_hz >= settings->f_min) && (out->freq_hz <= settings->f_max);
        out->in_band = v_ok && f_ok;
    }

#if AC_CAL_ENABLE
    ESP_LOGW(TAG, "CAL: volts_per_adc_rms_count = %.4f (at %.1fV RMS, raw rms_counts=%.1f)",
             (double)(AC_CAL_VRMS / (float)rms_counts), (double)AC_CAL_VRMS, rms_counts);
#endif
}

static void ac_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t n = fill_window();

        ac_mon_settings_t settings_snapshot;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        settings_snapshot = s_settings;
        xSemaphoreGive(s_lock);

        ac_mon_reading_t reading;
        process_window(n, &settings_snapshot, &reading);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_reading = reading;
        xSemaphoreGive(s_lock);

        /* Fire the callback outside s_lock (same "network/callout never
         * under s_lock" discipline used everywhere else in this project,
         * even though this particular callback doesn't do network I/O --
         * plug_manager_on_ac_mon_change() takes its own lock and calls
         * enqueue_action(), and nesting locks across modules is exactly the
         * kind of thing that's easy to get wrong later). Only on an actual
         * transition, not every window. */
        if (reading.have_reading && reading.in_band != s_last_in_band) {
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

    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 1024,
        .conv_frame_size = READ_CHUNK_BYTES,
    };
    esp_err_t err = adc_continuous_new_handle(&adc_config, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_new_handle failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_digi_pattern_config_t pattern[1] = {
        {
            .atten = AC_MON_ADC_ATTEN,
            .channel = AC_MON_ADC_CHANNEL,
            .unit = AC_MON_ADC_UNIT,
            .bit_width = AC_MON_ADC_BITWIDTH,
        },
    };
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = SAMPLE_RATE_HZ,
        .conv_mode = AC_MON_ADC_CONV_MODE,
        .pattern_num = 1,
        .adc_pattern = pattern,
    };
    err = adc_continuous_config(s_adc_handle, &dig_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_config failed: %s", esp_err_to_name(err));
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    err = adc_continuous_start(s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_start failed: %s", esp_err_to_name(err));
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    /* 6144, not the 4096 a task this simple would suggest: no networking
     * here (unlike the httpd/driver_http tasks that genuinely need
     * ~12288), but this project has already been bitten once by
     * underestimating how much stack ESP32-S3 needs versus classic ESP32
     * for a given call chain (see the earlier scheduler-panic stack
     * overflow) -- the ADC continuous driver's own internal calls plus
     * process_window()'s float/double math over a 4096-sample window are
     * enough uncertainty to want real headroom rather than the bare
     * minimum. */
    BaseType_t ok = xTaskCreate(ac_monitor_task, "ac_monitor", 6144, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start ac_monitor_task");
        adc_continuous_stop(s_adc_handle);
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Runtime check, not #if -- AC_MON_ADC_UNIT/ADC_UNIT_2 are C enum
     * constants, not preprocessor macros, so a #if comparing them would
     * silently evaluate both sides as 0 (any identifier the preprocessor
     * doesn't know is replaced with 0 in a #if) and always take this
     * branch regardless of the actual pin. */
    if (AC_MON_ADC_UNIT == ADC_UNIT_2) {
        ESP_LOGW(TAG, "AC mains monitor on GPIO%d uses ADC2, which is shared with the "
                      "Wi-Fi radio -- readings may be delayed or noisier while Wi-Fi is "
                      "active (always, on this device). Prefer an ADC1 pin (GPIO1-10) "
                      "if the board allows it.", AC_MON_ADC_GPIO);
    }
    ESP_LOGI(TAG, "AC mains monitor started (GPIO%d, ADC%d_CH%d, %d Hz sample rate)",
             AC_MON_ADC_GPIO, (AC_MON_ADC_UNIT == ADC_UNIT_1) ? 1 : 2,
             (int)AC_MON_ADC_CHANNEL, SAMPLE_RATE_HZ);
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
