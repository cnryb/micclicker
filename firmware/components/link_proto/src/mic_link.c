#include "mic_link.h"

#include <string.h>

static void put_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16_le(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

size_t mic_link_frame_bytes(uint8_t type)
{
    switch (type) {
    case MIC_LINK_TYPE_AUDIO:
        return MIC_LINK_AUDIO_FRAME_BYTES;
    case MIC_LINK_TYPE_KEY:
        return MIC_LINK_KEY_FRAME_BYTES;
    case MIC_LINK_TYPE_KEY_ACK:
        return MIC_LINK_KEY_ACK_FRAME_BYTES;
    case MIC_LINK_TYPE_HEARTBEAT:
        return MIC_LINK_HEARTBEAT_FRAME_BYTES;
    case MIC_LINK_TYPE_HEARTBEAT_ACK:
        return MIC_LINK_HEARTBEAT_ACK_FRAME_BYTES;
    case MIC_LINK_TYPE_PAIR_REQ:
        return MIC_LINK_PAIR_REQ_FRAME_BYTES;
    case MIC_LINK_TYPE_PAIR_RESP:
        return MIC_LINK_PAIR_RESP_FRAME_BYTES;
    default:
        return 0;
    }
}

const char *mic_link_type_name(uint8_t type)
{
    switch (type) {
    case MIC_LINK_TYPE_AUDIO:
        return "audio";
    case MIC_LINK_TYPE_KEY:
        return "key";
    case MIC_LINK_TYPE_KEY_ACK:
        return "key_ack";
    case MIC_LINK_TYPE_HEARTBEAT:
        return "heartbeat";
    case MIC_LINK_TYPE_HEARTBEAT_ACK:
        return "heartbeat_ack";
    case MIC_LINK_TYPE_PAIR_REQ:
        return "pair_req";
    case MIC_LINK_TYPE_PAIR_RESP:
        return "pair_resp";
    default:
        return "unknown";
    }
}

// Write the common header and zero the payload area that the caller fills in.
static esp_err_t begin_frame(mic_link_frame_t *frame, uint8_t type, uint16_t session,
                             uint32_t seq, size_t payload_bytes)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t total = MIC_LINK_HEADER_BYTES + payload_bytes;
    if (total > MIC_LINK_MAX_FRAME_BYTES) {
        frame->len = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    frame->bytes[0] = MIC_LINK_VERSION;
    frame->bytes[1] = type;
    put_u16_le(&frame->bytes[2], session);
    put_u32_le(&frame->bytes[4], seq);
    memset(&frame->bytes[MIC_LINK_HEADER_BYTES], 0, payload_bytes);
    frame->len = total;
    return ESP_OK;
}

esp_err_t mic_link_build_audio(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                               const uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES])
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = begin_frame(frame, MIC_LINK_TYPE_AUDIO, session, seq, MIC_LINK_AUDIO_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(&frame->bytes[MIC_LINK_HEADER_BYTES], payload, MIC_LINK_AUDIO_PAYLOAD_BYTES);
    return ESP_OK;
}

esp_err_t mic_link_build_key(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                             const mic_link_key_t *key)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = begin_frame(frame, MIC_LINK_TYPE_KEY, session, seq, MIC_LINK_KEY_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    frame->bytes[MIC_LINK_HEADER_BYTES] = key->key_id;
    frame->bytes[MIC_LINK_HEADER_BYTES + 1] = key->event;
    return ESP_OK;
}

