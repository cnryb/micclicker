// Host-side harness for the MicClicker ESP-NOW framing layer.
//
// Runs on a desktop compiler against the very same sources the firmware builds, so
// the wire format, the length and version checks, and the sequence accounting can be
// graded without flashing a board. It complements the on-device probe: this proves
// the codec, the probe proves the radio.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mic_link.h"

static int g_failures;
static int g_checks;

static void ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ok(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("   PASS ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    ++g_checks;
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("   FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    ++g_failures;
}

static void check(bool condition, const char *what)
{
    if (condition) {
        ok("%s", what);
    } else {
        fail("%s", what);
    }
}

static void check_err(esp_err_t actual, esp_err_t expected, const char *what)
{
    if (actual == expected) {
        ok("%s", what);
    } else {
        fail("%s (expected 0x%x, got 0x%x)", what, expected, actual);
    }
}

// ---- wire layout -----------------------------------------------------------

static void test_audio_frame_layout(void)
{
    printf("== audio frame ==\n");

    uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }

    mic_link_frame_t frame;
    check_err(mic_link_build_audio(&frame, 0x1234, 0xDEADBEEF, payload), ESP_OK, "build audio");
    check(frame.len == MIC_LINK_AUDIO_FRAME_BYTES, "audio frame is 168 bytes");
    check(MIC_LINK_AUDIO_FRAME_BYTES == 168, "audio frame constant is 168");
    check(frame.len <= MIC_LINK_MAX_FRAME_BYTES, "audio frame fits the 250-byte ESP-NOW limit");

    // Byte-exact header: little-endian session and seq, version and type first.
    check(frame.bytes[0] == 1, "version byte is 1");
    check(frame.bytes[1] == 0x01, "type byte is 0x01 (audio)");
    check(frame.bytes[2] == 0x34 && frame.bytes[3] == 0x12, "session is little-endian");
    check(frame.bytes[4] == 0xEF && frame.bytes[5] == 0xBE && frame.bytes[6] == 0xAD &&
              frame.bytes[7] == 0xDE,
          "seq is little-endian");
    check(memcmp(&frame.bytes[MIC_LINK_HEADER_BYTES], payload, sizeof(payload)) == 0,
          "payload copied verbatim after the header");

    mic_link_view_t view;
    check_err(mic_link_parse(frame.bytes, frame.len, &view), ESP_OK, "parse audio");
    check(view.header.type == MIC_LINK_TYPE_AUDIO, "parsed type is audio");
    check(view.header.session == 0x1234, "parsed session");
    check(view.header.seq == 0xDEADBEEF, "parsed seq");

    const uint8_t *decoded = NULL;
    check_err(mic_link_decode_audio(&view, &decoded), ESP_OK, "decode audio payload");
    check(decoded == &frame.bytes[MIC_LINK_HEADER_BYTES], "audio payload points into the frame");
}

static void test_key_frames(void)
{
    printf("== key and ack frames ==\n");

    mic_link_frame_t frame;
    mic_link_key_t key = {.key_id = MIC_LINK_KEY_UP, .event = MIC_LINK_KEY_EVENT_PRESS};
    check_err(mic_link_build_key(&frame, 7, 11, &key), ESP_OK, "build key");
    check(frame.len == MIC_LINK_KEY_FRAME_BYTES, "key frame is 10 bytes");
    check(frame.bytes[8] == MIC_LINK_KEY_UP && frame.bytes[9] == MIC_LINK_KEY_EVENT_PRESS,
          "key id and event in payload");

    mic_link_view_t view;
    mic_link_key_t decoded;
    check_err(mic_link_parse(frame.bytes, frame.len, &view), ESP_OK, "parse key");
    check_err(mic_link_decode_key(&view, &decoded), ESP_OK, "decode key");
    check(decoded.key_id == MIC_LINK_KEY_UP && decoded.event == MIC_LINK_KEY_EVENT_PRESS,
          "key round-trips");

    check_err(mic_link_build_key_ack(&frame, 7, 12, 0xCAFEF00D), ESP_OK, "build key ack");
    check(frame.len == MIC_LINK_KEY_ACK_FRAME_BYTES, "key ack frame is 12 bytes");
    uint32_t acked = 0;
    check_err(mic_link_parse(frame.bytes, frame.len, &view), ESP_OK, "parse key ack");
    check_err(mic_link_decode_key_ack(&view, &acked), ESP_OK, "decode key ack");
    check(acked == 0xCAFEF00D, "ack echoes the confirmed key seq unchanged");

    // A retransmitted key event keeps its seq; the ack payload therefore also keeps
    // matching. This is asserted in the retransmission test below.
}

static void test_heartbeat_frames(void)
{
    printf("== heartbeat frames ==\n");

    mic_link_frame_t frame;
    mic_link_heartbeat_t beat = {.phase = 3, .mode = MIC_LINK_HEARTBEAT_MODE_PROBE, .flags = 0xBEEF};
    check_err(mic_link_build_heartbeat(&frame, 0xFFFF, 0xFFFFFFFF, &beat), ESP_OK, "build heartbeat");
    check(frame.len == MIC_LINK_HEARTBEAT_FRAME_BYTES, "heartbeat frame is 12 bytes");

    mic_link_view_t view;
    mic_link_heartbeat_t decoded;
    check_err(mic_link_parse(frame.bytes, frame.len, &view), ESP_OK, "parse heartbeat");
    check_err(mic_link_decode_heartbeat(&view, &decoded), ESP_OK, "decode heartbeat");
    check(decoded.phase == 3 && decoded.mode == MIC_LINK_HEARTBEAT_MODE_PROBE && decoded.flags == 0xBEEF,
          "heartbeat round-trips at maximum session and seq");
    check(view.header.session == 0xFFFF && view.header.seq == 0xFFFFFFFF,
          "maximum session and seq survive");

    mic_link_heartbeat_ack_t ack = {
        .echo_seq = 0x0BADC0DE,
        .phase = 4,
        .state = MIC_LINK_STATE_PAIRED,
        .flags = 0x0102,
        .audio_rx = 1000000,
        .audio_dup = 17,
        .audio_gap = 5,
        .audio_late = 3,
        .key_rx = 240,
        .key_dup = 120,
        .rejected = 9,
    };
    check_err(mic_link_build_heartbeat_ack(&frame, 0xABCD, 88, &ack), ESP_OK, "build heartbeat ack");
    check(frame.len == MIC_LINK_HEARTBEAT_ACK_FRAME_BYTES, "heartbeat ack frame is 44 bytes");

    mic_link_heartbeat_ack_t out;
    check_err(mic_link_parse(frame.bytes, frame.len, &view), ESP_OK, "parse heartbeat ack");
    check_err(mic_link_decode_heartbeat_ack(&view, &out), ESP_OK, "decode heartbeat ack");
    check(out.echo_seq == 0x0BADC0DE, "heartbeat ack echoes the heartbeat seq for RTT");
    check(out.phase == 4 && out.state == MIC_LINK_STATE_PAIRED && out.flags == 0x0102,
          "heartbeat ack header fields round-trip");
    check(out.audio_rx == 1000000 && out.audio_dup == 17 && out.audio_gap == 5 && out.audio_late == 3,
          "heartbeat ack audio counters round-trip");
    check(out.key_rx == 240 && out.key_dup == 120 && out.rejected == 9,
          "heartbeat ack key and rejection counters round-trip");
}

static void test_pair_frames(void)
{
    printf("== pairing frames ==\n");

    mic_link_frame_t frame;
    mic_link_pair_req_t request;
    for (size_t i = 0; i < MIC_LINK_PAIR_KEY_BYTES; ++i) {
        request.lmk[i] = (uint8_t)(0x10 + i);
        request.pmk[i] = (uint8_t)(0xA0 + i);
    }
    request.channel = 6;

    check_err(mic_link_build_pair_req(&frame, &request), ESP_OK, "build pair request");
    check(frame.len == MIC_LINK_PAIR_REQ_FRAME_BYTES, "pair request frame is 41 bytes");
    check(MIC_LINK_PAIR_REQ_FRAME_BYTES == 41, "pair request constant is 41");
    check(frame.bytes[2] == 0 && frame.bytes[3] == 0 && frame.bytes[4] == 0 && frame.bytes[7] == 0,
          "pair request uses session 0 and seq 0");

    mic_link_view_t view;
    mic_link_pair_req_t decoded;
    check_err(mic_link_parse(frame.bytes, frame.len, &view), ESP_OK, "parse pair request");
    check_err(mic_link_decode_pair_req(&view, &decoded), ESP_OK, "decode pair request");
    check(memcmp(decoded.lmk, request.lmk, MIC_LINK_PAIR_KEY_BYTES) == 0, "LMK round-trips");
    check(memcmp(decoded.pmk, request.pmk, MIC_LINK_PAIR_KEY_BYTES) == 0, "PMK round-trips");
    check(decoded.channel == 6, "channel round-trips");

    // Two pairings must be able to produce different keys; the builder copies whatever
    // it is given rather than substituting a built-in constant.
    mic_link_pair_req_t other = request;
    other.lmk[0] ^= 0xFF;
    mic_link_frame_t other_frame;
    mic_link_build_pair_req(&other_frame, &other);
    check(memcmp(other_frame.bytes, frame.bytes, frame.len) != 0, "different keys give different bytes");

    mic_link_pair_resp_t response = {.result = MIC_LINK_PAIR_RESULT_OK,
                                     .state = MIC_LINK_STATE_PAIRED,
                                     .reserved = 0};
    check_err(mic_link_build_pair_resp(&frame, &response), ESP_OK, "build pair response");
    check(frame.len == MIC_LINK_PAIR_RESP_FRAME_BYTES, "pair response frame is 12 bytes");
    mic_link_pair_resp_t resp_out;
    check_err(mic_link_parse(frame.bytes, frame.len, &view), ESP_OK, "parse pair response");
    check_err(mic_link_decode_pair_resp(&view, &resp_out), ESP_OK, "decode pair response");
    check(resp_out.result == MIC_LINK_PAIR_RESULT_OK && resp_out.state == MIC_LINK_STATE_PAIRED,
          "pair response round-trips");

    check_err(mic_link_build_pair_req(NULL, &request), ESP_ERR_INVALID_ARG, "null frame rejected");
    check_err(mic_link_build_pair_req(&frame, NULL), ESP_ERR_INVALID_ARG, "null request rejected");
    check_err(mic_link_build_pair_resp(&frame, NULL), ESP_ERR_INVALID_ARG, "null response rejected");

    // A pairing frame with one byte of padding must not pass the exact-length check.
    mic_link_build_pair_req(&frame, &request);
    uint8_t scratch[MIC_LINK_MAX_FRAME_BYTES];
    memcpy(scratch, frame.bytes, frame.len);
    memset(&scratch[frame.len], 0, 1);
    check_err(mic_link_parse(scratch, frame.len + 1, &view), ESP_ERR_INVALID_SIZE,
              "padded pair request rejected");
}

// ---- rejection -------------------------------------------------------------

static void test_rejections(void)
{
    printf("== rejection ==\n");

    mic_link_frame_t frame;
    uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES] = {0};
    check_err(mic_link_build_audio(&frame, 1, 1, payload), ESP_OK, "build reference frame");

    mic_link_view_t view;

    check_err(mic_link_parse(NULL, frame.len, &view), ESP_ERR_INVALID_ARG, "null data rejected");
    check_err(mic_link_parse(frame.bytes, frame.len, NULL), ESP_ERR_INVALID_ARG, "null view rejected");
    check_err(mic_link_parse(frame.bytes, MIC_LINK_MAX_FRAME_BYTES + 1, &view), ESP_ERR_INVALID_ARG,
              "oversize length rejected");
    check_err(mic_link_parse(frame.bytes, 7, &view), ESP_ERR_INVALID_SIZE, "short header rejected");

    // Version mismatch: a receiver must drop rather than reinterpret.
    uint8_t scratch[MIC_LINK_MAX_FRAME_BYTES];
    memcpy(scratch, frame.bytes, frame.len);
    scratch[0] = MIC_LINK_VERSION + 1;
    check_err(mic_link_parse(scratch, frame.len, &view), ESP_ERR_INVALID_VERSION,
              "unknown version rejected");

    // Unknown type.
    memcpy(scratch, frame.bytes, frame.len);
    scratch[1] = 0x7F;
    check_err(mic_link_parse(scratch, frame.len, &view), ESP_ERR_NOT_SUPPORTED, "unknown type rejected");

    // Truncated payload.
    check_err(mic_link_parse(frame.bytes, frame.len - 1, &view), ESP_ERR_INVALID_SIZE,
              "truncated audio frame rejected");

    // Trailing bytes must not pass as a valid frame.
    memcpy(scratch, frame.bytes, frame.len);
    memset(&scratch[frame.len], 0xAA, 1);
    check_err(mic_link_parse(scratch, frame.len + 1, &view), ESP_ERR_INVALID_SIZE,
              "audio frame with trailing byte rejected");

    // A key frame pretending to be the size of an audio frame is still rejected,
    // because the length must match the type, not merely fall inside the limit.
    mic_link_key_t key = {.key_id = MIC_LINK_KEY_OK, .event = MIC_LINK_KEY_EVENT_RELEASE};
    mic_link_frame_t key_frame;
    mic_link_build_key(&key_frame, 1, 1, &key);
    check_err(mic_link_parse(key_frame.bytes, MIC_LINK_AUDIO_FRAME_BYTES, &view), ESP_ERR_INVALID_SIZE,
              "key frame at audio length rejected");

    // Typed decoders refuse a view of the wrong type.
    mic_link_build_audio(&frame, 1, 1, payload);
    mic_link_parse(frame.bytes, frame.len, &view);
    mic_link_key_t wrong;
    check_err(mic_link_decode_key(&view, &wrong), ESP_ERR_INVALID_ARG, "key decoder rejects audio view");
    check_err(mic_link_decode_audio(&view, NULL), ESP_ERR_INVALID_ARG, "audio decoder rejects null out");

    mic_link_parse(key_frame.bytes, key_frame.len, &view);
    const uint8_t *audio_ptr = NULL;
    check_err(mic_link_decode_audio(&view, &audio_ptr), ESP_ERR_INVALID_ARG,
              "audio decoder rejects key view");

    // Builder argument checks.
    check_err(mic_link_build_audio(NULL, 0, 0, payload), ESP_ERR_INVALID_ARG, "null frame rejected");
    check_err(mic_link_build_audio(&frame, 0, 0, NULL), ESP_ERR_INVALID_ARG, "null audio payload rejected");
    check_err(mic_link_build_key(&frame, 0, 0, NULL), ESP_ERR_INVALID_ARG, "null key rejected");
    check_err(mic_link_build_heartbeat(&frame, 0, 0, NULL), ESP_ERR_INVALID_ARG, "null heartbeat rejected");
    check_err(mic_link_build_heartbeat_ack(&frame, 0, 0, NULL), ESP_ERR_INVALID_ARG,
              "null heartbeat ack rejected");
}

