#include "mic_probe_shared.h"

void probe_fill_audio(uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES], uint32_t seq)
{
    for (size_t i = 0; i < MIC_LINK_AUDIO_PAYLOAD_BYTES; ++i) {
        payload[i] = PROBE_AUDIO_PATTERN(seq, i);
    }
}

uint32_t probe_check_audio(const uint8_t payload[MIC_LINK_AUDIO_PAYLOAD_BYTES], uint32_t seq)
{
    uint32_t mismatches = 0;
    for (size_t i = 0; i < MIC_LINK_AUDIO_PAYLOAD_BYTES; ++i) {
        if (payload[i] != PROBE_AUDIO_PATTERN(seq, i)) {
            ++mismatches;
        }
    }
    return mismatches;
}

const char *probe_phase_name(uint8_t phase)
{
    switch (phase) {
    case PROBE_PHASE_IDLE:
        return "idle";
    case PROBE_PHASE_DISCOVERY:
        return "discovery";
    case PROBE_PHASE_AUDIO_STREAM:
        return "audio_stream";
    case PROBE_PHASE_AUDIO_SPACED:
        return "audio_spaced";
    case PROBE_PHASE_KEY_RETRY:
        return "key_retry";
    case PROBE_PHASE_CHANNEL_SWEEP:
        return "channel_sweep";
    case PROBE_PHASE_ENCRYPTED:
        return "encrypted";
    case PROBE_PHASE_DONE:
        return "done";
    default:
        return "unknown";
    }
}
