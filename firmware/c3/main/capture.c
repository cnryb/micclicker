// ES8311 + I2S capture on the AI Passport board.
//
// Hardware facts are pinned in board.h (I2S MCLK=6/BCLK=5/WS=3/DOUT=2/DIN=4, ES8311 at
// I2C 0x18). The board exposes I2S as full duplex with a shared MCLK/BCLK/WS, and the
// previous project on this same board established the four details this driver has to
// respect:
//
//  1. esp_codec_dev_open() disables the channel, and disable requires a RUNNING channel.
//     Enabling once before open avoids a spurious "channel has not been enabled yet".
//  2. REG01..REG06 hold the clock dividers the driver computes from MCLK and the sample
//     rate. Overwriting them after open breaks ADC timing; only the analog gain is
//     touched here.
//  3. Codec mode is ADC-only. BOTH would power the DAC output stage, which this board
//     has no use for (the speaker amplifier is not wired to the MCU).
//  4. no_dac_ref is required for the mono ADC path.
//
// The capture format is the design baseline: 16 kHz, 16-bit signed little-endian, mono.
// The codec is configured for a single channel mask, so the I2S DMA still carries
// stereo slots and this layer keeps only slot 0. That is a property of the hardware
// path, not a design choice.

#include "mic_capture.h"

#include <string.h>

#include "board.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "es8311_codec.h"

#define MIC_CAPTURE_I2C_PORT   0
#define MIC_CAPTURE_I2S_PORT   I2S_NUM_0
// The codec is opened with channel = 1 and esp_codec_dev reconfigures I2S to match, so
// the DMA hands back single-channel samples: one int16 per sample, no slot to discard.
// Reading stereo and keeping slot 0 here would consume two frames' worth of audio per
// call and halve the stream rate, which is what the first version of this file did.
#define MIC_CAPTURE_I2S_SLOTS  1

static const char *TAG = "mic_capture";

static i2c_master_bus_handle_t s_bus;
static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_gpio_if_t *s_gpio;
const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_dev;
static bool s_ready;
static int s_gain_db = MIC_CAPTURE_DEFAULT_GAIN_DB;