static void test_type_table(void)
{
    printf("== type table ==\n");

    check(mic_link_frame_bytes(MIC_LINK_TYPE_AUDIO) == 168, "audio length is 168");
    check(mic_link_frame_bytes(MIC_LINK_TYPE_KEY) == 10, "key length is 10");
    check(mic_link_frame_bytes(MIC_LINK_TYPE_KEY_ACK) == 12, "key ack length is 12");
    check(mic_link_frame_bytes(MIC_LINK_TYPE_HEARTBEAT) == 12, "heartbeat length is 12");
    check(mic_link_frame_bytes(MIC_LINK_TYPE_HEARTBEAT_ACK) == 44, "heartbeat ack length is 44");
    check(mic_link_frame_bytes(MIC_LINK_TYPE_PAIR_REQ) == 41, "pair request length is 41");
    check(mic_link_frame_bytes(MIC_LINK_TYPE_PAIR_RESP) == 12, "pair response length is 12");
    check(mic_link_frame_bytes(0x00) == 0, "type 0x00 has no length");
    check(mic_link_frame_bytes(0xFF) == 0, "type 0xFF has no length");
    check(strcmp(mic_link_type_name(MIC_LINK_TYPE_KEY_ACK), "key_ack") == 0, "type name for key ack");
    check(strcmp(mic_link_type_name(0x7F), "unknown") == 0, "unassigned type name");
}

