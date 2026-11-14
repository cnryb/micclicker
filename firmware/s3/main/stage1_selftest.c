#include "sdkconfig.h"

#if CONFIG_MICCLICKER_STAGE1_SELFTEST

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_scan.h"

#define MIC_EXPECTED_PSRAM_BYTES (8U * 1024U * 1024U)
#define MIC_YIELD_WORDS (256U * 1024U / sizeof(uint32_t))
#define MIC_MAX_ERROR_LOGS 8
#define MIC_BOOT_WAIT_TIMEOUT_MS 30000

static const char *TAG = "s3_selftest";

static uint32_t test_pattern(size_t word_index, uint32_t seed)
{
    uint32_t index = (uint32_t)word_index;
    uint32_t value = index ^ seed;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value ^ (index * UINT32_C(0x9e3779b9));
}

static void fill_pattern(uint32_t *buffer, size_t words, uint32_t seed)
{
    for (size_t i = 0; i < words; ++i) {
        buffer[i] = test_pattern(i, seed);
        if ((i % MIC_YIELD_WORDS) == (MIC_YIELD_WORDS - 1)) {
            vTaskDelay(1);
        }
    }
}

static bool verify_pattern(const uint32_t *buffer, size_t words, uint32_t seed)
{
    size_t errors = 0;
    for (size_t i = 0; i < words; ++i) {
        uint32_t expected = test_pattern(i, seed);
        uint32_t actual = buffer[i];
        if (actual != expected) {
            if (errors < MIC_MAX_ERROR_LOGS) {
                ESP_LOGE(TAG, "PSRAM mismatch byte=0x%x actual=0x%08lx expected=0x%08lx",
                         (unsigned)(i * sizeof(uint32_t)), (unsigned long)actual,
                         (unsigned long)expected);
            }
            ++errors;
        }
        if ((i % MIC_YIELD_WORDS) == (MIC_YIELD_WORDS - 1)) {
            vTaskDelay(1);
        }
    }
    if (errors != 0) {
        ESP_LOGE(TAG, "PSRAM verification found %u mismatches for seed=0x%08lx",
                 (unsigned)errors, (unsigned long)seed);
        return false;
    }
    return true;
}

static bool psram_selftest(void)
{
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM_TEST: FAIL reason=not_initialized");
        return false;
    }

    size_t physical_size = esp_psram_get_size();
    size_t heap_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t test_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    test_size &= ~(sizeof(uint32_t) - 1U);
    ESP_LOGI(TAG, "PSRAM physical=%u heap=%u largest_free=%u",
             (unsigned)physical_size, (unsigned)heap_size, (unsigned)test_size);

    if (physical_size != MIC_EXPECTED_PSRAM_BYTES) {
        ESP_LOGE(TAG, "PSRAM_TEST: FAIL reason=size expected=%u actual=%u",
                 (unsigned)MIC_EXPECTED_PSRAM_BYTES, (unsigned)physical_size);
        return false;
    }
    if (test_size < (physical_size * 90U / 100U)) {
        ESP_LOGE(TAG, "PSRAM_TEST: FAIL reason=insufficient_test_coverage");
        return false;
    }
    if (!heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true)) {
        ESP_LOGE(TAG, "PSRAM_TEST: FAIL reason=heap_integrity_before_test");
        return false;
    }

    uint32_t *buffer = heap_caps_malloc(test_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "PSRAM_TEST: FAIL reason=allocation size=%u", (unsigned)test_size);
        return false;
    }

    const uint32_t seeds[] = {UINT32_C(0xa5a5f00d), UINT32_C(0x5a5a0ff1)};
    bool ok = true;
    for (size_t pass = 0; pass < sizeof(seeds) / sizeof(seeds[0]) && ok; ++pass) {
        ESP_LOGI(TAG, "PSRAM pass=%u seed=0x%08lx bytes=%u",
                 (unsigned)(pass + 1), (unsigned long)seeds[pass], (unsigned)test_size);
        fill_pattern(buffer, test_size / sizeof(uint32_t), seeds[pass]);
        ok = verify_pattern(buffer, test_size / sizeof(uint32_t), seeds[pass]);
    }
    heap_caps_free(buffer);

    if (!heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true)) {
        ESP_LOGE(TAG, "PSRAM heap integrity check failed");
        ok = false;
    }
    if (ok) {
        ESP_LOGI(TAG, "PSRAM_TEST: PASS physical_bytes=%u heap_bytes=%u tested_bytes=%u passes=2",
                 (unsigned)physical_size, (unsigned)heap_size, (unsigned)test_size);
    } else {
        ESP_LOGE(TAG, "PSRAM_TEST: FAIL reason=data_verification");
    }
    return ok;
}

static bool boot_button_selftest(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "S3_BOOT: FAIL reason=gpio_config error=%s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "S3_BOOT: WAITING action=\"Press and hold BOOT for 3 seconds while the app is running\" initial_level=%d",
             gpio_get_level(0));
    int64_t press_started_us = -1;
    int64_t wait_started_us = esp_timer_get_time();
    int last_reported_level = -1;
    int64_t last_level_log_us = 0;
    for (;;) {
        int level = gpio_get_level(0);
        bool pressed = (level == 0);
        int64_t now_us = esp_timer_get_time();
        if (press_started_us < 0 && now_us - wait_started_us >= MIC_BOOT_WAIT_TIMEOUT_MS * 1000LL) {
            ESP_LOGE(TAG, "S3_BOOT: FAIL reason=wait_timeout raw_level=%d", level);
            return false;
        }
        if (level != last_reported_level || now_us - last_level_log_us >= 1000000) {
            ESP_LOGI(TAG, "S3_BOOT: raw_level=%d", level);
            last_reported_level = level;
            last_level_log_us = now_us;
        }
        if (pressed && press_started_us < 0) {
            press_started_us = now_us;
            ESP_LOGI(TAG, "S3_BOOT: press_detected");
        } else if (!pressed && press_started_us >= 0) {
            ESP_LOGW(TAG, "S3_BOOT: early_release hold_ms=%lld; waiting for a new press",
                     (long long)((now_us - press_started_us) / 1000));
            press_started_us = -1;
        }
        if (pressed && press_started_us >= 0 && now_us - press_started_us >= 3000000) {
            ESP_LOGI(TAG, "S3_BOOT: PASS hold_ms=%lld",
                     (long long)((now_us - press_started_us) / 1000));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void micclicker_stage1_selftest_run(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "MicClicker S3 selftest model=%d revision=%d cores=%d",
             chip.model, chip.revision, chip.cores);

    // Allocate and verify PSRAM before Wi-Fi requests any dynamic memory.
    bool psram_ok = psram_selftest();
    esp_err_t wifi_err = micclicker_wifi_scan_run();
    bool wifi_ok = (wifi_err == ESP_OK);
    bool boot_ok = false;
    if (psram_ok && wifi_ok) {
        boot_ok = boot_button_selftest();
    } else {
        ESP_LOGE(TAG, "S3_BOOT: SKIP reason=prerequisite_failed");
    }

    if (psram_ok && wifi_ok && boot_ok) {
        ESP_LOGI(TAG, "SELFTEST: PASS psram=1 wifi=1 boot=1");
    } else {
        ESP_LOGE(TAG, "SELFTEST: FAIL psram=%d wifi=%d boot=%d wifi_err=%s",
                 psram_ok, wifi_ok, boot_ok, esp_err_to_name(wifi_err));
    }
}

#endif // CONFIG_MICCLICKER_STAGE1_SELFTEST
