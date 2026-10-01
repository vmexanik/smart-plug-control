#ifndef ZMPT101B_H
#define ZMPT101B_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_adc/adc_continuous.h"   // типи adc_unit_t / adc_channel_t / adc_atten_t

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    adc_unit_t adc_unit;           ///< ADC_UNIT_1 (DMA-режим на ESP32-S3 працює тільки з ADC1)
    adc_channel_t adc_channel;     ///< Наприклад, ADC_CHANNEL_9 (GPIO10 на ESP32-S3)
    adc_atten_t atten;             ///< ADC_ATTEN_DB_12 (діапазон до ~3.1 В)
    float calibration_factor;      ///< Коефіцієнт масштабування, В мережі на мВ RMS на вході АЦП
    uint32_t sample_rate_hz;       ///< Вихідна частота відліків для аналізу (5000 Гц); АЦП працює у 4 рази швидше
    uint16_t window_ms;            ///< Вікно вимірювання в мс (кратне 20 мс: 100 або 200 мс)
    float filter_alpha_v;          ///< EMA-фільтр напруги (0.1..0.2)
    float filter_alpha_f;          ///< EMA-фільтр частоти (0.05..0.15)
    float min_voltage_threshold;   ///< Поріг напруги (В), нижче якого частота вважається 0 Гц
} zmpt101b_config_t;

typedef struct {
    float voltage_rms;             ///< Діюча напруга (В)
    float frequency_hz;            ///< Частота мережі (Гц), після EMA
    float freq_raw_hz;             ///< Частота за останнє вікно без згладжування (0 = вікно відкинуто)
    uint32_t zc_count;             ///< Кількість підтверджених перетинів нуля у вікні (очікується 4-5 при 100 мс)
    float dc_offset_mv;            ///< Поточний DC-зсув (мВ)
    float vpp_mv;                  ///< Розмах сигналу на вході АЦП (мВ), для виявлення кліпу
    float noise_mv;                ///< Оцінка шуму (мВ RMS) за другою різницею, вже після усереднення
    uint32_t oversample;           ///< Скільки відліків АЦП усереднюється в одному вихідному відліку
    float adc_rate_hz;             ///< Виміряна реальна частота дискретизації АЦП (0 доки не виміряна, ~2 с)
    uint32_t pool_overflows;       ///< Скільки разів переповнювався буфер DMA (дані за цей час відкинуто)
    uint16_t adc_raw_min;          ///< Мін. сирий код АЦП у вікні (0..4095), для діагностики
    uint16_t adc_raw_max;          ///< Макс. сирий код АЦП у вікні; 4095 або близько = кліп
    bool is_valid;                 ///< Прапор придатності сигналу
} zmpt101b_data_t;

typedef struct zmpt101b_dev_s* zmpt101b_handle_t;

/** @brief Ініціалізація модуля (АЦП у безперервному DMA-режимі) */
esp_err_t zmpt101b_init(const zmpt101b_config_t *config, zmpt101b_handle_t *out_handle);

/** @brief Запуск таску вимірювання */
esp_err_t zmpt101b_start(zmpt101b_handle_t handle);

/** @brief Зупинка таску вимірювання (блокує до фактичного завершення таску) */
esp_err_t zmpt101b_stop(zmpt101b_handle_t handle);

/** @brief Зчитування комплексних даних (Напруга + Частота) */
esp_err_t zmpt101b_get_data(zmpt101b_handle_t handle, zmpt101b_data_t *data);

/**
 * @brief Копія останнього вікна (відліки в мВ після усереднення, без жодної обробки) для діагностики.
 * @param out     буфер на max_samples значень
 * @param n_out   скільки відліків скопійовано (0 якщо вікно ще не готове)
 */
esp_err_t zmpt101b_get_window(zmpt101b_handle_t handle, float *out, uint32_t max_samples, uint32_t *n_out);

/**
 * @brief Процедура автокалібрування коефіцієнта за еталонною напругою.
 *        Викликати лише за реального еталонного значення (мультиметр), а не константи.
 */
esp_err_t zmpt101b_auto_calibrate(zmpt101b_handle_t handle, float reference_v_rms, uint32_t duration_ms, float *out_new_factor);

/** @brief Деініціалізація модуля */
esp_err_t zmpt101b_deinit(zmpt101b_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // ZMPT101B_H
