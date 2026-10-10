#include "zmpt101b.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "ZMPT101B";

// --- АЦП у безперервному режимі (DMA) ---
#define ZMPT_DECIM_DEFAULT    4      // скільки відліків АЦП усереднюється в один вихідний (АЦП = fs * 4 = 20 кГц)
#define ZMPT_FRAME_CONV       256    // перетворень у кадрі DMA (12.8 мс при 20 кГц)
#define ZMPT_POOL_CONV        2048   // ємність буфера драйвера в перетвореннях (~100 мс при 20 кГц)
#define ZMPT_WARMUP_RAW       2000   // скільки перших відліків відкинути після старту (~100 мс при 20 кГц)
#define ADC_CODES             4096   // 12 біт

// --- Аналіз вікна ---
#define ZC_HYST               0.35   // половина смуги гістерезису, частка амплітуди
#define ZC_MAX                32     // макс. перетинів, що запам'ятовуються у вікні
#define FREQ_MIN_HZ           40.0f
#define FREQ_MAX_HZ           70.0f

struct zmpt101b_dev_s {
    zmpt101b_config_t cfg;
    adc_continuous_handle_t adc_handle;
    adc_cali_handle_t cali_handle;
    bool do_cali;
    uint32_t decim;                  // відліків АЦП на один вихідний відлік
    float *mv_lut;                   // код АЦП (0..4095) -> мВ, будується один раз при init
    float *last_win;                 // копія останнього вікна (мВ) для діагностики
    uint32_t win_n;                  // довжина вікна у відліках
    bool win_ready;
    volatile bool pool_ovf;          // виставляється з ISR при переповненні буфера DMA

    TaskHandle_t task_handle;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done_sem;      // таск сигналізує про завершення
    volatile bool is_running;

    zmpt101b_data_t current_data;
    float last_raw_rms_mv;           // для автокалібрування
    uint32_t seq;                    // номер останнього готового вікна
};

typedef struct {
    float rms_mv;       // RMS змінної складової (по цілому числу періодів, якщо їх знайдено)
    float dc_mv;        // DC (там же)
    float freq_hz;      // 0 якщо не вдалося надійно виміряти
    float noise_mv;     // оцінка шуму
    uint32_t zc_n;      // підтверджених перетинів
} win_result_t;

static bool init_adc_calibration(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle) {
    adc_cali_handle_t handle = NULL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &handle) == ESP_OK) calibrated = true;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_config, &handle) == ESP_OK) calibrated = true;
    }
#endif

    (void)unit; (void)channel; (void)atten;
    *out_handle = handle;
    return calibrated;
}

// Підбираємо коефіцієнт усереднення так, щоб fs_adc = fs_out * decim лишався в межах, які підтримує АЦП S3
static uint32_t pick_decim(uint32_t fs_out) {
    uint32_t d = ZMPT_DECIM_DEFAULT;
    while (d > 1 && fs_out * d > SOC_ADC_SAMPLE_FREQ_THRES_HIGH) d--;
    while (fs_out * d < SOC_ADC_SAMPLE_FREQ_THRES_LOW) d++;
    return d;
}

static bool IRAM_ATTR zmpt_on_pool_ovf(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data) {
    (void)handle; (void)edata;
    ((struct zmpt101b_dev_s *)user_data)->pool_ovf = true;
    return false;
}

/**
 * Аналіз одного вікна (чиста функція, без обмежень на RTOS).
 *
 * Перетин нуля вгору рахується тільки після того, як сигнал побував нижче (DC - h) і
 * піднявся вище (DC + h): шум нижче h не створює хибних перетинів. Момент перетину
 * знаходиться як нуль прямої, підігнаної МНК по всіх відліках між цими порогами
 * (шум усереднюється, а не береться з двох сусідніх відліків). Частота береться з
 * лінійної регресії часів усіх перетинів. DC і RMS рахуються по цілому числу періодів.
 */