// ---- sequence accounting ---------------------------------------------------

static void test_seq_in_order(void)
{
    printf("== seq: in order ==\n");

    mic_link_seq_t tracker;
    mic_link_seq_reset(&tracker);
    for (uint32_t seq = 0; seq < 1000; ++seq) {
        mic_link_seq_observe(&tracker, seq);
    }
    check(tracker.received == 1000, "1000 frames observed");
    check(tracker.holes == 0 && tracker.late == 0 && tracker.duplicates == 0,
          "no holes, late frames or duplicates");
    check(mic_link_seq_missing(&tracker) == 0, "no missing frames");
    check(tracker.next == 1000, "next expected seq is 1000");
}

static void test_seq_gap_and_late(void)
{
    printf("== seq: gap then late fill ==\n");

    mic_link_seq_t tracker;
    mic_link_seq_reset(&tracker);
    for (uint32_t seq = 0; seq <= 10; ++seq) {
        mic_link_seq_observe(&tracker, seq);
    }
    // Skip 11..13, then continue. Three frames are inferred missing.
    for (uint32_t seq = 14; seq <= 20; ++seq) {
        mic_link_seq_observe(&tracker, seq);
    }
    check(tracker.holes == 3, "three holes inferred");
    check(tracker.largest_hole == 3, "largest hole is 3");
    check(mic_link_seq_missing(&tracker) == 3, "three frames still missing");

    // Now the three late frames arrive. Effective loss drops to zero, but the hole
    // count stays, which is why both are reported separately.
    mic_link_seq_observe(&tracker, 12);
    mic_link_seq_observe(&tracker, 11);
    mic_link_seq_observe(&tracker, 13);
    check(tracker.late == 3, "three late arrivals counted");
    check(tracker.holes == 3, "hole count unchanged by late arrivals");
    check(mic_link_seq_missing(&tracker) == 0, "late arrivals clear the effective loss");

    // A repeated frame inside the window is a duplicate, not a late frame.
    mic_link_seq_observe(&tracker, 20);
    check(tracker.duplicates == 1, "repeat of an immediate predecessor is a duplicate");
    check(tracker.late == 3, "duplicate did not inflate the late count");

    // A retransmitted key event is recognisable before it is applied.
    check(mic_link_seq_is_duplicate(&tracker, 20), "duplicate probe sees the just-seen seq");
    check(!mic_link_seq_is_duplicate(&tracker, 21), "duplicate probe ignores the next seq");
    check(mic_link_seq_is_duplicate(&tracker, 13), "a late arrival is a duplicate once recorded");

    mic_link_seq_t empty;
    mic_link_seq_reset(&empty);
    check(!mic_link_seq_is_duplicate(&empty, 5), "unprimed tracker reports no duplicate");
}

