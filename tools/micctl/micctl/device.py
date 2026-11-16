"""Open the MicClicker vendor config HID interface via hidapi."""

from __future__ import annotations

import time
from typing import Any, Optional

from . import protocol as p

# TinyUSB TUD_HID_REPORT_DESC_GENERIC_INOUT uses vendor page 0xFF00 / usage 0x01.
CONFIG_USAGE_PAGE = 0xFF00
CONFIG_USAGE = 0x01


class DeviceError(RuntimeError):
    pass


def _import_hid():
    try:
        import hid  # type: ignore
    except ImportError as exc:
        raise DeviceError(
            "missing dependency 'hid' (pip install -r tools/micctl/requirements.txt)"
        ) from exc
    return hid


def _path_str(path: Any) -> str:
    if isinstance(path, bytes):
        return path.decode("utf-8", "replace")
    return str(path)


def list_config_paths(vid: int = p.VID, pid: int = p.PID) -> list[bytes]:
    hid = _import_hid()
    paths: list[bytes] = []
    for info in hid.enumerate(vid, pid):
        usage_page = int(info.get("usage_page") or 0)
        usage = int(info.get("usage") or 0)
        if usage_page == CONFIG_USAGE_PAGE and usage == CONFIG_USAGE:
            path = info.get("path")
            if path is None:
                continue
            paths.append(path if isinstance(path, bytes) else str(path).encode("utf-8"))
    return paths


class ConfigDevice:
    def __init__(self, handle):
        self._dev = handle

    @classmethod
    def open(cls, vid: int = p.VID, pid: int = p.PID) -> "ConfigDevice":
        hid = _import_hid()
        paths = list_config_paths(vid, pid)
        if not paths:
            raise DeviceError(
                f"no config HID interface for {vid:04x}:{pid:04x} "
                f"(usage_page=0x{CONFIG_USAGE_PAGE:04x} usage=0x{CONFIG_USAGE:02x})"
            )
        path = paths[0]
        try:
            dev = hid.device()
            dev.open_path(path)
        except Exception as exc:  # noqa: BLE001 - surface hidapi errors as DeviceError
            raise DeviceError(f"failed to open config HID ({_path_str(path)}): {exc}") from exc
        return cls(dev)

    def close(self) -> None:
        try:
            self._dev.close()
        except Exception:  # noqa: BLE001
            pass

    def __enter__(self) -> "ConfigDevice":
        return self

    def __exit__(self, *args) -> None:
        self.close()

    def get_report(self) -> bytes:
        # Report ID 0: get_input_report length includes the report-id byte on hidapi.
        raw = self._dev.get_input_report(0, p.REPORT_SIZE + 1)
        data = bytes(raw)
        if len(data) == p.REPORT_SIZE + 1 and data[0] == 0:
            return data[1:]
        if len(data) == p.REPORT_SIZE:
            return data
        if len(data) > p.REPORT_SIZE:
            if data[0] == 0:
                return data[1 : 1 + p.REPORT_SIZE]
            return data[: p.REPORT_SIZE]
        raise DeviceError(f"short GET_REPORT: got {len(data)} bytes")

    def set_report(self, frame: bytes) -> None:
        if len(frame) != p.REPORT_SIZE:
            raise DeviceError(f"SET_REPORT length {len(frame)} != {p.REPORT_SIZE}")
        # hidapi write() expects report-id as the first byte.
        written = self._dev.write(b"\x00" + frame)
        if written is not None and written < 1:
            raise DeviceError("SET_REPORT write returned 0")

    def wait_result(
        self,
        command: int,
        *,
        timeout_s: float = 2.0,
        poll_s: float = 0.02,
        expect_map: bool = False,
    ) -> p.StatusReport:
        deadline = time.monotonic() + timeout_s
        last: Optional[p.StatusReport] = None
        while time.monotonic() < deadline:
            raw = self.get_report()
            # While BUSY the firmware keeps the READ_MAP gate; parse as counters so a
            # premature expect_map decode cannot treat link words as a map blob.
            busy = len(raw) == p.REPORT_SIZE and raw[2] == command and raw[3] == p.RESULT_BUSY
            last = p.parse_status(raw, expect_map=expect_map and not busy)
            if last.last_command == command and last.last_result != p.RESULT_BUSY:
                return last
            time.sleep(poll_s)
        if last is None:
            raise DeviceError("no status report received")
        raise DeviceError(
            f"timeout waiting for command 0x{command:02x} "
            f"(last_command=0x{last.last_command:02x} result={last.last_result})"
        )
