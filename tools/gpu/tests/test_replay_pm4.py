#!/usr/bin/env python3
"""Tests for the offline PM4 command buffer replay harness."""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

# Try to import replay_pm4 module; this will fail until implemented.
try:
    import tools.gpu.replay_pm4 as replay_pm4
except ImportError:
    # Try adding project root
    repo_root = Path(__file__).resolve().parents[3]
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))
    try:
        import tools.gpu.replay_pm4 as replay_pm4
    except ImportError:
        replay_pm4 = None


def make_pm4_type3_packet(op: int, payload: list[int], r: int = 0) -> list[int]:
    """Construct a PM4 Type 3 packet dword array."""
    # Length in header is (total_dw - 2), where total_dw = 1 (header) + len(payload)
    total_dw = 1 + len(payload)
    len_field = (total_dw - 2) & 0x3FFF
    header = 0xC0000000 | (len_field << 16) | ((op & 0xFF) << 8) | ((r & 0x3F) << 2)
    return [header] + payload


class TestReplayPm4Unit(unittest.TestCase):
    """Unit tests for offline PM4 replay driver and binary stream parsing."""

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.test_dir = Path(self.temp_dir.name)

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_module_exists(self):
        """Verify tools.gpu.replay_pm4 module is importable."""
        self.assertIsNotNone(replay_pm4, "tools.gpu.replay_pm4 must exist")

    def test_synthetic_type3_stream_generation(self):
        """Verify synthetic PM4 binary streams can be packed with SET_SH_REG and DRAW_INDEX_AUTO."""
        # Opcode 0x76 = IT_SET_SH_REG (offset, value)
        set_sh_reg = make_pm4_type3_packet(0x76, [0x00000204, 0x12345678])
        # Opcode 0x2D = IT_DRAW_INDEX_AUTO (count, flags)
        draw_auto = make_pm4_type3_packet(0x2D, [36, 0])
        # Opcode 0x10 = IT_NOP
        nop = make_pm4_type3_packet(0x10, [0xCAFEBABE])

        all_dwords = set_sh_reg + draw_auto + nop
        raw_bytes = struct.pack(f"<{len(all_dwords)}I", *all_dwords)
        self.assertEqual(len(raw_bytes), len(all_dwords) * 4)

        bin_path = self.test_dir / "valid_stream.bin"
        bin_path.write_bytes(raw_bytes)

        # Test parsing through replay_pm4 helper
        result = replay_pm4.parse_stream_file(bin_path)
        self.assertEqual(result["dwords_count"], len(all_dwords))
        self.assertEqual(result["packets_count"], 3)
        self.assertIn("IT_SET_SH_REG", result["opcodes"])
        self.assertIn("IT_DRAW_INDEX_AUTO", result["opcodes"])
        self.assertIn("IT_NOP", result["opcodes"])

    def test_corrupted_truncated_packet_rejected(self):
        """Verify corrupted stream with truncated packet payload fails gracefully."""
        # Header claims payload of 10 dwords, but only 2 provided
        header = 0xC0000000 | ((10 - 1) << 16) | (0x76 << 8)
        truncated_dwords = [header, 0x204]
        raw_bytes = struct.pack(f"<{len(truncated_dwords)}I", *truncated_dwords)

        bin_path = self.test_dir / "corrupted_stream.bin"
        bin_path.write_bytes(raw_bytes)

        with self.assertRaises((ValueError, RuntimeError)):
            replay_pm4.parse_stream_file(bin_path)

    def test_log_format_stream_parsing(self):
        """Verify text log files with PM4 dumps can be parsed into dwords."""
        log_content = (
            "----- Buffer --- dwords: 0x00006, offset : 0, addr: 0000000000000000 -----\n"
            "00000 | 0xc0017600 | IT_SET_SH_REG (OP:0x76) SH:GX CNT:2\n"
            "      | 0x00000204 | \n"
            "      | 0x12345678 | \n"
            "00003 | 0xc0012d00 | IT_DRAW_INDEX_AUTO (OP:0x2d) SH:GX CNT:2\n"
            "      | 0x00000024 | \n"
            "      | 0x00000000 | \n"
        )
        log_path = self.test_dir / "stream.log"
        log_path.write_text(log_content, encoding="utf-8")

        result = replay_pm4.parse_stream_file(log_path)
        self.assertEqual(result["dwords_count"], 6)
        self.assertEqual(result["packets_count"], 2)

    def test_schema_header_stream(self):
        """Verify binary streams with KPM4 schema header are parsed correctly."""
        set_sh = make_pm4_type3_packet(0x76, [0x204, 0x5555])
        # Schema header: Magic b'KPM4', version=1, dword_count, flags
        magic = b"KPM4"
        version = 1
        flags = 0
        header_bytes = struct.pack("<4sIII", magic, version, len(set_sh), flags)
        payload_bytes = struct.pack(f"<{len(set_sh)}I", *set_sh)

        schema_path = self.test_dir / "schema_stream.bin"
        schema_path.write_bytes(header_bytes + payload_bytes)

        result = replay_pm4.parse_stream_file(schema_path)
        self.assertEqual(result["dwords_count"], len(set_sh))
        self.assertEqual(result["packets_count"], 1)
        self.assertEqual(result.get("schema_version"), 1)


class TestReplayPm4Cli(unittest.TestCase):
    """Integration tests running kyty_emulator --replay-pm4 if binary exists."""

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.test_dir = Path(self.temp_dir.name)
        self.repo_root = Path(__file__).resolve().parents[3]
        self.emulator_path = self.repo_root / "_Build" / "compat-foundation" / "kyty_emulator.exe"

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_emulator_replay_valid_stream(self):
        """If emulator exists, verify --replay-pm4 executes valid packet stream."""
        if not self.emulator_path.exists():
            self.skipTest(f"Emulator not found at {self.emulator_path}")

        set_sh_reg = make_pm4_type3_packet(0x76, [0x00000204, 0x12345678])
        draw_auto = make_pm4_type3_packet(0x2D, [36, 0])
        all_dwords = set_sh_reg + draw_auto
        bin_path = self.test_dir / "test_cmd.bin"
        bin_path.write_bytes(struct.pack(f"<{len(all_dwords)}I", *all_dwords))

        cmd = [str(self.emulator_path), "--replay-pm4", str(bin_path), "--headless"]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        self.assertEqual(proc.returncode, 0, f"Replay failed: stdout={proc.stdout}, stderr={proc.stderr}")
        data = json.loads(proc.stdout.strip().splitlines()[-1])
        self.assertEqual(data.get("status"), "ok")
        self.assertEqual(data.get("packets_parsed"), 2)


if __name__ == "__main__":
    unittest.main()
