#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "micclicker_g722_selftest.h"
#include "wifi_scan.h"

#define MIC_ADC_TRIGGER_SAMPLES 3
#define MIC_ADC_SAMPLES 400
#define MIC_ADC_INTERVAL_MS 10
#define MIC_ADC_WAIT_TIMEOUT_MS 60000

static const char *TAG = "c3_selftest";

typedef enum {
    PHASE_RELEASED = 0,
    PHASE_UP,
    PHASE_DOWN,
    PHASE_OK,
    PHASE_COUNT,
} adc_phase_id_t;

typedef struct {
    adc_phase_id_t id;
    const char *name;
    const char *instruction;
    int lower_bound_mv;
    int upper_bound_mv;
} adc_phase_t;

static const adc_phase_t PHASES[PHASE_COUNT] = {
    {PHASE_RELEASED, "RELEASED", "Release all buttons", MIC_BOARD_BUTTON_MV_OK_MAX, -1},
    {PHASE_UP, "UP", "Hold UP until the phase result appears", -1, MIC_BOARD_BUTTON_MV_UP_MAX},
    {PHASE_DOWN, "DOWN", "Hold DOWN until the phase result appears", MIC_BOARD_BUTTON_MV_UP_MAX, MIC_BOARD_BUTTON_MV_DOWN_MAX},
    {PHASE_OK, "OK", "Hold OK until the phase result appears", MIC_BOARD_BUTTON_MV_DOWN_MAX, MIC_BOARD_BUTTON_MV_OK_MAX},
};

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t calibration;
    bool calibrated;
} adc_context_t;

static adc_phase_id_t classify_mv(int mv)
{
    if (mv <= MIC_BOARD_BUTTON_MV_UP_MAX) {
        return PHASE_UP;
    }
    if (mv <= MIC_BOARD_BUTTON_MV_DOWN_MAX) {
        return PHASE_DOWN;
    }
    if (mv <= MIC_BOARD_BUTTON_MV_OK_MAX) {
        return PHASE_OK;
    }
    return PHASE_RELEASED;
}

static esp_err_t adc_initialize(adc_context_t *context)
{
    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = MIC_BOARD_BUTTON_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &context->unit);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(context->unit, MIC_BOARD_BUTTON_ADC_CHANNEL, &channel_config);
    if (err != ESP_OK) {
        return err;
    }

    adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = MIC_BOARD_BUTTON_ADC_UNIT,
        .chan = MIC_BOARD_BUTTON_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&calibration_config, &context->calibration);
    context->calibrated = (err == ESP_OK);
    if (!context->calibrated) {
        context->calibration = NULL;
        if (err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "ADC calibration initialization failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGW(TAG, "ADC curve-fitting calibration unavailable; voltages are estimates");
    }
    return ESP_OK;
}

static esp_err_t adc_read(const adc_context_t *context, int *raw, int *mv)
{
    esp_err_t err = adc_oneshot_read(context->unit, MIC_BOARD_BUTTON_ADC_CHANNEL, raw);
    if (err != ESP_OK) {
        return err;
    }
    if (context->calibrated) {
        return adc_cali_raw_to_voltage(context->calibration, *raw, mv);
    }
    *mv = (*raw * 3300) / 4095;
    return ESP_OK;
}