static void test_seq_duplicates(void)
{
    printf("== seq: duplicates ==\n");

    mic_link_seq_t tracker;
    mic_link_seq_reset(&tracker);
    for (uint32_t seq = 0; seq < 10; ++seq) {
        mic_link_seq_observe(&tracker, seq);
    }
    // Retransmit the last four frames twice, as a key retry loop would.
    for (int round = 0; round < 2; ++round) {
        for (uint32_t seq = 6; seq < 10; ++seq) {
            mic_link_seq_observe(&tracker, seq);
        }
    }
    check(tracker.duplicates == 8, "eight duplicate observations");
    check(tracker.holes == 0 && tracker.late == 0, "duplicates create no holes or late frames");
    check(tracker.received == 18, "received counts duplicates");
}

static void test_seq_wraparound(void)
{
    printf("== seq: wraparound ==\n");

    mic_link_seq_t tracker;
    mic_link_seq_reset(&tracker);
    uint32_t start = UINT32_MAX - 5;
    for (uint32_t i = 0; i < 12; ++i) {
        mic_link_seq_observe(&tracker, start + i);
    }
    check(tracker.holes == 0, "wraparound is not a hole");
    check(mic_link_seq_missing(&tracker) == 0, "wraparound loses nothing");
    check(tracker.next == 6, "next seq wraps to 6");

    // A gap across the wrap boundary is still counted correctly.
    mic_link_seq_observe(&tracker, 9);
    check(tracker.holes == 3, "gap spanning the wrap counted as 3");
    check(tracker.next == 10, "next seq advances past the wrap");
}

