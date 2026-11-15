#include "sdkconfig.h"

#if !CONFIG_MICCLICKER_STAGE1_SELFTEST && !CONFIG_MICCLICKER_ESPNOW_PROBE_RECEIVER

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mic_jitter.h"
#include "mic_link_audio.h"
#include "mic_link_keys.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "usb_descriptors.h"

#define MIC_CMD_PING 0x01
#define MIC_CMD_LEFT_ALT 0x02
#define MIC_CMD_ENTER 0x03
#define MIC_CMD_ALL_KEYS_UP 0x04

#define MIC_RESULT_OK 0
#define MIC_RESULT_INVALID 1
#define MIC_RESULT_BUSY 2
#define MIC_RESULT_QUEUE_FULL 3
#define MIC_RESULT_NOT_READY 4

#define MIC_KEY_HOLD_MS 50
#define MIC_AUDIO_FRAMES_PER_MS 16

static const char *TAG = "usb_prototype";
static QueueHandle_t s_command_queue;

static volatile uint8_t s_last_command;
static volatile uint8_t s_last_result;
static volatile bool s_config_error;
static volatile bool s_alt_pressed;
static volatile bool s_enter_pressed;
static volatile uint32_t s_ping_count;
static volatile uint32_t s_alt_count;
static volatile uint32_t s_enter_count;
static volatile uint32_t s_all_up_count;
static volatile uint32_t s_rejected_count;
static volatile bool s_audio_streaming;
static volatile uint32_t s_audio_produced_frames;
static volatile uint32_t s_audio_short_writes;
static volatile uint32_t s_audio_usb_packets;
static volatile uint32_t s_audio_usb_bytes;
static volatile uint32_t s_audio_empty_packets;
static volatile uint32_t s_audio_alt_transitions;
// Set once the ring has primed, so the startup wait happens once per stream rather
// than every time the buffer dips.
static volatile bool s_audio_primed;

static bool s_mute[2];
static int16_t s_volume_db256[2];
static int16_t s_gain_q15[2] = {32767, 32767};
static uint16_t s_effective_gain = 32767;
static uint32_t s_sample_frequency = 16000;
static uint8_t s_clock_valid = 1;
static audio20_control_range_2_n_t(1) s_volume_range[2];
static audio20_control_range_4_n_t(1) s_sample_frequency_range;

static void put_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void fill_status_report(uint8_t *buffer)
{
    memset(buffer, 0, MIC_CONFIG_REPORT_SIZE);
    buffer[0] = 1;
    buffer[1] = (tud_mounted() ? 0x01 : 0) |
                (s_audio_streaming ? 0x02 : 0) |
                (s_alt_pressed ? 0x04 : 0) |
                (s_enter_pressed ? 0x08 : 0) |
                (s_config_error ? 0x10 : 0);
    buffer[2] = s_last_command;
    buffer[3] = s_last_result;
    put_u32_le(&buffer[4], s_ping_count);
    put_u32_le(&buffer[8], s_alt_count);
    put_u32_le(&buffer[12], s_enter_count);
    put_u32_le(&buffer[16], s_all_up_count);
    put_u32_le(&buffer[20], s_rejected_count);
    put_u32_le(&buffer[24], s_audio_produced_frames);
    put_u32_le(&buffer[28], s_audio_short_writes);
    put_u32_le(&buffer[32], s_audio_usb_packets);
    put_u32_le(&buffer[36], s_audio_usb_bytes);
    put_u32_le(&buffer[40], s_audio_empty_packets);
    put_u32_le(&buffer[44], s_audio_alt_transitions);

    // Stage 4 link health. Bytes 0..47 keep the stage-2 layout so an existing tool
    // reading this report keeps working; these four words extend it.
    mic_link_audio_stats_t link;
    mic_link_audio_get_stats(&link);
    mic_jitter_t *ring = mic_link_audio_ring();
    mic_jitter_stats_t jitter;
    mic_jitter_get_stats(ring, &jitter);
    // rx | (gap << 16) so a single 32-bit word carries both counts under the 64-byte cap.
    put_u32_le(&buffer[48], link.audio_rx | (link.audio_gap << 16));
    put_u32_le(&buffer[52], link.audio_dup | (link.audio_late << 16));
    put_u32_le(&buffer[56], jitter.take_underruns | (jitter.put_overruns << 16));
    put_u32_le(&buffer[60], mic_jitter_fill_ms(ring));
}

