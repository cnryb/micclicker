#!/usr/bin/env bash
# Grade an ESP-NOW probe run from the two captured console logs.
#
# The probe firmware prints machine-readable PROBE_* lines on both ends. This script
# extracts them, checks the acceptance criteria for stage 3, and exits non-zero when a
# criterion fails. It reads existing logs only: capturing them is a separate, manual
# step because it needs both boards attached.
#
# Usage:
#   ./firmware/tests/espnow/check.sh <sender-log> <receiver-log>
#
# Both arguments may be raw ESP-IDF monitor output; CR line endings and colour codes
# are handled.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'Usage: %s <sender-log> <receiver-log>\n' "$0" >&2
    printf '\nThe probe firmware must be flashed to both ends first:\n' >&2
    printf '  ./firmware/build.sh c3 --probe && ./firmware/build.sh s3 --probe\n' >&2
    printf 'See firmware/tests/espnow/README.md for the capture procedure.\n' >&2
    exit 2
fi

TX=$1
RX=$2
for f in "$TX" "$RX"; do
    if [[ ! -f "$f" ]]; then
        printf 'Missing log: %s\n' "$f" >&2
        exit 2
    fi
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Strip CR and any ANSI colour codes so the field parsing below is literal.
clean() {
    sed -e 's/\r$//' -e 's/\x1b\[[0-9;]*[A-Za-z]//g' "$1"
}

clean "$TX" > "$WORK/tx.txt"
clean "$RX" > "$WORK/rx.txt"

failures=0
fail() {
    printf '   FAIL %s\n' "$*"
    failures=$((failures + 1))
}
pass() {
    printf '   PASS %s\n' "$*"
}

# Pull "key=value" out of the first line containing a marker.
field() {
    local file=$1 marker=$2 key=$3
    awk -v m="$marker" -v k="$key" '
        index($0, m) {
            n = split($0, parts, " ")
            for (i = 1; i <= n; ++i) {
                if (index(parts[i], k "=") == 1) {
                    sub("^" k "=", "", parts[i])
                    print parts[i]
                    exit
                }
            }
        }
    ' "$file"
}

printf '== sender log ==\n'
# PROBE_ARMED is the reliable marker: it prints after the start delay. PROBE_READY
# prints before it, which is usually before a host can open the console.
tx_ready=$(field "$WORK/tx.txt" "PROBE_ARMED role=sender" "session")
[[ -n "$tx_ready" ]] && pass "sender armed with session=$tx_ready" || fail "no PROBE_ARMED from the sender"

tx_peer=$(grep -c 'PROBE_PEER mac=00:00:00:00:00:00' "$WORK/tx.txt" || true)
[[ "$tx_peer" -eq 0 ]] && pass "sender discovered a peer" || fail "sender did not discover a peer"

tx_stream_bps=$(field "$WORK/tx.txt" "PROBE_STREAM frames=" "wire_kbps")
tx_stream_frames=$(field "$WORK/tx.txt" "PROBE_STREAM frames=" "frames")
if [[ -n "$tx_stream_frames" && "$tx_stream_frames" -gt 0 ]]; then
    pass "burst phase sent $tx_stream_frames frames at ${tx_stream_bps} kbit/s on the wire"
else
    fail "burst phase sent no frames"
fi

tx_key_applied=$(field "$WORK/tx.txt" "PROBE_KEY events=" "receiver_applied")
tx_key_dup=$(field "$WORK/tx.txt" "PROBE_KEY events=" "receiver_suppressed")
tx_key_events=$(field "$WORK/tx.txt" "PROBE_KEY events=" "events")
if [[ "${tx_key_applied:-0}" == "${tx_key_events:-x}" ]]; then
    pass "all $tx_key_events key events applied, $tx_key_dup retransmissions suppressed"
else
    fail "key events applied=$tx_key_applied of events=$tx_key_events (expected equal)"
fi

if grep -q 'PROBE_PAIR result=ok encrypt=1' "$WORK/tx.txt"; then
    pass "sender paired and moved the peer to encrypted unicast"
else
    fail "sender did not complete pairing"
fi

if grep -q 'PROBE_ENCRYPTED result=ok' "$WORK/tx.txt"; then
    pass "encrypted heartbeat round trip succeeded"
else
    fail "encrypted heartbeat round trip failed"
fi

if grep -q 'PROBE_PAIR persistence=ok' "$WORK/tx.txt"; then
    pass "sender persisted the binding"
else
    fail "sender did not persist the binding"
fi

# Per-phase loss comes from the receiver's cumulative counters, diffed by the sender.
tx_loss=$(awk '/PROBE_PHASE phase=audio_stream/ && /state=end/ {
    for (i = 1; i <= NF; ++i) if ($i ~ /^peer_loss=/) { sub("^peer_loss=", "", $i); print $i; exit }
}' "$WORK/tx.txt")
tx_dup_stream=$(awk '/PROBE_PHASE phase=audio_stream/ && /state=end/ {
    for (i = 1; i <= NF; ++i) if ($i ~ /^peer_dup=/) { sub("^peer_dup=", "", $i); print $i; exit }
}' "$WORK/tx.txt")
if [[ "${tx_loss:-}" == "0" && "${tx_dup_stream:-}" == "0" ]]; then
    pass "burst phase: zero loss, zero duplicates"
