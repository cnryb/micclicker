"""Config HID report encode/decode. Matches docs/config-hid.md."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

REPORT_SIZE = 64
MAP_BLOB_SIZE = 16
MAP_BLOB_VERSION = 1

VID = 0x303A
PID = 0x4014

CMD_PING = 0x01
CMD_LEFT_ALT = 0x02
CMD_ENTER = 0x03
CMD_ALL_KEYS_UP = 0x04
CMD_READ_MAP = 0x10
CMD_WRITE_MAP = 0x11
CMD_RESET_MAP = 0x12

RESULT_OK = 0
RESULT_INVALID = 1
RESULT_BUSY = 2
RESULT_QUEUE_FULL = 3
RESULT_NOT_READY = 4

FLAG_MOUNTED = 0x01
FLAG_STREAMING = 0x02
FLAG_ALT_PRESSED = 0x04
FLAG_ENTER_PRESSED = 0x08
FLAG_CONFIG_ERROR = 0x10
FLAG_PAIRED = 0x20
FLAG_LINK_UP = 0x40

KEY_ID_UP = 0x01
KEY_ID_DOWN = 0x02
KEY_ID_OK = 0x03

KEYCODE_NONE = 0x00
KEYCODE_ENTER = 0x28
MOD_RIGHT_CTRL = 0x10
MOD_RIGHT_ALT = 0x40

KEYCODE_RESERVED_MIN = 0x01
KEYCODE_RESERVED_MAX = 0x03
KEYCODE_MAX = 0xE7


@dataclass(frozen=True)
class KeyBinding:
    key_id: int
    modifier: int
    keycode: int


@dataclass(frozen=True)
class KeyMap:
    up: KeyBinding
    down: KeyBinding
    ok: KeyBinding


@dataclass(frozen=True)
class StatusReport:
    version: int
    flags: int
    last_command: int
    last_result: int
    ping_count: int
    alt_count: int
    enter_count: int
    all_up_count: int
    rejected_count: int
    audio_produced_frames: int
    audio_short_writes: int
    audio_usb_packets: int
    audio_usb_bytes: int
    audio_empty_packets: int
    audio_alt_transitions: int
    audio_rx: int
    audio_gap: int
    audio_dup: int
    audio_late: int
    underruns: int
    overruns: int
    fill_ms: int
    # Present only when the GET followed a successful READ_MAP gate.
    map_blob: Optional[bytes] = None

    @property
    def mounted(self) -> bool:
        return bool(self.flags & FLAG_MOUNTED)

    @property
    def streaming(self) -> bool:
        return bool(self.flags & FLAG_STREAMING)

    @property
    def config_error(self) -> bool:
        return bool(self.flags & FLAG_CONFIG_ERROR)

    @property
    def paired(self) -> bool:
        return bool(self.flags & FLAG_PAIRED)

    @property
    def link_up(self) -> bool:
        return bool(self.flags & FLAG_LINK_UP)


def _u32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "little")


def _put_u32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = int(value).to_bytes(4, "little")


def validate_binding(binding: KeyBinding) -> None:
    if binding.key_id not in (KEY_ID_UP, KEY_ID_DOWN, KEY_ID_OK):
        raise ValueError(f"unknown key_id 0x{binding.key_id:02x}")
    if binding.keycode == KEYCODE_NONE:
        return
    if KEYCODE_RESERVED_MIN <= binding.keycode <= KEYCODE_RESERVED_MAX:
        raise ValueError(f"reserved keycode 0x{binding.keycode:02x}")
    if binding.keycode > KEYCODE_MAX:
        raise ValueError(f"keycode 0x{binding.keycode:02x} out of range")


def validate_map(key_map: KeyMap) -> None:
    for binding in (key_map.up, key_map.down, key_map.ok):
        validate_binding(binding)
    if key_map.up.key_id != KEY_ID_UP or key_map.down.key_id != KEY_ID_DOWN or key_map.ok.key_id != KEY_ID_OK:
        raise ValueError("map bindings must be UP/DOWN/OK in order")


def default_map() -> KeyMap:
    return KeyMap(
        up=KeyBinding(KEY_ID_UP, MOD_RIGHT_ALT, KEYCODE_NONE),
        down=KeyBinding(KEY_ID_DOWN, MOD_RIGHT_CTRL, KEYCODE_NONE),
        ok=KeyBinding(KEY_ID_OK, 0, KEYCODE_ENTER),
    )


def encode_map_blob(key_map: KeyMap) -> bytes:
    validate_map(key_map)
    out = bytearray(MAP_BLOB_SIZE)
    _put_u32(out, 0, MAP_BLOB_VERSION)
    out[4] = key_map.up.modifier & 0xFF
    out[5] = key_map.up.keycode & 0xFF
    out[8] = key_map.down.modifier & 0xFF
    out[9] = key_map.down.keycode & 0xFF
    out[12] = key_map.ok.modifier & 0xFF
    out[13] = key_map.ok.keycode & 0xFF
    return bytes(out)


def decode_map_blob(blob: bytes) -> KeyMap:
    if len(blob) != MAP_BLOB_SIZE:
        raise ValueError(f"map blob length {len(blob)} != {MAP_BLOB_SIZE}")
    if _u32(blob, 0) != MAP_BLOB_VERSION:
        raise ValueError(f"unsupported map blob version {_u32(blob, 0)}")
    key_map = KeyMap(
        up=KeyBinding(KEY_ID_UP, blob[4], blob[5]),
        down=KeyBinding(KEY_ID_DOWN, blob[8], blob[9]),
        ok=KeyBinding(KEY_ID_OK, blob[12], blob[13]),
    )
    validate_map(key_map)
    return key_map


def build_command(opcode: int, payload: bytes = b"") -> bytes:
    if not (0 <= opcode <= 0xFF):
        raise ValueError("opcode out of range")
    if len(payload) > REPORT_SIZE - 1:
        raise ValueError("payload too long")
    frame = bytearray(REPORT_SIZE)
    frame[0] = opcode
    frame[1 : 1 + len(payload)] = payload
    return bytes(frame)


def build_write_map(key_map: KeyMap) -> bytes:
    return build_command(CMD_WRITE_MAP, encode_map_blob(key_map))


def parse_status(report: bytes, *, expect_map: bool = False) -> StatusReport:
    if len(report) != REPORT_SIZE:
        raise ValueError(f"status length {len(report)} != {REPORT_SIZE}")
    map_blob: Optional[bytes] = None
    audio_rx = audio_gap = audio_dup = audio_late = underruns = overruns = fill_ms = 0
    if expect_map:
        map_blob = bytes(report[48:64])
    else:
        packed_rx = _u32(report, 48)
        packed_dup = _u32(report, 52)
        packed_jitter = _u32(report, 56)
        audio_rx = packed_rx & 0xFFFF
        audio_gap = (packed_rx >> 16) & 0xFFFF
        audio_dup = packed_dup & 0xFFFF
        audio_late = (packed_dup >> 16) & 0xFFFF
        underruns = packed_jitter & 0xFFFF
        overruns = (packed_jitter >> 16) & 0xFFFF
        fill_ms = _u32(report, 60)
    return StatusReport(
        version=report[0],
        flags=report[1],
        last_command=report[2],
        last_result=report[3],
        ping_count=_u32(report, 4),
        alt_count=_u32(report, 8),
        enter_count=_u32(report, 12),
        all_up_count=_u32(report, 16),
        rejected_count=_u32(report, 20),
        audio_produced_frames=_u32(report, 24),
        audio_short_writes=_u32(report, 28),
        audio_usb_packets=_u32(report, 32),
        audio_usb_bytes=_u32(report, 36),
        audio_empty_packets=_u32(report, 40),
        audio_alt_transitions=_u32(report, 44),
        audio_rx=audio_rx,
        audio_gap=audio_gap,
        audio_dup=audio_dup,
        audio_late=audio_late,
        underruns=underruns,
        overruns=overruns,
        fill_ms=fill_ms,
        map_blob=map_blob,
    )


def parse_mod_key(text: str) -> tuple[int, int]:
    """Parse 'MOD:KEY' hex pair, e.g. '40:00' or '0x40:0x28'."""
    parts = text.replace("0x", "").replace("0X", "").split(":")
    if len(parts) != 2:
        raise ValueError(f"expected MOD:KEY, got {text!r}")
    modifier = int(parts[0], 16)
    keycode = int(parts[1], 16)
    if not (0 <= modifier <= 0xFF and 0 <= keycode <= 0xFF):
        raise ValueError(f"MOD:KEY out of byte range: {text!r}")
    return modifier, keycode


def format_binding(binding: KeyBinding) -> str:
    return f"{binding.modifier:02x}:{binding.keycode:02x}"


RESULT_NAMES = {
    RESULT_OK: "OK",
    RESULT_INVALID: "INVALID",
    RESULT_BUSY: "BUSY",
    RESULT_QUEUE_FULL: "QUEUE_FULL",
    RESULT_NOT_READY: "NOT_READY",
}