static bool send_all_keys_up(void)
{
    // All keys up is not "send a zero report next to the key path"; it has to go through
    // the same state machine that owns the report, or the two writers race and the host
    // can end up holding a key the firmware thinks is released.
    //
    // The release is issued here and emitted by the key task, which retries until the host
    // polls. That is why this returns as soon as the request is queued rather than waiting
    // for the endpoint: the request itself cannot be lost.
    mic_link_keys_release_all("usb_request");
    s_alt_pressed = false;
    s_enter_pressed = false;
    ++s_all_up_count;
    return true;
}

static bool send_test_key(uint8_t command)
{
    uint8_t key_id = command == MIC_CMD_LEFT_ALT ? MIC_KEY_ID_UP : MIC_KEY_ID_OK;
    // Press then release through the mapping, so the test exercises the same table the
    // device actually uses rather than a hard-coded Alt/Enter pair. The hold is what makes
    // the pair observable to a host that samples at frame rate.
    mic_link_keys_self_test(key_id, MIC_KEY_EVENT_PRESS);
    vTaskDelay(pdMS_TO_TICKS(MIC_KEY_HOLD_MS));
    mic_link_keys_self_test(key_id, MIC_KEY_EVENT_RELEASE);
    if (command == MIC_CMD_LEFT_ALT) {
        s_alt_pressed = true;
        ++s_alt_count;
    } else {
        s_enter_pressed = true;
        ++s_enter_count;
    }
    return true;
}

static void command_task(void *argument)
{
    (void)argument;
    for (;;) {
        uint8_t command = 0;
        if (xQueueReceive(s_command_queue, &command, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }

        bool ok = true;
        switch (command) {
        case MIC_CMD_PING:
            ++s_ping_count;
            break;
        case MIC_CMD_LEFT_ALT:
        case MIC_CMD_ENTER:
            ok = send_test_key(command);
            break;
        case MIC_CMD_ALL_KEYS_UP:
            ok = send_all_keys_up();
            break;
        default:
            ++s_rejected_count;
            s_last_result = MIC_RESULT_INVALID;
            s_config_error = true;
            continue;
        }
        s_last_result = ok ? MIC_RESULT_OK : MIC_RESULT_NOT_READY;
        s_config_error = !ok;
    }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    if (instance == MIC_HID_KEYBOARD_INSTANCE) {
        return mic_keyboard_report_descriptor;
    }
    if (instance == MIC_HID_CONFIG_INSTANCE) {
        return mic_config_report_descriptor;
    }
    return NULL;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    if (instance != MIC_HID_CONFIG_INSTANCE || report_id != MIC_CONFIG_REPORT_ID ||
        report_type != HID_REPORT_TYPE_INPUT || reqlen < MIC_CONFIG_REPORT_SIZE) {
        return 0;
    }
    fill_status_report(buffer);
    return MIC_CONFIG_REPORT_SIZE;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    if (instance == MIC_HID_KEYBOARD_INSTANCE) {
        // Keyboard LED output is independent of the vendor configuration channel.
        return;
    }
    if (instance != MIC_HID_CONFIG_INSTANCE || report_id != MIC_CONFIG_REPORT_ID ||
        report_type != HID_REPORT_TYPE_OUTPUT || buffer == NULL || bufsize != MIC_CONFIG_REPORT_SIZE) {
        s_last_result = MIC_RESULT_INVALID;
        s_config_error = true;
        ++s_rejected_count;
        return;
    }

    s_last_command = buffer[0];
    if (buffer[0] != MIC_CMD_PING && buffer[0] != MIC_CMD_LEFT_ALT &&
        buffer[0] != MIC_CMD_ENTER && buffer[0] != MIC_CMD_ALL_KEYS_UP) {
        s_last_result = MIC_RESULT_INVALID;
        s_config_error = true;
        ++s_rejected_count;
        return;
    }
    if (xQueueSend(s_command_queue, &buffer[0], 0) != pdTRUE) {
        s_last_result = MIC_RESULT_QUEUE_FULL;
        s_config_error = true;
        ++s_rejected_count;
        return;
    }
    s_last_result = MIC_RESULT_BUSY;
    s_config_error = false;
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    (void)instance;
    (void)protocol;
}

bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle_rate)
{
    (void)instance;
    (void)idle_rate;
    return true;
}