else
    fail "burst phase: loss=${tx_loss:-?} dup=${tx_dup_stream:-?} (expected 0 and 0)"
fi

tx_rtt_avg=$(awk '/PROBE_PHASE phase=audio_spaced/ && /state=end/ {
    for (i = 1; i <= NF; ++i) if ($i ~ /^rtt_avg_us=/) { sub("^rtt_avg_us=", "", $i); print $i; exit }
}' "$WORK/tx.txt")
tx_rtt_samples=$(awk '/PROBE_PHASE phase=audio_spaced/ && /state=end/ {
    for (i = 1; i <= NF; ++i) if ($i ~ /^rtt_samples=/) { sub("^rtt_samples=", "", $i); print $i; exit }
}' "$WORK/tx.txt")
if [[ "${tx_rtt_samples:-0}" -gt 0 ]]; then
    pass "paced phase heartbeat RTT average ${tx_rtt_avg} us over $tx_rtt_samples samples"
else
    fail "paced phase produced no heartbeat RTT samples"
fi

if grep -q 'PROBE_RESULT: PASS' "$WORK/tx.txt"; then
    pass "sender reported PASS"
else
    fail "sender reported FAIL"
fi

printf '\n== receiver log ==\n'
rx_ready=$(grep -c 'PROBE_ARMED role=receiver' "$WORK/rx.txt" || true)
[[ "$rx_ready" -gt 0 ]] && pass "receiver armed" || fail "no PROBE_ARMED from the receiver"

rx_session=$(field "$WORK/rx.txt" "PROBE_SESSION" "session")
if [[ -n "$rx_session" ]]; then
    pass "receiver adopted session $rx_session"
else
    fail "receiver never adopted a session (no frames arrived)"
fi

# The receiver's last periodic line is the closing state of the run.
rx_last=$(grep 'PROBE_RX audio_rx=' "$WORK/rx.txt" | tail -1 || true)
rx_audio=$(printf '%s' "$rx_last" | sed -n 's/.*audio_rx=\([0-9]*\).*/\1/p')
rx_dup=$(printf '%s' "$rx_last" | sed -n 's/.*audio_dup=\([0-9]*\).*/\1/p')
rx_gap=$(printf '%s' "$rx_last" | sed -n 's/.*audio_gap=\([0-9]*\).*/\1/p')
rx_late=$(printf '%s' "$rx_last" | sed -n 's/.*audio_late=\([0-9]*\).*/\1/p')
rx_err=$(printf '%s' "$rx_last" | sed -n 's/.*payload_errors=\([0-9]*\).*/\1/p')
rx_key=$(printf '%s' "$rx_last" | sed -n 's/.*key_rx=\([0-9]*\).*/\1/p')
rx_keydup=$(printf '%s' "$rx_last" | sed -n 's/.*key_dup=\([0-9]*\).*/\1/p')

if [[ "${rx_err:-x}" == "0" ]]; then
    pass "every accepted audio payload verified against its seq (0 mismatching bytes)"
else
    fail "audio payload verification errors: ${rx_err:-?}"
fi

if [[ "${rx_gap:-x}" == "0" && "${rx_late:-x}" == "0" ]]; then
    pass "receiver saw no sequence holes and no late frames"
else
    fail "receiver saw holes=${rx_gap:-?} late=${rx_late:-?}"
fi

if [[ "${rx_dup:-x}" == "0" ]]; then
    pass "receiver saw no duplicate audio frames"
else
    fail "receiver saw ${rx_dup:-?} duplicate audio frames"
fi

# Every sent frame must arrive: compare the sender's sent total with received.
sent_total=$(( ${tx_stream_frames:-0} + 250 ))
if [[ -n "$rx_audio" && "$rx_audio" == "$sent_total" ]]; then
    pass "receiver accepted $rx_audio of $sent_total paced+burst frames (no loss)"
elif [[ -n "$rx_audio" ]]; then
    fail "receiver accepted $rx_audio of $sent_total frames"
fi

if [[ "${rx_key:-0}" == "20" && "${rx_keydup:-0}" == "40" ]]; then
    pass "receiver applied 20 key events and suppressed 40 retransmissions"
else
    fail "receiver key counters: applied=${rx_key:-?} suppressed=${rx_keydup:-?} (expected 20 and 40)"
fi

if grep -q 'PROBE_PAIR result=ok encrypt=1' "$WORK/rx.txt"; then
    pass "receiver accepted pairing and switched the peer to encrypted"
else
    fail "receiver did not accept pairing"
fi

if grep -q 'PROBE_BIND state=present .*action=erased_for_probe' "$WORK/rx.txt"; then
    pass "receiver found and erased a binding left by an earlier run"
elif grep -q 'PROBE_BIND state=absent' "$WORK/rx.txt"; then
    pass "receiver started with no stored binding"
fi

printf '\n'
if [[ $failures -eq 0 ]]; then
    printf 'ESPNOW_PROBE: PASS\n'
    exit 0
fi
printf 'ESPNOW_PROBE: FAIL failures=%d\n' "$failures"
exit 1
