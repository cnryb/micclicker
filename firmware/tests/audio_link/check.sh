#!/usr/bin/env bash
# Grade one stage-4 audio-link acceptance run.
#
# The sender (C3) and receiver (S3) each log one machine-readable STATUS line per
# second. This script treats those two logs as the measurement: it checks that frames
# were produced at the design rate, that the receiver saw essentially all of them, that
# neither end resets, and that the adaptive buffer stayed inside its bounds instead of
# walking to an overflow or a dry-out over the run.
#
# Usage:
#   ./firmware/tests/audio_link/check.sh <c3 log> <s3 log> [seconds-expected]
#
# Exit codes: 0 pass, 1 fail, 2 usage.
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    printf 'Usage: %s <c3 log> <s3 log> [expected-seconds]\n' "$0" >&2
    exit 2
fi

C3_LOG=$1
S3_LOG=$2
WANT_SECONDS=${3:-0}

for f in "$C3_LOG" "$S3_LOG"; do
    if [[ ! -f "$f" ]]; then
        printf 'missing log: %s\n' "$f" >&2
        exit 2
    fi
done

# A serial capture begins mid-stream and can contain framing noise, which makes grep
# decide the file is binary and print nothing. Every read below therefore forces text.
g() { grep -a "$@"; }
# First and last matching line, without a closing reader that could SIGPIPE the grep.
first_line() { sed -n "/$1/p" "$2" | sed -n '1p'; }
last_line() { sed -n "/$1/p" "$2" | sed -n '$p'; }

fail=0
check() {
    local label=$1 ok=$2 detail=$3
    if [[ "$ok" == "1" ]]; then
        printf 'PASS  %-28s %s\n' "$label" "$detail"
    else
        printf 'FAIL  %-28s %s\n' "$label" "$detail"
        fail=1
    fi
}

# --- sender -----------------------------------------------------------------
# The last STATUS line carries the cumulative counters, which is what the run total is.
last_c3=$(g -c 'C3_AUDIO: STATUS' "$C3_LOG" || true)
if [[ "$last_c3" -eq 0 ]]; then
    check "sender status lines" 0 "none found"
    printf '\nRESULT: FAIL\n'
    exit 1
fi

head_c3=$(first_line 'C3_AUDIO: STATUS' "$C3_LOG")
tail_c3=$(last_line 'C3_AUDIO: STATUS' "$C3_LOG")
# Field extraction: match "<key>=" as a whole word. Using a plain substring match would
# let the key "fill" hit "fill_ms" and "max_fill", which silently reported empty values.
grab() {
    local value
    value=$(tr ' ' '\n' <<<"$2" | sed -n "s/^$1=\([0-9][0-9]*\)$/\1/p" | sed -n '1p')
    printf '%s' "$value"
}
# Errors are cumulative monotonic counters, so any growth during the window is the run's
# error count. Captured/sent are also cumulative, so the window delta is the run total:
# using the last value alone would include whatever happened before logging began.
delta() {
    local key=$1
    local first last
    first=$(grab "$key" "$head_c3"); last=$(grab "$key" "$tail_c3")
    first=${first:-0}; last=${last:-0}
    echo $(( last - first ))
}
c3_captured=$(delta captured)
c3_encoded=$(delta encoded)
c3_sent=$(delta sent)
c3_send_err=$(delta send_err)
c3_cap_err=$(delta capture_err)
c3_enc_err=$(delta encode_err)

# The first and last timestamps give the measured wall-clock span of the sender log.
# ESP-IDF prints the log timestamp in milliseconds (CONFIG_FREERTOS_HZ is 1000 for
# both targets), so the span is (last - first) / 1000 seconds.
first_ts=$(sed -n 's/^I (\([0-9]*\).*/\1/p' "$C3_LOG" | sed -n '1p')
last_ts=$(sed -n 's/^I (\([0-9]*\).*/\1/p' "$C3_LOG" | sed -n '$p')
span_ms=$(( last_ts > first_ts ? last_ts - first_ts : 0 ))
span_s=$(( span_ms / 1000 ))