static esp_err_t i2c_bus_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;
    }
    i2c_master_bus_config_t config = {
        .i2c_port = MIC_CAPTURE_I2C_PORT,
        .sda_io_num = MIC_BOARD_I2C_SDA_GPIO,
        .scl_io_num = MIC_BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&config, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=i2c err=%s sda=%d scl=%d", esp_err_to_name(err),
                 MIC_BOARD_I2C_SDA_GPIO, MIC_BOARD_I2C_SCL_GPIO);
        return err;
    }
    return ESP_OK;
}

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan = {
        .id = MIC_CAPTURE_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        // 240 frames per descriptor at 16 kHz is 15 ms of latency per descriptor, so a
        // six-descriptor ring gives ~90 ms of slack before the DMA overruns. The
        // capture task drains far faster than that.
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t err = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=i2s_chan err=%s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = MIC_CAPTURE_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            // The bus is clocked in stereo because MCLK/BCLK/WS are shared with the
            // codec interface; only slot 0 carries the microphone.
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = MIC_BOARD_I2S_MCLK_GPIO,
            .bclk = MIC_BOARD_I2S_BCLK_GPIO,
            .ws = MIC_BOARD_I2S_WS_GPIO,
            .dout = MIC_BOARD_I2S_DOUT_GPIO,
            .din = MIC_BOARD_I2S_DIN_GPIO,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };

    if ((err = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=i2s_tx err=%s", esp_err_to_name(err));
        return err;
    }
    if ((err = i2s_channel_init_std_mode(s_rx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=i2s_rx err=%s", esp_err_to_name(err));
        return err;
    }

    // esp_codec_dev_open() disables the channel before reconfiguring it, and disable
    // requires RUNNING. Enable once now so that call is legal; the codec is not
    // configured yet, so nothing audible happens.
    err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=i2s_enable err=%s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t codec_init(void)
{
    s_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = MIC_CAPTURE_I2C_PORT,
        // This interface wants the 8-bit form of the address.
        .addr = MIC_BOARD_ES8311_ADDR << 1,
        .bus_handle = s_bus,
    });
    if (s_ctrl == NULL) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=codec_ctrl hint=check_i2c_address_0x%02x",
                 MIC_BOARD_ES8311_ADDR);
        return ESP_FAIL;
    }

    s_data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = MIC_CAPTURE_I2S_PORT,
        .tx_handle = s_tx,
        .rx_handle = s_rx,
    });
    if (s_data == NULL) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=codec_data");
        return ESP_ERR_NO_MEM;
    }

    s_gpio = audio_codec_new_gpio();
    if (s_gpio == NULL) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=codec_gpio");
        return ESP_ERR_NO_MEM;
    }

    s_codec_if = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if = s_ctrl,
        .gpio_if = s_gpio,
        // ADC only: BOTH would also power the DAC output stage, which this board cannot
        // use (the amplifier enable is not wired to the MCU).
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = MIC_BOARD_I2S_PA_CTRL_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
        // Required by the mono ADC path.
        .no_dac_ref = true,
    });
    if (s_codec_if == NULL) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=codec_new");
        return ESP_FAIL;
    }

    s_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        // Input only: declaring IN_OUT would require the driver to open the output side
        // as well, which this board does not need.
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data,
    });
    if (s_dev == NULL) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=codec_dev");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t codec_open(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = MIC_CAPTURE_SAMPLE_RATE,
        // 0 lets the driver use its default 256xfs MCLK.
        .mclk_multiple = 0,
    };
    int ret = esp_codec_dev_open(s_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=codec_open err=%d", ret);
        return ESP_FAIL;
    }

    // Only the analog gain is set here. The clock divider registers belong to the
    // driver; overwriting them after open corrupts ADC timing.
    ret = esp_codec_dev_set_in_gain(s_dev, (float)s_gain_db);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=codec_gain err=%d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mic_capture_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_init();
    if (err != ESP_OK) {
        return err;
    }
    err = codec_init();
    if (err != ESP_OK) {
        return err;
    }
    err = codec_open();
    if (err != ESP_OK) {
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "CAPTURE: READY rate=%d bits=16 channels=1 gain_db=%d", MIC_CAPTURE_SAMPLE_RATE,
             s_gain_db);
    return ESP_OK;
}

bool mic_capture_ready(void)
{
    return s_ready;
}

esp_err_t mic_capture_read(int16_t *pcm, size_t samples)
{
    if (!s_ready || s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // A single blocking read of the whole request. The DMA paces this call: it returns
    // once `samples` mono samples have actually been captured, which is one frame period
    // for a full G.722 frame. That blocking property is what clocks the frame grid, so
    // splitting the read into chunks here would only add syscalls.
    size_t remaining = samples;
    int16_t *out = pcm;
    while (remaining > 0) {
        size_t chunk = remaining < MIC_CAPTURE_CHUNK_SAMPLES * 8 ? remaining
                                                                : MIC_CAPTURE_CHUNK_SAMPLES * 8;
        int bytes = (int)(chunk * sizeof(int16_t) * MIC_CAPTURE_I2S_SLOTS);
        int ret = esp_codec_dev_read(s_dev, out, bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "CAPTURE: read failed err=%d", ret);
            return ESP_FAIL;
        }
        out += chunk;
        remaining -= chunk;
    }
    return ESP_OK;
}

esp_err_t mic_capture_set_gain(int gain_db)
{
    if (gain_db < 0 || gain_db > 42) {
        return ESP_ERR_INVALID_ARG;
    }
    s_gain_db = gain_db;
    if (!s_ready) {
        return ESP_OK;  // applied by the next init
    }
    int ret = esp_codec_dev_set_in_gain(s_dev, (float)gain_db);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "CAPTURE: FAIL stage=gain err=%d", ret);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CAPTURE: gain_db=%d", gain_db);
    return ESP_OK;
}

int mic_capture_gain(void)
{
    return s_gain_db;
}
