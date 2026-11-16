#include "sdkconfig.h"

#if !CONFIG_MICCLICKER_STAGE1_SELFTEST && !CONFIG_MICCLICKER_ESPNOW_PROBE_RECEIVER

#include "mic_led.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mic_led";

// WS2812B timing at a 10 MHz RMT resolution (one tick is 0.1 us).
// Zero bit: 0.3 us high + 0.9 us low. One bit: 0.8 us high + 0.5 us low.
// A low period of at least 50 us latches the frame; 300 us leaves margin for
// the variants that want a longer reset.
#define LED_RMT_RESOLUTION_HZ (10 * 1000 * 1000)
#define LED_ZERO_HIGH_TICKS 3
#define LED_ZERO_LOW_TICKS 9
#define LED_ONE_HIGH_TICKS 8
#define LED_ONE_LOW_TICKS 5
#define LED_RESET_TICKS 3000

// Two frames per update: the first can be swallowed while the line settles
// after the pin is handed to the RMT peripheral.
#define LED_FRAMES_PER_UPDATE 2

#define LED_BUTTON_GPIO 0
#define LED_BUTTON_POLL_MS 20
#define LED_BUTTON_DEBOUNCE_POLLS 3

// Held-button colour: bright enough to read across a desk, dim enough not to
// glare. "Off" is the normal state, so nothing else ever turns the LED on.
#define LED_BUTTON_LEVEL 80

static bool s_lit;

static void fill_frame(rmt_symbol_word_t *frame, uint8_t red, uint8_t green, uint8_t blue)
{
    // Addressable LEDs take green first.
    const uint32_t grb = ((uint32_t)green << 16) | ((uint32_t)red << 8) | (uint32_t)blue;
    for (int i = 0; i < 24; ++i) {
        const bool one = ((grb >> (23 - i)) & 1u) != 0;
        frame[i].level0 = 1;
        frame[i].duration0 = one ? LED_ONE_HIGH_TICKS : LED_ZERO_HIGH_TICKS;
        frame[i].level1 = 0;
        frame[i].duration1 = one ? LED_ONE_LOW_TICKS : LED_ZERO_LOW_TICKS;
    }
    frame[24].level0 = 0;
    frame[24].duration0 = LED_RESET_TICKS;
    frame[24].level1 = 0;
    frame[24].duration1 = 0;
}

void mic_led_set(uint8_t red, uint8_t green, uint8_t blue)
{
    rmt_symbol_word_t frame[25];
    fill_frame(frame, red, green, blue);

    rmt_channel_handle_t channel = NULL;
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = CONFIG_MICCLICKER_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    if (rmt_new_tx_channel(&channel_config, &channel) != ESP_OK) {
        return;
    }

    const rmt_copy_encoder_config_t encoder_config = {};
    rmt_encoder_handle_t encoder = NULL;
    if (rmt_new_copy_encoder(&encoder_config, &encoder) != ESP_OK) {
        rmt_del_channel(channel);
        return;
    }
    if (rmt_enable(channel) != ESP_OK) {
        rmt_del_encoder(encoder);
        rmt_del_channel(channel);
        return;
    }

    const rmt_transmit_config_t transmit_config = {.loop_count = 0};
    for (int i = 0; i < LED_FRAMES_PER_UPDATE; ++i) {
        if (rmt_transmit(channel, encoder, frame, sizeof(frame), &transmit_config) == ESP_OK) {
            rmt_tx_wait_all_done(channel, 50);
        }
    }

    // Dropping the channel releases the pin, so the LED keeps its latched
    // colour while the line floats instead of being held at a level.
    rmt_disable(channel);
    rmt_del_encoder(encoder);
    rmt_del_channel(channel);
}

void mic_led_off(void)
{
    mic_led_set(0, 0, 0);
}

static void enable_button_input(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << LED_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
}

// Light the LED only while BOOT is held. This gives a physical way to check the
// LED without a config tool: the normal state stays dark.
static void led_task(void *argument)
{
    (void)argument;
    int low_polls = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_BUTTON_POLL_MS));
        if (gpio_get_level(LED_BUTTON_GPIO) == 0) {
            if (low_polls < LED_BUTTON_DEBOUNCE_POLLS) {
                ++low_polls;
            }
        } else {
            low_polls = 0;
        }

        const bool held = low_polls >= LED_BUTTON_DEBOUNCE_POLLS;
        if (held == s_lit) {
            continue;
        }
        s_lit = held;
        if (held) {
            ESP_LOGI(TAG, "LED: on (BOOT held)");
            mic_led_set(LED_BUTTON_LEVEL, LED_BUTTON_LEVEL, LED_BUTTON_LEVEL);
        } else {
            ESP_LOGI(TAG, "LED: off");
            mic_led_off();
        }
    }
}

void mic_led_start(void)
{
    // Park the LED first: after power-up it shows white until it receives a
    // black frame, and that is the state the device should never be seen in.
    mic_led_off();
    ESP_LOGI(TAG, "LED: parked off gpio=%d (hold BOOT to light)", CONFIG_MICCLICKER_LED_GPIO);

    enable_button_input();
    if (xTaskCreate(led_task, "mc_led", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "LED: FAIL reason=task; led stays in its current state");
    }
}

#endif
