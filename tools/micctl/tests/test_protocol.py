import unittest

from micctl import protocol as p


class MapBlobTests(unittest.TestCase):
    def test_defaults_round_trip(self):
        blob = p.encode_map_blob(p.default_map())
        self.assertEqual(len(blob), p.MAP_BLOB_SIZE)
        self.assertEqual(int.from_bytes(blob[0:4], "little"), 1)
        decoded = p.decode_map_blob(blob)
        self.assertEqual(decoded.up.modifier, p.MOD_RIGHT_ALT)
        self.assertEqual(decoded.up.keycode, p.KEYCODE_NONE)
        self.assertEqual(decoded.down.modifier, p.MOD_RIGHT_CTRL)
        self.assertEqual(decoded.down.keycode, p.KEYCODE_NONE)
        self.assertEqual(decoded.ok.modifier, 0)
        self.assertEqual(decoded.ok.keycode, p.KEYCODE_ENTER)

    def test_rejects_reserved_keycode(self):
        bad = p.KeyMap(
            up=p.KeyBinding(p.KEY_ID_UP, 0, 0x01),
            down=p.KeyBinding(p.KEY_ID_DOWN, 0, 0),
            ok=p.KeyBinding(p.KEY_ID_OK, 0, p.KEYCODE_ENTER),
        )
        with self.assertRaises(ValueError):
            p.encode_map_blob(bad)

    def test_rejects_bad_version(self):
        blob = bytearray(p.encode_map_blob(p.default_map()))
        blob[0] = 2
        with self.assertRaises(ValueError):
            p.decode_map_blob(bytes(blob))

    def test_parse_mod_key(self):
        self.assertEqual(p.parse_mod_key("40:00"), (0x40, 0x00))
        self.assertEqual(p.parse_mod_key("0x00:0x28"), (0x00, 0x28))
        with self.assertRaises(ValueError):
            p.parse_mod_key("40")


class StatusReportTests(unittest.TestCase):
    def _blank(self) -> bytearray:
        return bytearray(p.REPORT_SIZE)

    def test_parse_counters_and_flags(self):
        raw = self._blank()
        raw[0] = 1
        raw[1] = p.FLAG_MOUNTED | p.FLAG_PAIRED | p.FLAG_LINK_UP
        raw[2] = p.CMD_PING
        raw[3] = p.RESULT_OK
        raw[4:8] = (7).to_bytes(4, "little")
        raw[48:52] = (5 | (9 << 16)).to_bytes(4, "little")
        raw[52:56] = (2 | (3 << 16)).to_bytes(4, "little")
        raw[56:60] = (1 | (4 << 16)).to_bytes(4, "little")
        raw[60:64] = (42).to_bytes(4, "little")
        status = p.parse_status(bytes(raw), expect_map=False)
        self.assertTrue(status.mounted)
        self.assertTrue(status.paired)
        self.assertTrue(status.link_up)
        self.assertEqual(status.ping_count, 7)
        self.assertEqual(status.audio_rx, 5)
        self.assertEqual(status.audio_gap, 9)
        self.assertEqual(status.audio_dup, 2)
        self.assertEqual(status.audio_late, 3)
        self.assertEqual(status.underruns, 1)
        self.assertEqual(status.overruns, 4)
        self.assertEqual(status.fill_ms, 42)
        self.assertIsNone(status.map_blob)

    def test_parse_map_overlay(self):
        raw = self._blank()
        raw[0] = 1
        raw[2] = p.CMD_READ_MAP
        raw[3] = p.RESULT_OK
        blob = p.encode_map_blob(p.default_map())
        raw[48:64] = blob
        status = p.parse_status(bytes(raw), expect_map=True)
        self.assertEqual(status.map_blob, blob)
        self.assertEqual(p.decode_map_blob(status.map_blob), p.default_map())


class CommandFrameTests(unittest.TestCase):
    def test_build_write_map(self):
        frame = p.build_write_map(p.default_map())
        self.assertEqual(len(frame), p.REPORT_SIZE)
        self.assertEqual(frame[0], p.CMD_WRITE_MAP)
        self.assertEqual(frame[1:17], p.encode_map_blob(p.default_map()))
        self.assertEqual(frame[17:], bytes(p.REPORT_SIZE - 17))

    def test_build_read_map(self):
        frame = p.build_command(p.CMD_READ_MAP)
        self.assertEqual(frame[0], p.CMD_READ_MAP)
        self.assertEqual(frame[1:], bytes(p.REPORT_SIZE - 1))


if __name__ == "__main__":
    unittest.main()