static void usb_event_handler(tinyusb_event_t *event, void *argument)
{
    (void)argument;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        // A freshly attached host knows nothing about the keys this end may be holding,
        // and TinyUSB starts each attach with an empty report. Clearing the local state is
        // what keeps the two views equal.
        mic_link_keys_on_usb_reset();
        s_audio_streaming = false;
        tud_audio_clear_ep_in_ff();
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        mic_link_keys_on_usb_reset();
        s_audio_streaming = false;
    }
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    // A suspended host stops polling, so a held modifier would survive the sleep on the
    // host's side and reappear after resume. Release before that can happen.
    mic_link_keys_on_usb_reset();
    s_audio_streaming = false;
}

void tud_resume_cb(void)
{
    mic_link_keys_on_usb_reset();
}

static void update_effective_gain(void)
{
    uint32_t combined = 32767;
    for (size_t i = 0; i < 2; ++i) {
        float gain = powf(10.0f, s_volume_db256[i] / (256.0f * 20.0f));
        int32_t q15 = (int32_t)(gain * 32767.0f);
        if (q15 < 0) {
            q15 = 0;
        } else if (q15 > 32767) {
            q15 = 32767;
        }
        s_gain_q15[i] = (int16_t)q15;
        combined = (combined * (uint32_t)q15 + 16383) >> 15;
    }
    s_effective_gain = (uint16_t)combined;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buffer)
{
    (void)rhport;
    (void)request;
    (void)buffer;
    return false;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buffer)
{
    (void)rhport;
    (void)request;
    (void)buffer;
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request, uint8_t *buffer)
{
    (void)rhport;
    uint8_t channel = TU_U16_LOW(request->wValue);
    uint8_t selector = TU_U16_HIGH(request->wValue);
    uint8_t entity = TU_U16_HIGH(request->wIndex);

    if (entity != 2 || channel >= 2 || request->bRequest != AUDIO20_CS_REQ_CUR) {
        return false;
    }
    if (selector == AUDIO20_FU_CTRL_MUTE) {
        if (request->wLength != sizeof(audio20_control_cur_1_t)) {
            return false;
        }
        s_mute[channel] = ((audio20_control_cur_1_t *)buffer)->bCur != 0;
        return true;
    }
    if (selector == AUDIO20_FU_CTRL_VOLUME) {
        if (request->wLength != sizeof(audio20_control_cur_2_t)) {
            return false;
        }
        int16_t value = (int16_t)((audio20_control_cur_2_t *)buffer)->bCur;
        if (value < -60 * 256 || value > 0) {
            return false;
        }
        s_volume_db256[channel] = value;
        update_effective_gain();
        return true;
    }
    return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;
    return false;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    uint8_t channel = TU_U16_LOW(request->wValue);
    uint8_t selector = TU_U16_HIGH(request->wValue);
    uint8_t entity = TU_U16_HIGH(request->wIndex);

    if (entity == 1 && selector == AUDIO20_TE_CTRL_CONNECTOR) {
        audio20_desc_channel_cluster_t cluster = {
            .bNrChannels = 1,
            .bmChannelConfig = (audio20_channel_config_t)0,
            .iChannelNames = 0,
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &cluster, sizeof(cluster));
    }
    if (entity == 2 && channel < 2) {
        if (selector == AUDIO20_FU_CTRL_MUTE && request->bRequest == AUDIO20_CS_REQ_CUR) {
            return tud_control_xfer(rhport, request, &s_mute[channel], sizeof(s_mute[channel]));
        }
        if (selector == AUDIO20_FU_CTRL_VOLUME) {
            if (request->bRequest == AUDIO20_CS_REQ_CUR) {
                return tud_control_xfer(rhport, request, &s_volume_db256[channel], sizeof(s_volume_db256[channel]));
            }
            if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                                  &s_volume_range[channel],
                                                                  sizeof(s_volume_range[channel]));
            }
        }
        return false;
    }
    if (entity == 4) {
        if (selector == AUDIO20_CS_CTRL_SAM_FREQ) {
            if (request->bRequest == AUDIO20_CS_REQ_CUR) {
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                                  &s_sample_frequency,
                                                                  sizeof(s_sample_frequency));
            }
            if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
                return tud_audio_buffer_and_schedule_control_xfer(rhport, request,
                                                                  &s_sample_frequency_range,
                                                                  sizeof(s_sample_frequency_range));
            }
        } else if (selector == AUDIO20_CS_CTRL_CLK_VALID && request->bRequest == AUDIO20_CS_REQ_CUR) {
            return tud_control_xfer(rhport, request, &s_clock_valid, sizeof(s_clock_valid));
        }
    }
    return false;
}

bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id,
                           uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)func_id;
    (void)ep_in;
    (void)cur_alt_setting;
    // ISR context: only update lightweight observations, no logging or allocation.
    ++s_audio_usb_packets;
    s_audio_usb_bytes += n_bytes_sent;
    if (n_bytes_sent == 0) {
        ++s_audio_empty_packets;
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;
    s_audio_streaming = false;
    ++s_audio_alt_transitions;
    s_audio_primed = false;
    mic_link_audio_set_consuming(false);
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    uint8_t interface = TU_U16_LOW(request->wIndex);
    uint8_t alternate = TU_U16_LOW(request->wValue);
    if (interface != 1 || alternate > 1) {
        return false;
    }
    bool streaming = (alternate == 1);
    if (s_audio_streaming != streaming) {
        s_audio_streaming = streaming;
        ++s_audio_alt_transitions;
        // The link receiver must know whether anyone is draining the endpoint: feeding
        // the ring while the host is idle only produces overflow.
        mic_link_audio_set_consuming(streaming);
        // A new stream starts from a flushed ring, so it must prime again.
        s_audio_primed = false;
    }
    return true;
}

// Drain the link ring into the UAC endpoint, one millisecond at a time.
//
// The endpoint expects exactly one packet per millisecond, so this task is paced by the
// USB frame clock rather than by data availability. When the ring is short it still
// hands back a full millisecond: the ring conceals the gap, which is what keeps the
// stream continuous across a lost frame or a clock mismatch instead of tearing.
//
// The mute and volume controls from the stage-2 prototype are still applied here, so
// the host side behaves the same way it did with the sine wave.
static void audio_task(void *argument)
{
    (void)argument;
    mic_jitter_t *ring = mic_link_audio_ring();
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1));
        if (!s_audio_streaming) {
            continue;
        }

        int16_t frame[MIC_AUDIO_FRAMES_PER_MS];
        bool muted = s_mute[0] || s_mute[1];
        if (muted) {
            memset(frame, 0, sizeof(frame));
        } else {
            // Wait for the ring to prime before the first packet. Starting to drain an
            // empty ring turns the startup transient into a burst of concealed
            // milliseconds; waiting one target worth of audio turns it into a single
            // ~60 ms delay at the start of the stream, which no listener notices.
            if (!s_audio_primed) {
                if (mic_jitter_fill_ms(ring) < MIC_JITTER_TARGET_MS) {
                    memset(frame, 0, sizeof(frame));
                    uint16_t w = tud_audio_write(frame, sizeof(frame));
                    if (w == sizeof(frame)) {
                        ++s_audio_produced_frames;
                    } else {
                        ++s_audio_short_writes;
                    }
                    continue;
                }
                s_audio_primed = true;
            }

            // A one-millisecond take never short-reads: an underrun is concealed by the
            // ring so the endpoint always sees a full packet.
            mic_jitter_take(ring, frame, MIC_AUDIO_FRAMES_PER_MS, esp_timer_get_time());

            uint32_t gain = s_effective_gain;
            if (gain != 32767) {
                for (size_t i = 0; i < MIC_AUDIO_FRAMES_PER_MS; ++i) {
                    int32_t scaled = ((int32_t)frame[i] * (int32_t)gain + 16383) >> 15;
                    if (scaled > INT16_MAX) {
                        scaled = INT16_MAX;
                    } else if (scaled < INT16_MIN) {
                        scaled = INT16_MIN;
                    }
                    frame[i] = (int16_t)scaled;
                }
            }
        }

        uint16_t written = tud_audio_write(frame, sizeof(frame));
        if (written == sizeof(frame)) {
            ++s_audio_produced_frames;
        } else {
            ++s_audio_short_writes;
        }
    }
}

