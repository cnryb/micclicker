#include "sdkconfig.h"

#if CONFIG_MICCLICKER_ESPNOW_PROBE_SENDER
#include "esp_log.h"
#include "mic_probe.h"

static const char *TAG = "c3_app";

void app_main(void)
{
    // The probe needs a radio and nothing else; the ADC and G.722 selftests are a
    // separate build because they require interaction that would stall a radio run.
    esp_err_t err = mic_probe_run_sender(CONFIG_MICCLICKER_ESPNOW_PROBE_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Starting the ESP-NOW sender probe failed: %s", esp_err_to_name(err));
    }
}
#else
void micclicker_c3_selftest_main(void);

void app_main(void)
{
    micclicker_c3_selftest_main();
}
#endif