esp_err_t mic_link_build_key_ack(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                                 uint32_t acked_key_seq)
{
    esp_err_t err = begin_frame(frame, MIC_LINK_TYPE_KEY_ACK, session, seq, MIC_LINK_KEY_ACK_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    put_u32_le(&frame->bytes[MIC_LINK_HEADER_BYTES], acked_key_seq);
    return ESP_OK;
}

esp_err_t mic_link_build_heartbeat(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                                   const mic_link_heartbeat_t *heartbeat)
{
    if (heartbeat == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = begin_frame(frame, MIC_LINK_TYPE_HEARTBEAT, session, seq, MIC_LINK_HEARTBEAT_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *payload = &frame->bytes[MIC_LINK_HEADER_BYTES];
    payload[0] = heartbeat->phase;
    payload[1] = heartbeat->mode;
    put_u16_le(&payload[2], heartbeat->flags);
    return ESP_OK;
}

esp_err_t mic_link_build_heartbeat_ack(mic_link_frame_t *frame, uint16_t session, uint32_t seq,
                                       const mic_link_heartbeat_ack_t *ack)
{
    if (ack == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = begin_frame(frame, MIC_LINK_TYPE_HEARTBEAT_ACK, session, seq, MIC_LINK_HEARTBEAT_ACK_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *payload = &frame->bytes[MIC_LINK_HEADER_BYTES];
    put_u32_le(&payload[0], ack->echo_seq);
    payload[4] = ack->phase;
    payload[5] = ack->state;
    put_u16_le(&payload[6], ack->flags);
    put_u32_le(&payload[8], ack->audio_rx);
    put_u32_le(&payload[12], ack->audio_dup);
    put_u32_le(&payload[16], ack->audio_gap);
    put_u32_le(&payload[20], ack->audio_late);
    put_u32_le(&payload[24], ack->key_rx);
    put_u32_le(&payload[28], ack->key_dup);
    put_u32_le(&payload[32], ack->rejected);
    return ESP_OK;
}

esp_err_t mic_link_build_pair_req(mic_link_frame_t *frame, const mic_link_pair_req_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // Pairing happens before either end knows the other's session, so both pairing
    // frames carry session 0 and seq 0 and are exempt from sequence accounting.
    esp_err_t err = begin_frame(frame, MIC_LINK_TYPE_PAIR_REQ, 0, 0, MIC_LINK_PAIR_REQ_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *payload = &frame->bytes[MIC_LINK_HEADER_BYTES];
    memcpy(&payload[0], request->lmk, MIC_LINK_PAIR_KEY_BYTES);
    memcpy(&payload[MIC_LINK_PAIR_KEY_BYTES], request->pmk, MIC_LINK_PAIR_KEY_BYTES);
    payload[MIC_LINK_PAIR_KEY_BYTES * 2] = request->channel;
    return ESP_OK;
}

esp_err_t mic_link_build_pair_resp(mic_link_frame_t *frame, const mic_link_pair_resp_t *response)
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = begin_frame(frame, MIC_LINK_TYPE_PAIR_RESP, 0, 0, MIC_LINK_PAIR_RESP_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *payload = &frame->bytes[MIC_LINK_HEADER_BYTES];
    payload[0] = response->result;
    payload[1] = response->state;
    put_u16_le(&payload[2], response->reserved);
    return ESP_OK;
}

esp_err_t mic_link_parse(const uint8_t *data, size_t len, mic_link_view_t *view)
{
    if (data == NULL || view == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > MIC_LINK_MAX_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(view, 0, sizeof(*view));
    if (len < MIC_LINK_HEADER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (data[0] != MIC_LINK_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    size_t expected = mic_link_frame_bytes(data[1]);
    if (expected == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Exact match: a truncated payload and a payload with trailing bytes are both
    // rejected, so a sender cannot smuggle unparsed data past the length check.
    if (len != expected) {
        return ESP_ERR_INVALID_SIZE;
    }
    view->header.version = data[0];
    view->header.type = data[1];
    view->header.session = get_u16_le(&data[2]);
    view->header.seq = get_u32_le(&data[4]);
    view->payload = &data[MIC_LINK_HEADER_BYTES];
    view->payload_len = len - MIC_LINK_HEADER_BYTES;
    return ESP_OK;
}

static esp_err_t expect(const mic_link_view_t *view, uint8_t type, size_t payload_len)
{
    if (view == NULL || view->payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (view->header.type != type) {
        return ESP_ERR_INVALID_ARG;
    }
    if (view->payload_len != payload_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t mic_link_decode_audio(const mic_link_view_t *view, const uint8_t **payload)
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = expect(view, MIC_LINK_TYPE_AUDIO, MIC_LINK_AUDIO_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    *payload = view->payload;
    return ESP_OK;
}

esp_err_t mic_link_decode_key(const mic_link_view_t *view, mic_link_key_t *key)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = expect(view, MIC_LINK_TYPE_KEY, MIC_LINK_KEY_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    key->key_id = view->payload[0];
    key->event = view->payload[1];
    return ESP_OK;
}

esp_err_t mic_link_decode_key_ack(const mic_link_view_t *view, uint32_t *acked_key_seq)
{
    if (acked_key_seq == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = expect(view, MIC_LINK_TYPE_KEY_ACK, MIC_LINK_KEY_ACK_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    *acked_key_seq = get_u32_le(view->payload);
    return ESP_OK;
}

esp_err_t mic_link_decode_heartbeat(const mic_link_view_t *view, mic_link_heartbeat_t *heartbeat)
{
    if (heartbeat == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = expect(view, MIC_LINK_TYPE_HEARTBEAT, MIC_LINK_HEARTBEAT_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    heartbeat->phase = view->payload[0];
    heartbeat->mode = view->payload[1];
    heartbeat->flags = get_u16_le(&view->payload[2]);
    return ESP_OK;
}

esp_err_t mic_link_decode_heartbeat_ack(const mic_link_view_t *view, mic_link_heartbeat_ack_t *ack)
{
    if (ack == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = expect(view, MIC_LINK_TYPE_HEARTBEAT_ACK, MIC_LINK_HEARTBEAT_ACK_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    ack->echo_seq = get_u32_le(&view->payload[0]);
    ack->phase = view->payload[4];
    ack->state = view->payload[5];
    ack->flags = get_u16_le(&view->payload[6]);
    ack->audio_rx = get_u32_le(&view->payload[8]);
    ack->audio_dup = get_u32_le(&view->payload[12]);
    ack->audio_gap = get_u32_le(&view->payload[16]);
    ack->audio_late = get_u32_le(&view->payload[20]);
    ack->key_rx = get_u32_le(&view->payload[24]);
    ack->key_dup = get_u32_le(&view->payload[28]);
    ack->rejected = get_u32_le(&view->payload[32]);
    return ESP_OK;
}

esp_err_t mic_link_decode_pair_req(const mic_link_view_t *view, mic_link_pair_req_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = expect(view, MIC_LINK_TYPE_PAIR_REQ, MIC_LINK_PAIR_REQ_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(request->lmk, &view->payload[0], MIC_LINK_PAIR_KEY_BYTES);
    memcpy(request->pmk, &view->payload[MIC_LINK_PAIR_KEY_BYTES], MIC_LINK_PAIR_KEY_BYTES);
    request->channel = view->payload[MIC_LINK_PAIR_KEY_BYTES * 2];
    return ESP_OK;
}

esp_err_t mic_link_decode_pair_resp(const mic_link_view_t *view, mic_link_pair_resp_t *response)
{
    if (response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = expect(view, MIC_LINK_TYPE_PAIR_RESP, MIC_LINK_PAIR_RESP_PAYLOAD_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    response->result = view->payload[0];
    response->state = view->payload[1];
    response->reserved = get_u16_le(&view->payload[2]);
    return ESP_OK;
}

void mic_link_seq_reset(mic_link_seq_t *tracker)
{
    if (tracker == NULL) {
        return;
    }
    memset(tracker, 0, sizeof(*tracker));
}

bool mic_link_seq_is_duplicate(const mic_link_seq_t *tracker, uint32_t seq)
{
    if (tracker == NULL || !tracker->primed) {
        return false;
    }
    int32_t delta = (int32_t)(seq - tracker->next);
    if (delta >= 0) {
        return false;
    }
    uint32_t distance = (uint32_t)(-delta);
    if (distance > MIC_LINK_SEQ_WINDOW) {
        return false;
    }
    return (tracker->recent & (UINT64_C(1) << (distance - 1))) != 0;
}

// Shift the received window so the newest sequence number lands on bit 0.
//
// `advance` is how many sequence numbers the stream moved forward since the
// previously recorded newest frame: 1 for the next frame in order, gap + 1 after a
// jump. A jump wider than the window evicts every recorded entry, which is correct:
// those entries are now more than MIC_LINK_SEQ_WINDOW frames behind and can no
// longer be classified as late.
static void record_newest(mic_link_seq_t *tracker, uint32_t advance)
{
    tracker->recent = advance >= MIC_LINK_SEQ_WINDOW ? UINT64_C(1)
                                                   : ((tracker->recent << advance) | UINT64_C(1));
}

void mic_link_seq_observe(mic_link_seq_t *tracker, uint32_t seq)
{
    if (tracker == NULL) {
        return;
    }
    ++tracker->received;

    if (!tracker->primed) {
        tracker->primed = true;
        tracker->next = seq + 1;
        record_newest(tracker, 0);
        return;
    }

    int32_t delta = (int32_t)(seq - tracker->next);
    if (delta == 0) {
        ++tracker->next;
        record_newest(tracker, 1);
        return;
    }
    if (delta > 0) {
        uint32_t gap = (uint32_t)delta;
        tracker->holes += gap;
        if (gap > tracker->largest_hole) {
            tracker->largest_hole = gap;
        }
        tracker->next = seq + 1;
        // gap + 1: the previous newest frame is now that many numbers behind.
        record_newest(tracker, gap + 1);
        return;
    }

    uint32_t distance = (uint32_t)(-delta);
    if (distance > MIC_LINK_SEQ_WINDOW) {
        // Too far behind to classify. Counting it as late would silently reduce the
        // reported loss, so it is recorded separately instead.
        ++tracker->resyncs;
        return;
    }
    uint64_t bit = UINT64_C(1) << (distance - 1);
    if ((tracker->recent & bit) != 0) {
        ++tracker->duplicates;
    } else {
        tracker->recent |= bit;
        ++tracker->late;
    }
}

uint32_t mic_link_seq_missing(const mic_link_seq_t *tracker)
{
    if (tracker == NULL || tracker->holes <= tracker->late) {
        return 0;
    }
    return tracker->holes - tracker->late;
}

void mic_link_session_reset(mic_link_session_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

bool mic_link_session_adopt(mic_link_session_t *state, uint16_t session)
{
    if (state == NULL) {
        return false;
    }
    if (state->session_valid && state->session == session) {
        return false;
    }
    if (state->session_valid) {
        ++state->session_changes;
    }
    state->session_valid = true;
    state->session = session;
    // Sequence numbering restarts with the sender's boot; carrying the trackers over
    // would report every restart as a multi-billion-frame hole.
    mic_link_seq_reset(&state->audio);
    mic_link_seq_reset(&state->key);
    return true;
}