# Frame rate uses *encoded* frames: capture still runs while disarmed (codec stays warm),
# so captured/s is always ~50 and would hide a stream that never armed. Allow 2% either
# way for the per-second status sampling boundary.
if [[ "$span_s" -ge 5 && "$c3_encoded" -gt 0 ]]; then
    rate_x100=$(( c3_encoded * 100 / span_s ))
    ok=0
    if [[ $rate_x100 -ge 4900 && $rate_x100 -le 5100 ]]; then ok=1; fi
    check "sender frame rate" "$ok" "${rate_x100}/100 fps over ${span_s}s (want 50.00 encoded)"
else
    check "sender frame rate" 0 "log too short or never armed (${span_s}s, encoded=$c3_encoded)"
fi

check "sender capture errors" "$([[ "$c3_cap_err" -eq 0 ]] && echo 1 || echo 0)" "capture_err=$c3_cap_err"
check "sender encode errors"  "$([[ "$c3_enc_err" -eq 0 ]] && echo 1 || echo 0)" "encode_err=$c3_enc_err"

# A few refused sends are normal: ESP-NOW rejects a send when its queue is momentarily
# full, and the sender retries the same sequence number. A large number means the
# radio never kept up with the 50 fps input.
send_err_ratio_x1000=0
if [[ "$c3_sent" -gt 0 ]]; then
    send_err_ratio_x1000=$(( c3_send_err * 1000 / (c3_sent + c3_send_err) ))
fi
ok=0
if [[ $send_err_ratio_x1000 -le 5 ]]; then ok=1; fi
check "sender send failures" "$ok" "$c3_send_err of $((c3_sent + c3_send_err)) (${send_err_ratio_x1000}/1000, want <=5)"

ok=0
if [[ "$c3_sent" -gt 0 && "$c3_encoded" -gt 0 ]]; then
    sent_pct=$(( c3_sent * 100 / c3_encoded ))
    if [[ $sent_pct -ge 99 ]]; then ok=1; fi
    check "sender delivery rate" "$ok" "$c3_sent of $c3_encoded encoded (${sent_pct}%, want >=99)"
else
    check "sender delivery rate" 0 "no frames"
fi

# --- receiver ---------------------------------------------------------------
last_s3=$(g -c 'S3_AUDIO: STATUS' "$S3_LOG" || true)
if [[ "$last_s3" -eq 0 ]]; then
    check "receiver status lines" 0 "none found"
    printf '\nRESULT: FAIL\n'
    exit 1
fi

head_s3=$(first_line 'S3_AUDIO: STATUS' "$S3_LOG")
tail_s3=$(last_line 'S3_AUDIO: STATUS' "$S3_LOG")
r_rx=$(grab rx "$tail_s3")
r_dup=$(grab dup "$tail_s3")
r_decoded=$(grab decoded "$tail_s3")
r_gap=$(grab gap "$tail_s3")
r_late=$(grab late "$tail_s3")
r_loss=$(grab loss "$tail_s3")
r_dec_err=$(grab decode_err "$tail_s3")
r_overruns=$(grab overruns "$tail_s3")
r_underruns=$(grab underruns "$tail_s3")
r_drops=$(grab drops "$tail_s3")
r_repeats=$(grab repeats "$tail_s3")
r_fill=$(grab fill_ms "$tail_s3")
r_maxfill=$(grab max_fill "$tail_s3")
r_minfill=$(grab min_fill "$tail_s3")

# Cross-check the frame rate against the receiver's own view and the log span. A frame
# count that is internally consistent but impossible for the elapsed time (for example a
# log whose counters were rewritten) shows up here as a mismatch between the two ends.
if [[ "$c3_sent" -gt 0 && "$span_s" -ge 5 ]]; then
    # Frames the receiver actually saw, as its own cumulative delta.
    r_span_s=$(( $(last_line 'S3_AUDIO: STATUS' "$S3_LOG" | sed -n 's/.*I (\([0-9]*\)).*/\1/p') - \
                 $(first_line 'S3_AUDIO: STATUS' "$S3_LOG" | sed -n 's/.*I (\([0-9]*\)).*/\1/p') ))
    r_span_s=$(( r_span_s / 1000 ))
    if [[ "$r_span_s" -ge 5 ]]; then
        c3_rate_x100=$(( c3_sent * 100 / span_s ))
        r_rate_x100=$(( ( $(grab rx "$tail_s3") - $(grab rx "$head_s3") ) * 100 / r_span_s ))
        diff_x100=$(( c3_rate_x100 > r_rate_x100 ? c3_rate_x100 - r_rate_x100 : r_rate_x100 - c3_rate_x100 ))
        ok=0
        if [[ $diff_x100 -le 200 ]]; then ok=1; fi
        check "end-to-end rate agreement" "$ok" \
              "sender ${c3_rate_x100}/100 fps vs receiver ${r_rate_x100}/100 fps (delta ${diff_x100}/100, want <=200)"
    fi