void micclicker_usb_prototype_start(void)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(mic_serial_descriptor, sizeof(mic_serial_descriptor), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    for (size_t i = 0; i < 2; ++i) {
        s_volume_range[i].wNumSubRanges = 1;
        s_volume_range[i].subrange[0].bMin = -60 * 256;
        s_volume_range[i].subrange[0].bMax = 0;
        s_volume_range[i].subrange[0].bRes = 256;
    }
    s_sample_frequency_range.wNumSubRanges = 1;
    s_sample_frequency_range.subrange[0].bMin = 16000;
    s_sample_frequency_range.subrange[0].bMax = 16000;
    s_sample_frequency_range.subrange[0].bRes = 0;
    update_effective_gain();

    s_command_queue = xQueueCreate(8, sizeof(uint8_t));
    if (s_command_queue == NULL) {
        ESP_LOGE(TAG, "USB_PROTOTYPE: FAIL reason=command_queue");
        return;
    }

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_event_handler);
    config.descriptor.device = &mic_device_descriptor;
    config.descriptor.full_speed_config = mic_configuration_descriptor;
    config.descriptor.string = mic_string_descriptor;
    config.descriptor.string_count = MIC_STRING_DESCRIPTOR_COUNT;
    esp_err_t err = tinyusb_driver_install(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB_PROTOTYPE: FAIL reason=driver_install error=%s", esp_err_to_name(err));
        return;
    }

    // Bring the link receiver up after USB installs but before the audio task runs, so
    // the first UAC read already has a valid ring rather than a NULL dereference; the key
    // path comes next so a key event that arrives with the first frames has a task to
    // land on.
    micclicker_s3_audio_start();
    esp_err_t keys_err = micclicker_s3_keys_start();
    if (keys_err != ESP_OK) {
        // Not fatal: audio and the config channel keep working, and the log line is what
        // says the keyboard is dead.
        ESP_LOGE(TAG, "USB_PROTOTYPE: KEYS_FAIL err=%s", esp_err_to_name(keys_err));
    }

    TaskHandle_t command_handle = NULL;
    TaskHandle_t audio_handle = NULL;
    if (xTaskCreate(command_task, "mc_config", 4096, NULL, 4, &command_handle) != pdPASS) {
        ESP_LOGE(TAG, "USB_PROTOTYPE: FAIL reason=command_task");
        return;
    }
    if (xTaskCreate(audio_task, "mc_audio", 4096, NULL, 5, &audio_handle) != pdPASS) {
        ESP_LOGE(TAG, "USB_PROTOTYPE: FAIL reason=audio_task");
        vTaskDelete(command_handle);
        return;
    }

    ESP_LOGI(TAG,
             "USB_PROTOTYPE: READY audio=16000Hz/16bit/mono source=link hid=keyboard+config "
             "keys=link");
}

#endif // !CONFIG_MICCLICKER_STAGE1_SELFTEST