static bool run_adc_phase(const adc_context_t *context, const adc_phase_t *phase)
{
    ESP_LOGI(TAG, "ADC_WAITING phase=%s action=\"%s\" trigger_samples=%d samples=%d interval_ms=%d",
             phase->name, phase->instruction, MIC_ADC_TRIGGER_SAMPLES,
             MIC_ADC_SAMPLES, MIC_ADC_INTERVAL_MS);

    int consecutive_matches = 0;
    int wait_valid = 0;
    int wait_errors = 0;
    int wait_raw_min = INT_MAX;
    int wait_raw_max = INT_MIN;
    int wait_mv_min = INT_MAX;
    int wait_mv_max = INT_MIN;
    int last_raw = -1;
    int last_mv = -1;
    int64_t wait_started_us = esp_timer_get_time();
    while (consecutive_matches < MIC_ADC_TRIGGER_SAMPLES) {
        if (esp_timer_get_time() - wait_started_us >= MIC_ADC_WAIT_TIMEOUT_MS * 1000LL) {
            ESP_LOGE(TAG,
                     "ADC_PHASE: FAIL phase=%s reason=wait_timeout valid=%d errors=%d raw_min=%d raw_max=%d mv_min=%d mv_max=%d last_raw=%d last_mv=%d",
                     phase->name, wait_valid, wait_errors,
                     wait_valid > 0 ? wait_raw_min : -1, wait_valid > 0 ? wait_raw_max : -1,
                     wait_valid > 0 ? wait_mv_min : -1, wait_valid > 0 ? wait_mv_max : -1,
                     last_raw, last_mv);
            return false;
        }
        int raw = 0;
        int mv = 0;
        esp_err_t err = adc_read(context, &raw, &mv);
        if (err == ESP_OK) {
            ++wait_valid;
            last_raw = raw;
            last_mv = mv;
            wait_raw_min = raw < wait_raw_min ? raw : wait_raw_min;
            wait_raw_max = raw > wait_raw_max ? raw : wait_raw_max;
            wait_mv_min = mv < wait_mv_min ? mv : wait_mv_min;
            wait_mv_max = mv > wait_mv_max ? mv : wait_mv_max;
        } else {
            ++wait_errors;
            if (wait_errors == 1) {
                ESP_LOGE(TAG, "ADC wait read failed phase=%s error=%s",
                         phase->name, esp_err_to_name(err));
            }
        }
        if (err == ESP_OK && classify_mv(mv) == phase->id) {
            ++consecutive_matches;
        } else {
            consecutive_matches = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(MIC_ADC_INTERVAL_MS));
    }
    ESP_LOGI(TAG, "ADC_SAMPLING phase=%s", phase->name);

    int raw_min = INT_MAX;
    int raw_max = INT_MIN;
    int mv_min = INT_MAX;
    int mv_max = INT_MIN;
    int mismatches = 0;
    int valid = 0;
    uint64_t raw_sum = 0;
    uint64_t raw_sum_squares = 0;
    int64_t mv_sum = 0;

    for (int sample = 0; sample < MIC_ADC_SAMPLES; ++sample) {
        int raw = 0;
        int mv = 0;
        esp_err_t err = adc_read(context, &raw, &mv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed phase=%s sample=%d error=%s",
                     phase->name, sample, esp_err_to_name(err));
        } else {
            ++valid;
            raw_min = raw < raw_min ? raw : raw_min;
            raw_max = raw > raw_max ? raw : raw_max;
            mv_min = mv < mv_min ? mv : mv_min;
            mv_max = mv > mv_max ? mv : mv_max;
            raw_sum += (uint32_t)raw;
            raw_sum_squares += (uint32_t)raw * (uint32_t)raw;
            mv_sum += mv;
            if (classify_mv(mv) != phase->id) {
                ++mismatches;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(MIC_ADC_INTERVAL_MS));
    }

    if (valid == 0) {
        ESP_LOGE(TAG, "ADC_PHASE: FAIL phase=%s reason=no_valid_samples", phase->name);
        return false;
    }

    double raw_mean = (double)raw_sum / valid;
    double variance = (double)raw_sum_squares / valid - raw_mean * raw_mean;
    if (variance < 0.0) {
        variance = 0.0;
    }
    int raw_std_milli = (int)(sqrt(variance) * 1000.0);
    int raw_mean_milli = (int)(raw_mean * 1000.0);
    int mv_mean = (int)(mv_sum / valid);

    ESP_LOGI(TAG,
             "ADC_RESULT phase=%s valid=%d raw_min=%d raw_max=%d raw_mean_milli=%d raw_std_milli=%d mv_min=%d mv_max=%d mv_mean=%d calibrated=%d mismatches=%d",
             phase->name, valid, raw_min, raw_max, raw_mean_milli, raw_std_milli,
             mv_min, mv_max, mv_mean, context->calibrated, mismatches);

    int lower_margin = phase->lower_bound_mv >= 0 ? mv_min - phase->lower_bound_mv : 0;
    int upper_margin = phase->upper_bound_mv >= 0 ? phase->upper_bound_mv - mv_max : 0;
    ESP_LOGI(TAG, "ADC_MARGIN phase=%s lower_mv=%d upper_mv=%d",
             phase->name, lower_margin, upper_margin);

    bool margins_ok = (phase->lower_bound_mv < 0 || mv_min > phase->lower_bound_mv) &&
                      (phase->upper_bound_mv < 0 || mv_max <= phase->upper_bound_mv);
    bool ok = (valid == MIC_ADC_SAMPLES && mismatches == 0 && margins_ok);
    if (ok) {
        ESP_LOGI(TAG, "ADC_PHASE: PASS phase=%s", phase->name);
    } else {
        ESP_LOGE(TAG, "ADC_PHASE: FAIL phase=%s", phase->name);
    }
    return ok;
}

static void selftest_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "MicClicker C3 selftest model=%d revision=%d cores=%d",
             chip.model, chip.revision, chip.cores);

    // Codec check runs first: it needs no user interaction and failing fast is more
    // useful than running the button phases before reporting a broken codec.
    bool g722_ok = micclicker_g722_selftest_run();

    esp_err_t wifi_err = micclicker_wifi_scan_run();

    adc_context_t adc = {0};
    esp_err_t adc_err = adc_initialize(&adc);
    bool adc_ok = (adc_err == ESP_OK);
    if (adc_err != ESP_OK) {
        ESP_LOGE(TAG, "ADC initialization failed: %s", esp_err_to_name(adc_err));
    } else {
        for (size_t i = 0; i < PHASE_COUNT; ++i) {
            if (!run_adc_phase(&adc, &PHASES[i])) {
                adc_ok = false;
            }
        }
    }

    if (wifi_err == ESP_OK && adc_ok && adc.calibrated && g722_ok) {
        ESP_LOGI(TAG, "SELFTEST: PASS wifi=1 adc=1 adc_calibrated=1 g722=1");
    } else if (wifi_err == ESP_OK && adc_ok && g722_ok) {
        ESP_LOGW(TAG, "SELFTEST: PASS_LIMITED wifi=1 adc=1 adc_calibrated=0 g722=1");
    } else {
        ESP_LOGE(TAG, "SELFTEST: FAIL wifi=%d adc=%d g722=%d wifi_err=%s adc_init_err=%s",
                 wifi_err == ESP_OK, adc_ok, g722_ok, esp_err_to_name(wifi_err),
                 esp_err_to_name(adc_err));
    }
}

void micclicker_c3_selftest_main(void)
{
    selftest_main();
}