static void analyze_window(const float *s, uint32_t n, double fs, win_result_t *r) {
    memset(r, 0, sizeof(*r));
    if (n < 8) return;

    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) sum += s[i];
    const double dc_full = sum / (double)n;

    double sq = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        const double d = (double)s[i] - dc_full;
        sq += d * d;
    }
    const double rms_full = sqrt(sq / (double)n);
    r->dc_mv = (float)dc_full;
    r->rms_mv = (float)rms_full;

    // Оцінка шуму: для синусоїди друга різниця мала, для білого шуму var(d2) = 6 sigma^2
    double d2sq = 0.0;
    for (uint32_t i = 1; i + 1 < n; i++) {
        const double d2 = (double)s[i + 1] - 2.0 * (double)s[i] + (double)s[i - 1];
        d2sq += d2 * d2;
    }
    r->noise_mv = (float)sqrt(d2sq / (double)(n - 2) / 6.0);

    if (rms_full < 1.0) return;   // сигналу немає

    // Перетини нуля з гістерезисом
    const double h = ZC_HYST * 1.41421356237 * rms_full;
    double zc[ZC_MAX];
    uint32_t zn = 0;
    bool armed = false;
    uint32_t lo = 0;

    for (uint32_t i = 0; i < n; i++) {
        const double x = (double)s[i] - dc_full;
        if (x < -h) {
            armed = true;
            lo = i;                      // останній відлік нижче нижнього порога
        } else if (armed && x > h) {
            const uint32_t cnt = i - lo + 1;
            double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            for (uint32_t j = lo; j <= i; j++) {
                const double xi = (double)(j - lo);
                const double yi = (double)s[j] - dc_full;
                sx += xi; sy += yi; sxx += xi * xi; sxy += xi * yi;
            }
            const double den = (double)cnt * sxx - sx * sx;
            if (den > 0.0) {
                const double b = ((double)cnt * sxy - sx * sy) / den;
                const double a = (sy - b * sx) / (double)cnt;
                if (b > 0.0 && zn < ZC_MAX) zc[zn++] = (double)lo + (-a / b);
            }
            armed = false;
        }
    }
    r->zc_n = zn;

    uint32_t i0 = 0, i1 = n;
    if (zn >= 2) {
        // Регресія zc[k] = z0 + k * P, звідки період P (у відліках)
        double sk = 0.0, sz = 0.0, skk = 0.0, skz = 0.0;
        for (uint32_t k = 0; k < zn; k++) {
            sk += k; sz += zc[k]; skk += (double)k * k; skz += (double)k * zc[k];
        }
        const double den = (double)zn * skk - sk * sk;
        const double P = ((double)zn * skz - sk * sz) / den;
        if (P > 0.0) r->freq_hz = (float)(fs / P);

        i0 = (uint32_t)ceil(zc[0]);
        i1 = (uint32_t)ceil(zc[zn - 1]);
        if (i1 <= i0 || i1 > n) { i0 = 0; i1 = n; }
    }

    if (i0 != 0 || i1 != n) {
        const uint32_t m = i1 - i0;
        double s1 = 0.0;
        for (uint32_t i = i0; i < i1; i++) s1 += s[i];
        const double dc = s1 / (double)m;
        double q = 0.0;
        for (uint32_t i = i0; i < i1; i++) {
            const double d = (double)s[i] - dc;
            q += d * d;
        }
        r->dc_mv = (float)dc;
        r->rms_mv = (float)sqrt(q / (double)m);
    }

    // Білий шум додає свою потужність до RMS: rms^2 = signal^2 + noise^2
    const double n2 = (double)r->noise_mv * (double)r->noise_mv;
    const double r2 = (double)r->rms_mv * (double)r->rms_mv;
    r->rms_mv = (r2 > n2) ? (float)sqrt(r2 - n2) : 0.0f;
}