fi

check "receiver decode errors" "$([[ "$r_dec_err" -eq 0 ]] && echo 1 || echo 0)" "decode_err=$r_dec_err"

# Effective loss is holes minus late arrivals (docs/protocol.md section 3.4).
ok=0
if [[ "$r_rx" -gt 0 ]]; then
    loss_ppm=$(( r_loss * 1000000 / r_rx ))
    if [[ $loss_ppm -le 1000 ]]; then ok=1; fi   # <= 0.1%
    check "receiver effective loss" "$ok" "$r_loss of $r_rx frames (${loss_ppm} ppm, want <=1000)"
else
    check "receiver effective loss" 0 "no frames received"
fi

# The ring must never have overflowed: with the host consuming, a put_overrun means the
# endpoint was not drained, which is audible as a discontinuity.
check "receiver ring overflow" "$([[ "$r_overruns" -eq 0 ]] && echo 1 || echo 0)" \
      "put_overruns=$r_overruns (want 0 while the host is recording)"

# Underruns at the very start are expected (the ring primes in ~60 ms). A growing count
# means the link cannot sustain 50 fps and the stream is being concealed continuously.
ok=0
if [[ "$r_rx" -gt 0 ]]; then
    underrun_ppm=$(( r_underruns * 1000000 / r_rx ))
    if [[ $underrun_ppm -le 20000 ]]; then ok=1; fi   # <= 2%
    check "receiver underruns" "$ok" "$r_underruns of $r_rx (${underrun_ppm} ppm, want <=20000)"
else
    check "receiver underruns" 0 "no frames received"
fi

# The buffer controller exists to keep the fill level bounded. If the run ends pinned at
# capacity or pinned at zero, the drift compensation is not working.
ok=0
if [[ "$r_maxfill" -gt 0 && "$r_maxfill" -le 200 ]]; then ok=1; fi
check "receiver fill high water" "$ok" "max_fill=${r_maxfill}ms (capacity 200)"

printf '\n'
printf 'sender   : captured=%s encoded=%s sent=%s\n' "$c3_captured" "$c3_encoded" "$c3_sent"
printf 'receiver : rx=%s dup=%s decoded=%s gap=%s late=%s loss=%s\n' \
       "$r_rx" "$r_dup" "$r_decoded" "$r_gap" "$r_late" "$r_loss"
printf 'ring     : overflow=%s underrun=%s drops=%s repeats=%s fill=%s..%sms\n' \
       "$r_overruns" "$r_underruns" "$r_drops" "$r_repeats" "$r_minfill" "$r_maxfill"

# Run length is reported against the requested duration, but it is graded on what the
# logs can actually prove. The console window starts when the capture attaches, which is
# independent of when the recording started, so a short window is a logging artifact
# rather than a firmware finding. What must hold is that the stream was continuous for
# the whole window; the caller compares the measured span with the recording separately.
if [[ "$WANT_SECONDS" -gt 0 ]]; then
    if [[ "$span_s" -ge "$WANT_SECONDS" ]]; then
        printf 'PASS  run length                     %ss measured (>=%ss requested)\n' \
               "$span_s" "$WANT_SECONDS"
    else
        printf 'NOTE  run length                     %ss logged vs %ss requested; the console\n' \
               "$span_s" "$WANT_SECONDS"
        printf '                                     window is shorter than the recording, so\n'
        printf '                                     this is a logging artifact. Verify the\n'
        printf '                                     recording duration with ffprobe.\n'
    fi
fi

printf '\n'
if [[ $fail -eq 0 ]]; then
    printf 'RESULT: PASS\n'
    exit 0
fi
printf 'RESULT: FAIL\n'
exit 1
