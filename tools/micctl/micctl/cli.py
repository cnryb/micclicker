"""CLI entry points for micctl."""

from __future__ import annotations

import argparse
import sys

from . import protocol as p
from .device import ConfigDevice, DeviceError


def _print_status(status: p.StatusReport) -> None:
    result_name = p.RESULT_NAMES.get(status.last_result, str(status.last_result))
    print(f"version={status.version}")
    print(
        f"flags=0x{status.flags:02x} mounted={int(status.mounted)} streaming={int(status.streaming)} "
        f"paired={int(status.paired)} link_up={int(status.link_up)} config_error={int(status.config_error)}"
    )
    print(f"last_command=0x{status.last_command:02x} last_result={result_name}({status.last_result})")
    print(
        f"counts ping={status.ping_count} alt={status.alt_count} enter={status.enter_count} "
        f"all_up={status.all_up_count} rejected={status.rejected_count}"
    )
    print(
        f"audio produced={status.audio_produced_frames} short_writes={status.audio_short_writes} "
        f"usb_packets={status.audio_usb_packets} usb_bytes={status.audio_usb_bytes} "
        f"empty={status.audio_empty_packets} alt_transitions={status.audio_alt_transitions}"
    )
    print(
        f"link rx={status.audio_rx} gap={status.audio_gap} dup={status.audio_dup} "
        f"late={status.audio_late} underruns={status.underruns} overruns={status.overruns} "
        f"fill_ms={status.fill_ms}"
    )


def _print_map(key_map: p.KeyMap) -> None:
    print(f"UP={p.format_binding(key_map.up)}  # modifier:keycode")
    print(f"DOWN={p.format_binding(key_map.down)}")
    print(f"OK={p.format_binding(key_map.ok)}")


def cmd_status(_args: argparse.Namespace) -> int:
    with ConfigDevice.open() as dev:
        status = p.parse_status(dev.get_report(), expect_map=False)
        _print_status(status)
    return 0


def cmd_map_get(_args: argparse.Namespace) -> int:
    with ConfigDevice.open() as dev:
        dev.set_report(p.build_command(p.CMD_READ_MAP))
        # The GET that observes last_result=OK also consumes the map gate (bytes 48..63).
        status = dev.wait_result(p.CMD_READ_MAP, expect_map=True)
        if status.last_result != p.RESULT_OK:
            print(
                f"READ_MAP failed: {p.RESULT_NAMES.get(status.last_result, status.last_result)}",
                file=sys.stderr,
            )
            return 1
        if status.map_blob is None:
            print("READ_MAP OK but map blob missing", file=sys.stderr)
            return 1
        key_map = p.decode_map_blob(status.map_blob)
        _print_map(key_map)
    return 0


def cmd_map_set(args: argparse.Namespace) -> int:
    up_mod, up_key = p.parse_mod_key(args.up)
    down_mod, down_key = p.parse_mod_key(args.down)
    ok_mod, ok_key = p.parse_mod_key(args.ok)
    key_map = p.KeyMap(
        up=p.KeyBinding(p.KEY_ID_UP, up_mod, up_key),
        down=p.KeyBinding(p.KEY_ID_DOWN, down_mod, down_key),
        ok=p.KeyBinding(p.KEY_ID_OK, ok_mod, ok_key),
    )
    try:
        p.validate_map(key_map)
    except ValueError as exc:
        print(f"invalid map: {exc}", file=sys.stderr)
        return 2

    with ConfigDevice.open() as dev:
        dev.set_report(p.build_write_map(key_map))
        status = dev.wait_result(p.CMD_WRITE_MAP)
        if status.last_result != p.RESULT_OK:
            print(
                f"WRITE_MAP failed: {p.RESULT_NAMES.get(status.last_result, status.last_result)}",
                file=sys.stderr,
            )
            return 1
        print("WRITE_MAP OK")
        _print_map(key_map)
    return 0


def cmd_map_reset(_args: argparse.Namespace) -> int:
    with ConfigDevice.open() as dev:
        dev.set_report(p.build_command(p.CMD_RESET_MAP))
        status = dev.wait_result(p.CMD_RESET_MAP)
        if status.last_result != p.RESULT_OK:
            print(
                f"RESET_MAP failed: {p.RESULT_NAMES.get(status.last_result, status.last_result)}",
                file=sys.stderr,
            )
            return 1
        print("RESET_MAP OK")
        _print_map(p.default_map())
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="micctl", description="MicClicker config HID CLI")
    sub = parser.add_subparsers(dest="command", required=True)

    status = sub.add_parser("status", help="GET status report")
    status.set_defaults(func=cmd_status)

    map_parser = sub.add_parser("map", help="key mapping commands")
    map_sub = map_parser.add_subparsers(dest="map_command", required=True)

    map_get = map_sub.add_parser("get", help="read mapping via READ_MAP gate")
    map_get.set_defaults(func=cmd_map_get)

    map_set = map_sub.add_parser("set", help="write mapping (MOD:KEY hex pairs)")
    map_set.add_argument("--up", required=True, help="UP binding, e.g. 40:00")
    map_set.add_argument("--down", required=True, help="DOWN binding, e.g. 00:00")
    map_set.add_argument("--ok", required=True, help="OK binding, e.g. 00:28")
    map_set.set_defaults(func=cmd_map_set)

    map_reset = map_sub.add_parser("reset", help="restore default mapping")
    map_reset.set_defaults(func=cmd_map_reset)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except DeviceError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