static void zmpt101b_task(void *arg) {
    zmpt101b_handle_t dev = (zmpt101b_handle_t)arg;

    const uint32_t n = (dev->cfg.sample_rate_hz * dev->cfg.window_ms) / 1000;
    const uint32_t decim = dev->decim;
    const uint32_t frame_bytes = ZMPT_FRAME_CONV * SOC_ADC_DIGI_RESULT_BYTES;

    float *s = malloc(sizeof(float) * n);
    uint8_t *buf = malloc(frame_bytes);
    adc_continuous_data_t *parsed = malloc(sizeof(adc_continuous_data_t) * ZMPT_FRAME_CONV);
    bool started = false;

    if (!s || !buf || !parsed) {
        ESP_LOGE(TAG, "Allocation error for measurement task buffers");
        dev->is_running = false;
        goto out;
    }

    dev->pool_ovf = false;
    if (adc_continuous_start(dev->adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "adc_continuous_start failed");
        dev->is_running = false;
        goto out;
    }
    started = true;

    uint32_t idx = 0;                      // скільки вихідних відліків у поточному вікні
    uint32_t acc_n = 0;                    // скільки відліків АЦП накопичено в поточному вихідному
    float acc = 0.0f;
    float vmin = 1e9f, vmax = -1e9f;
    uint32_t warm = ZMPT_WARMUP_RAW;
    uint32_t ovf_total = 0;
    uint32_t timeouts = 0;
    bool warned_channel = false;
    uint16_t rmin = ADC_CODES - 1, rmax = 0;   // мін/макс сирих кодів у вікні

    // Вимір реальної частоти дискретизації: точки відліку беруться в кінці кадрів
    uint64_t raw_total = 0;
    bool rate_started = false;
    int64_t t_ref = 0;
    uint64_t raw_ref = 0;
    float adc_rate = 0.0f;

    while (dev->is_running) {
        uint32_t got = 0;
        const esp_err_t er = adc_continuous_read(dev->adc_handle, buf, frame_bytes, &got, 100);
        if (er == ESP_ERR_TIMEOUT) {
            if (++timeouts == 10) ESP_LOGW(TAG, "No ADC data for 1 s");
            continue;
        }
        timeouts = 0;
        if (er == ESP_ERR_INVALID_STATE) {   // АЦП швидший за обробку: буфер переповнений
            dev->pool_ovf = true;
        } else if (er != ESP_OK) {
            ESP_LOGE(TAG, "adc_continuous_read failed: %s", esp_err_to_name(er));
            dev->is_running = false;
            break;
        }

        const int64_t t_now = esp_timer_get_time();

        if (dev->pool_ovf) {                 // втрачені відліки -> вікно порушене, починаємо заново
            dev->pool_ovf = false;
            ovf_total++;
            idx = 0; acc_n = 0; acc = 0.0f;
            vmin = 1e9f; vmax = -1e9f;
            warm = ZMPT_WARMUP_RAW;
            rate_started = false;
            if (er != ESP_OK) continue;
        }

        got -= got % SOC_ADC_DIGI_RESULT_BYTES;
        uint32_t cnt = 0;
        if (got == 0 || adc_continuous_parse_data(dev->adc_handle, buf, got, parsed, &cnt) != ESP_OK) continue;

        for (uint32_t i = 0; i < cnt; i++) {
            if (!parsed[i].valid) continue;
            if (!warned_channel && parsed[i].channel != dev->cfg.adc_channel) {
                ESP_LOGW(TAG, "Unexpected ADC channel %d in data (expected %d)",
                         (int)parsed[i].channel, (int)dev->cfg.adc_channel);
                warned_channel = true;
            }
            raw_total++;
            if (warm) { warm--; continue; }

            const uint16_t code = (uint16_t)(parsed[i].raw_data & (ADC_CODES - 1));
            if (code < rmin) rmin = code;
            if (code > rmax) rmax = code;
            acc += dev->mv_lut[code];
            if (++acc_n < decim) continue;

            const float x = acc / (float)decim;
            acc = 0.0f; acc_n = 0;
            s[idx++] = x;
            if (x < vmin) vmin = x;
            if (x > vmax) vmax = x;
            if (idx < n) continue;

            // --- вікно зібране: аналіз і публікація ---
            win_result_t res;
            analyze_window(s, n, (double)dev->cfg.sample_rate_hz, &res);

            float f = res.freq_hz;
            if (f < FREQ_MIN_HZ || f > FREQ_MAX_HZ) f = 0.0f;   // вікно ненадійне

            xSemaphoreTake(dev->lock, portMAX_DELAY);

            const float v = res.rms_mv * dev->cfg.calibration_factor;
            const float av = dev->cfg.filter_alpha_v;
            const float af = dev->cfg.filter_alpha_f;
            const bool first = !dev->current_data.is_valid;

            // Без EMA на вході/виході з "немає напруги": інакше після обриву мережі згладжене значення
            // повільно повзе до нуля (і так само повільно підіймається назад), а AC1 має бачити обрив одразу.
            const bool snap = first || av <= 0.0f || av > 1.0f
                           || v < dev->cfg.min_voltage_threshold
                           || dev->current_data.voltage_rms < dev->cfg.min_voltage_threshold;
            if (snap) {
                dev->current_data.voltage_rms = v;
            } else {
                dev->current_data.voltage_rms = av * v + (1.0f - av) * dev->current_data.voltage_rms;
            }

            if (v < dev->cfg.min_voltage_threshold) {
                dev->current_data.frequency_hz = 0.0f;          // немає напруги
            } else if (f > 0.0f) {
                if (af > 0.0f && af <= 1.0f && dev->current_data.frequency_hz > 0.0f) {
                    dev->current_data.frequency_hz = af * f + (1.0f - af) * dev->current_data.frequency_hz;
                } else {
                    dev->current_data.frequency_hz = f;
                }
            }   // інакше тримаємо попереднє значення

            dev->current_data.freq_raw_hz = f;
            dev->current_data.zc_count = res.zc_n;
            dev->current_data.dc_offset_mv = res.dc_mv;
            dev->current_data.vpp_mv = vmax - vmin;
            dev->current_data.noise_mv = res.noise_mv;
            dev->current_data.oversample = decim;
            dev->current_data.adc_rate_hz = adc_rate;
            dev->current_data.pool_overflows = ovf_total;
            dev->current_data.adc_raw_min = rmin;
            dev->current_data.adc_raw_max = rmax;
            memcpy(dev->last_win, s, sizeof(float) * n);
            dev->win_ready = true;
            dev->current_data.is_valid = true;
            dev->last_raw_rms_mv = res.rms_mv;
            dev->seq++;

            xSemaphoreGive(dev->lock);

            idx = 0;
            vmin = 1e9f; vmax = -1e9f;
            rmin = ADC_CODES - 1; rmax = 0;
        }

        // Реальна частота дискретизації: відліки між двома кінцями кадрів / час між ними
        if (!rate_started && warm == 0) {
            rate_started = true;
            t_ref = t_now;
            raw_ref = raw_total;
        } else if (rate_started && (t_now - t_ref) >= 2000000) {
            adc_rate = (float)((double)(raw_total - raw_ref) * 1e6 / (double)(t_now - t_ref));
        }
    }

out:
    if (started) adc_continuous_stop(dev->adc_handle);
    free(parsed);
    free(buf);
    free(s);
    xSemaphoreGive(dev->done_sem);
    vTaskDelete(NULL);
}