static void test_seq_large_jump(void)
{
    printf("== seq: large jump ==\n");

    mic_link_seq_t tracker;
    mic_link_seq_reset(&tracker);
    mic_link_seq_observe(&tracker, 100);
    mic_link_seq_observe(&tracker, 100 + 5000);
    check(tracker.holes == 4999, "a 5000-frame jump records 4999 holes");
    check(tracker.largest_hole == 4999, "largest hole reflects the jump");
    check(tracker.next == 5101, "next seq is after the jump");

    // Frames from before the jump are now outside the window and cannot be graded;
    // they must not be miscounted as late (which would hide real loss).
    uint32_t late_before = tracker.late;
    mic_link_seq_observe(&tracker, 200);
    check(tracker.late == late_before, "far-behind frame is not counted as late");
    check(tracker.resyncs == 1, "far-behind frame recorded as a resync");
    check(mic_link_seq_missing(&tracker) == 4999, "loss stays visible at 4999");
}

static void test_seq_duplicate_near_wrap(void)
{
    printf("== seq: duplicate detection near wrap ==\n");

    mic_link_seq_t tracker;
    mic_link_seq_reset(&tracker);
    mic_link_seq_observe(&tracker, UINT32_MAX - 1);
    mic_link_seq_observe(&tracker, UINT32_MAX);
    mic_link_seq_observe(&tracker, 0);
    check(mic_link_seq_is_duplicate(&tracker, UINT32_MAX), "seq before the wrap is a duplicate");
    check(mic_link_seq_is_duplicate(&tracker, UINT32_MAX - 1), "two before the wrap is a duplicate");
    check(!mic_link_seq_is_duplicate(&tracker, 1), "the next seq is not a duplicate");
    check(tracker.holes == 0 && tracker.duplicates == 0, "no spurious holes across the wrap");
}

