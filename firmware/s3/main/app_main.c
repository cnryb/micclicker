#include "sdkconfig.h"

#if CONFIG_MICCLICKER_STAGE1_SELFTEST
void micclicker_stage1_selftest_run(void);

void app_main(void)
{
    micclicker_stage1_selftest_run();
}
#elif CONFIG_MICCLICKER_ESPNOW_PROBE_RECEIVER
#include "esp_log.h"
#include "mic_probe.h"

static const char *TAG = "s3_app";

void app_main(void)
{
    // The probe does not enumerate USB: it owns the radio and the console only. That
    // keeps the measurement free of USB traffic on the same core.
    esp_err_t err = mic_probe_run_receiver(CONFIG_MICCLICKER_ESPNOW_PROBE_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Starting the ESP-NOW receiver probe failed: %s", esp_err_to_name(err));
    }
}
#else
void micclicker_usb_prototype_start(void);

void app_main(void)
{
    micclicker_usb_prototype_start();
}
#endif