esp_err_t zmpt101b_init(const zmpt101b_config_t *config, zmpt101b_handle_t *out_handle) {
    if (!config || !out_handle) return ESP_ERR_INVALID_ARG;
    if (config->sample_rate_hz == 0 || config->window_ms == 0) return ESP_ERR_INVALID_ARG;

    zmpt101b_handle_t dev = calloc(1, sizeof(struct zmpt101b_dev_s));
    if (!dev) return ESP_ERR_NO_MEM;

    memcpy(&dev->cfg, config, sizeof(zmpt101b_config_t));
    dev->decim = pick_decim(dev->cfg.sample_rate_hz);

    esp_err_t err = ESP_ERR_NO_MEM;
    bool have_adc = false;

    dev->lock = xSemaphoreCreateMutex();
    dev->done_sem = xSemaphoreCreateBinary();
    dev->mv_lut = malloc(sizeof(float) * ADC_CODES);
    dev->win_n = (dev->cfg.sample_rate_hz * dev->cfg.window_ms) / 1000;
    dev->last_win = calloc(dev->win_n, sizeof(float));
    if (!dev->lock || !dev->done_sem || !dev->mv_lut || !dev->last_win) goto fail;

    // Калібрування АЦП -> таблиця код -> мВ (одразу для всіх 4096 кодів, щоб не рахувати криву на кожному відліку)
    dev->do_cali = init_adc_calibration(dev->cfg.adc_unit, dev->cfg.adc_channel, dev->cfg.atten, &dev->cali_handle);
    if (!dev->do_cali) {
        ESP_LOGW(TAG, "ADC calibration scheme unavailable, using linear 0..3300 mV approximation");
    }
    for (int code = 0; code < ADC_CODES; code++) {
        int mv = 0;
        if (dev->do_cali && adc_cali_raw_to_voltage(dev->cali_handle, code, &mv) == ESP_OK) {
            dev->mv_lut[code] = (float)mv;
        } else {
            dev->mv_lut[code] = (float)(code * 3300) / 4095.0f;
        }
    }

    adc_continuous_handle_cfg_t hcfg = {
        .max_store_buf_size = ZMPT_POOL_CONV * SOC_ADC_DIGI_RESULT_BYTES,
        .conv_frame_size = ZMPT_FRAME_CONV * SOC_ADC_DIGI_RESULT_BYTES,
    };
    err = adc_continuous_new_handle(&hcfg, &dev->adc_handle);
    if (err != ESP_OK) goto fail;
    have_adc = true;

    adc_digi_pattern_config_t pattern = {
        .atten = dev->cfg.atten,
        .channel = (uint8_t)dev->cfg.adc_channel,   // без маски: на S3 ADC1 має канали 0..9
        .unit = dev->cfg.adc_unit,
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };
    adc_continuous_config_t dcfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = dev->cfg.sample_rate_hz * dev->decim,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
    };
    err = adc_continuous_config(dev->adc_handle, &dcfg);
    if (err != ESP_OK) goto fail;

    adc_continuous_evt_cbs_t cbs = { .on_pool_ovf = zmpt_on_pool_ovf };
    err = adc_continuous_register_event_callbacks(dev->adc_handle, &cbs, dev);
    if (err != ESP_OK) goto fail;

    ESP_LOGI(TAG, "ADC continuous: %lu Hz x%lu -> %lu Hz output",
             (unsigned long)(dev->cfg.sample_rate_hz * dev->decim), (unsigned long)dev->decim,
             (unsigned long)dev->cfg.sample_rate_hz);

    *out_handle = dev;
    return ESP_OK;