static void test_sessions(void)
{
    printf("== sessions ==\n");

    mic_link_session_t state;
    mic_link_session_reset(&state);

    check(mic_link_session_adopt(&state, 100), "first session adopted");
    check(!mic_link_session_adopt(&state, 100), "same session is not re-adopted");
    check(state.session_changes == 0, "no session change recorded yet");

    mic_link_seq_observe(&state.audio, 0);
    mic_link_seq_observe(&state.audio, 1);
    mic_link_seq_observe(&state.key, 0);
    check(state.audio.received == 2, "audio tracker primed");

    check(mic_link_session_adopt(&state, 200), "new session adopted");
    check(state.session_changes == 1, "one session change recorded");
    check(state.audio.received == 0 && state.audio.holes == 0 && !state.audio.primed,
          "audio tracker reset on session change");
    check(state.key.received == 0 && !state.key.primed, "key tracker reset on session change");

    // After the reset, a restart from seq 0 must not look like a huge hole.
    mic_link_seq_observe(&state.audio, 0);
    check(state.audio.holes == 0, "restarted numbering creates no hole");

    check(!mic_link_session_adopt(NULL, 1), "null session state is harmless");
}

// ---- key retransmission contract ------------------------------------------

static void test_key_retransmit_contract(void)
{
    printf("== key retransmission contract ==\n");

    // A key event is retransmitted with an unchanged seq until acked; the receiver's
    // duplicate check must suppress the repeats, and the ack must echo the seq as-is.
    mic_link_seq_t key_tracker;
    mic_link_seq_reset(&key_tracker);

    mic_link_frame_t first;
    mic_link_key_t ev = {.key_id = MIC_LINK_KEY_OK, .event = MIC_LINK_KEY_EVENT_PRESS};
    check_err(mic_link_build_key(&first, 42, 100, &ev), ESP_OK, "build first transmission");

    mic_link_frame_t retry;
    check_err(mic_link_build_key(&retry, 42, 100, &ev), ESP_OK, "build retransmission");
    check(first.len == retry.len && memcmp(first.bytes, retry.bytes, first.len) == 0,
          "retransmission is byte-identical, including seq");

    mic_link_view_t view;
    mic_link_key_t decoded;
    mic_link_parse(first.bytes, first.len, &view);
    mic_link_decode_key(&view, &decoded);
    check(!mic_link_seq_is_duplicate(&key_tracker, view.header.seq), "first event is new");
    mic_link_seq_observe(&key_tracker, view.header.seq);

    mic_link_parse(retry.bytes, retry.len, &view);
    check(mic_link_seq_is_duplicate(&key_tracker, view.header.seq), "retransmission is a duplicate");
    mic_link_seq_observe(&key_tracker, view.header.seq);
    check(key_tracker.duplicates == 1, "duplicate counted once");

    mic_link_frame_t ack;
    check_err(mic_link_build_key_ack(&ack, 42, 7, view.header.seq), ESP_OK, "build ack for that seq");
    uint32_t acked = 0;
    mic_link_parse(ack.bytes, ack.len, &view);
    mic_link_decode_key_ack(&view, &acked);
    check(acked == 100, "ack carries the retransmitted event seq, not the ack's own seq");
    check(view.header.seq == 7, "ack has its own seq, independent of the key stream");
}

int main(void)
{
    printf("== MicClicker link protocol host tests ==\n\n");

    test_audio_frame_layout();
    printf("\n");
    test_key_frames();
    printf("\n");
    test_heartbeat_frames();
    printf("\n");
    test_pair_frames();
    printf("\n");
    test_rejections();
    printf("\n");
    test_type_table();
    printf("\n");
    test_seq_in_order();
    printf("\n");
    test_seq_gap_and_late();
    printf("\n");
    test_seq_duplicates();
    printf("\n");
    test_seq_wraparound();
    printf("\n");
    test_seq_large_jump();
    printf("\n");
    test_seq_duplicate_near_wrap();
    printf("\n");
    test_sessions();
    printf("\n");
    test_key_retransmit_contract();
    printf("\n");

    if (g_failures == 0) {
        printf("LINK_PROTO_TEST: PASS checks=%d\n", g_checks);
        return 0;
    }
    printf("LINK_PROTO_TEST: FAIL failures=%d checks=%d\n", g_failures, g_checks);
    return 1;
}