fail:
    if (have_adc) adc_continuous_deinit(dev->adc_handle);
    if (dev->do_cali) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(dev->cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(dev->cali_handle);
#endif
    }
    if (dev->lock) vSemaphoreDelete(dev->lock);
    if (dev->done_sem) vSemaphoreDelete(dev->done_sem);
    free(dev->mv_lut);
    free(dev->last_win);
    free(dev);
    return err;
}

esp_err_t zmpt101b_start(zmpt101b_handle_t handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->task_handle) return ESP_OK;

    handle->is_running = true;   // до створення таску, щоб подвійний start не створив два таски
    if (xTaskCreatePinnedToCore(zmpt101b_task, "zmpt101b_task", 4096, handle, 5,
                                &handle->task_handle, 1) != pdPASS) {
        handle->is_running = false;
        handle->task_handle = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t zmpt101b_stop(zmpt101b_handle_t handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (!handle->task_handle) return ESP_OK;

    handle->is_running = false;
    const TickType_t to = pdMS_TO_TICKS(2 * handle->cfg.window_ms + 500);
    if (xSemaphoreTake(handle->done_sem, to) != pdTRUE) return ESP_ERR_TIMEOUT;
    handle->task_handle = NULL;
    return ESP_OK;
}

esp_err_t zmpt101b_get_data(zmpt101b_handle_t handle, zmpt101b_data_t *data) {
    if (!handle || !data) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    memcpy(data, &handle->current_data, sizeof(zmpt101b_data_t));
    xSemaphoreGive(handle->lock);

    return ESP_OK;
}

esp_err_t zmpt101b_get_window(zmpt101b_handle_t handle, float *out, uint32_t max_samples, uint32_t *n_out) {
    if (!handle || !out || !n_out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    uint32_t m = 0;
    if (handle->win_ready) {
        m = (handle->win_n < max_samples) ? handle->win_n : max_samples;
        memcpy(out, handle->last_win, sizeof(float) * m);
    }
    xSemaphoreGive(handle->lock);

    *n_out = m;
    return ESP_OK;
}

esp_err_t zmpt101b_auto_calibrate(zmpt101b_handle_t handle, float reference_v_rms,
                                  uint32_t duration_ms, float *out_new_factor) {
    if (!handle || reference_v_rms <= 0.0f || duration_ms < 500) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Starting Auto-Calibration procedure for %lu ms...", (unsigned long)duration_ms);

    double acc = 0.0;
    uint32_t cnt = 0;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    uint32_t last_seq = handle->seq;
    xSemaphoreGive(handle->lock);

    const int64_t t_end = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    while (esp_timer_get_time() < t_end) {
        vTaskDelay(pdMS_TO_TICKS(10));
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        if (handle->seq != last_seq) {            // кожне вікно рахується один раз
            last_seq = handle->seq;
            if (handle->last_raw_rms_mv > 1.0f) { // відсікаємо відсутній сигнал
                acc += handle->last_raw_rms_mv;
                cnt++;
            }
        }
        xSemaphoreGive(handle->lock);
    }

    if (cnt == 0) {
        ESP_LOGE(TAG, "Auto-Calibration failed: no signal detected on ADC input!");
        return ESP_FAIL;
    }

    const float avg_raw_rms_mv = (float)(acc / cnt);
    const float k = reference_v_rms / avg_raw_rms_mv;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->cfg.calibration_factor = k;
    handle->current_data.voltage_rms = reference_v_rms;   // скидаємо EMA під новий масштаб
    xSemaphoreGive(handle->lock);

    if (out_new_factor) *out_new_factor = k;

    ESP_LOGI(TAG, "Auto-Calibration completed! %lu windows, Avg Raw mV: %.2f, New Factor: %f",
             (unsigned long)cnt, avg_raw_rms_mv, k);
    return ESP_OK;
}

esp_err_t zmpt101b_set_calibration_factor(zmpt101b_handle_t handle, float k) {
    if (!handle || !(k > 0.0f)) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    const float old = handle->cfg.calibration_factor;
    handle->cfg.calibration_factor = k;
    // Згладжена напруга лежить у старому масштабі: перемасштабуємо її, щоб показ не "стрибав" через EMA
    if (old > 0.0f) handle->current_data.voltage_rms *= k / old;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

float zmpt101b_get_calibration_factor(zmpt101b_handle_t handle) {
    if (!handle) return 0.0f;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    const float k = handle->cfg.calibration_factor;
    xSemaphoreGive(handle->lock);
    return k;
}

esp_err_t zmpt101b_deinit(zmpt101b_handle_t handle) {
    if (!handle) return ESP_ERR_INVALID_ARG;

    // Не звільняємо пам'ять, поки таск не підтвердив завершення (він же зупиняє АЦП)
    if (zmpt101b_stop(handle) != ESP_OK) return ESP_ERR_TIMEOUT;

    adc_continuous_deinit(handle->adc_handle);

    if (handle->do_cali) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(handle->cali_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(handle->cali_handle);
#endif
    }

    vSemaphoreDelete(handle->lock);
    vSemaphoreDelete(handle->done_sem);
    free(handle->mv_lut);
    free(handle->last_win);
    free(handle);
    return ESP_OK;
}
