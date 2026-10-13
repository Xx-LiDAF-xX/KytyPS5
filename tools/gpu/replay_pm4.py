#!/usr/bin/env python3
"""Offline PM4 command buffer replay and analysis harness."""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

OPCODE_NAMES = {
    0x10: "IT_NOP",
    0x11: "IT_SET_BASE",
    0x12: "IT_CLEAR_STATE",
    0x13: "IT_INDEX_BUFFER_SIZE",
    0x15: "IT_DISPATCH_DIRECT",
    0x16: "IT_DISPATCH_INDIRECT",
    0x20: "IT_SET_PREDICATION",
    0x22: "IT_COND_EXEC",
    0x24: "IT_DRAW_INDIRECT",
    0x25: "IT_DRAW_INDEX_INDIRECT",
    0x26: "IT_INDEX_BASE",
    0x27: "IT_DRAW_INDEX_2",
    0x28: "IT_CONTEXT_CONTROL",
    0x2A: "IT_INDEX_TYPE",
    0x2C: "IT_DRAW_INDIRECT_MULTI",
    0x2D: "IT_DRAW_INDEX_AUTO",
    0x2F: "IT_NUM_INSTANCES",
    0x33: "IT_INDIRECT_BUFFER_CNST",
    0x35: "IT_DRAW_INDEX_OFFSET_2",
    0x37: "IT_WRITE_DATA",
    0x38: "IT_DRAW_INDEX_INDIRECT_MULTI",
    0x39: "IT_MEM_SEMAPHORE",
    0x3A: "IT_DISPATCH_DRAW_PREAMBLE",
    0x3C: "IT_WAIT_REG_MEM",
    0x3F: "IT_INDIRECT_BUFFER",
    0x40: "IT_COPY_DATA",
    0x41: "IT_CP_DMA",
    0x42: "IT_PFP_SYNC_ME",
    0x43: "IT_SURFACE_SYNC",
    0x46: "IT_EVENT_WRITE",
    0x47: "IT_EVENT_WRITE_EOP",
    0x48: "IT_EVENT_WRITE_EOS",
    0x49: "IT_RELEASE_MEM",
    0x50: "IT_DMA_DATA",
    0x58: "IT_ACQUIRE_MEM",
    0x59: "IT_REWIND",
    0x63: "IT_SET_SH_REG_INDIRECT",
    0x64: "IT_SET_UCONFIG_REG_INDIRECT",
    0x68: "IT_SET_CONFIG_REG",
    0x69: "IT_SET_CONTEXT_REG",
    0x76: "IT_SET_SH_REG",
    0x78: "IT_SET_QUEUE_REG",
    0x79: "IT_SET_UCONFIG_REG",
    0x7A: "IT_SET_UCONFIG_REG_INDEX",
    0x81: "IT_WRITE_CONST_RAM",
    0x83: "IT_DUMP_CONST_RAM",
    0x84: "IT_INCREMENT_CE_COUNTER",
    0x85: "IT_INCREMENT_DE_COUNTER",
    0x86: "IT_WAIT_ON_CE_COUNTER",
    0x88: "IT_WAIT_ON_DE_COUNTER_DIFF",
    0x8D: "IT_DISPATCH_DRAW",
    0x8E: "IT_GET_LOD_STATS",
    0x93: "IT_WAIT_REG_MEM_64",
    0x9F: "IT_SET_CONTEXT_REG_INDIRECT",
}


def parse_dwords_from_log(text: str) -> List[int]:
    """Extract dword values from a PM4 text dump log."""
    dwords = []
    # Match patterns like: "00000 | 0xc0017600 |" or "      | 0x00000204 |"
    pattern = re.compile(r"\|\s*(0x[0-9a-fA-F]{8})\s*\|")
    for line in text.splitlines():
        match = pattern.search(line)
        if match:
            dwords.append(int(match.group(1), 16))
    return dwords


def parse_stream_file(file_path: Path | str) -> Dict[str, Any]:
    """Parse a PM4 command stream from a binary or log file and validate packet structure."""
    path = Path(file_path)
    if not path.is_file():
        raise FileNotFoundError(f"Stream file not found: {path}")

    raw_bytes = path.read_bytes()
    schema_version = None

    if path.suffix.lower() == ".log":
        try:
            text = raw_bytes.decode("utf-8")
        except UnicodeDecodeError:
            text = raw_bytes.decode("latin1")
        dwords = parse_dwords_from_log(text)
    else:
        # Check for KPM4 schema header
        if len(raw_bytes) >= 16 and raw_bytes[:4] == b"KPM4":
            magic, schema_version, dword_count, flags = struct.unpack("<4sIII", raw_bytes[:16])
            payload = raw_bytes[16:]
            if len(payload) < dword_count * 4:
                raise ValueError(
                    f"Corrupted KPM4 stream: header declares {dword_count} dwords, but only {len(payload)//4} available"
                )
            dwords = list(struct.unpack(f"<{dword_count}I", payload[: dword_count * 4]))
        else:
            if len(raw_bytes) % 4 != 0:
                raise ValueError(f"Binary PM4 file size {len(raw_bytes)} is not a multiple of 4")
            count = len(raw_bytes) // 4
            dwords = list(struct.unpack(f"<{count}I", raw_bytes))

    # Validate PM4 packets in dwords
    idx = 0
    total = len(dwords)
    packets_count = 0
    opcodes = []
    packet_details = []

    while idx < total:
        header = dwords[idx]
        pkt_type = header >> 30

        if header == 0x80000000 or (header & 0x3FFFFF00) == 0x3FFF1000:
            # 1-dword packet
            pkt_len = 1
            op = 0x10  # NOP
            opcodes.append(OPCODE_NAMES.get(op, f"0x{op:02X}"))
            packets_count += 1
            idx += 1
            continue

        if pkt_type == 3:  # Type 3
            # Length in header is (packet_dw - 2)
            len_field = (header >> 16) & 0x3FFF
            pkt_dw = len_field + 2
            op = (header >> 8) & 0xFF

            if idx + pkt_dw > total:
                raise ValueError(
                    f"Truncated packet at dword {idx}: header 0x{header:08X} requires {pkt_dw} dwords, only {total - idx} left"
                )

            op_name = OPCODE_NAMES.get(op, f"OP_0x{op:02X}")
            opcodes.append(op_name)
            packet_details.append({
                "offset": idx,
                "header": header,
                "opcode": op,
                "name": op_name,
                "length_dw": pkt_dw,
            })
            packets_count += 1
            idx += pkt_dw
        elif pkt_type == 2:  # Type 2 NOP
            packets_count += 1
            idx += 1
        elif pkt_type == 0:  # Type 0
            count = ((header >> 16) & 0x3FFF) + 1
            if idx + 1 + count > total:
                raise ValueError(f"Truncated Type0 packet at {idx}")
            packets_count += 1
            idx += 1 + count
        else:
            raise ValueError(f"Unsupported or invalid PM4 packet type {pkt_type} at dword {idx} (header: 0x{header:08X})")

    result = {
        "status": "ok",
        "dwords_count": total,
        "packets_count": packets_count,
        "opcodes": opcodes,
        "packets": packet_details,
    }
    if schema_version is not None:
        result["schema_version"] = schema_version

    return result


def replay_stream(
    emulator_path: Path | str,
    input_path: Path | str,
    headless: bool = True,
    timeout_sec: float = 30.0,
) -> Dict[str, Any]:
    """Execute PM4 packet stream through kyty_emulator --replay-pm4."""
    emu = Path(emulator_path)
    if not emu.is_file():
        raise FileNotFoundError(f"Emulator binary not found: {emu}")

    inp = Path(input_path)
    if not inp.is_file():
        raise FileNotFoundError(f"Input stream not found: {inp}")

    cmd = [str(emu), "--replay-pm4", str(inp)]
    if headless:
        cmd.append("--headless")

    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout_sec,
    )

    stdout_text = proc.stdout.strip()
    json_data = {}
    for line in stdout_text.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                json_data = json.loads(line)
                break
            except json.JSONDecodeError:
                pass

    if proc.returncode != 0:
        err_msg = json_data.get("error", proc.stderr.strip() or f"Process exited with code {proc.returncode}")
        raise RuntimeError(f"Emulator PM4 replay failed (code {proc.returncode}): {err_msg}")

    return json_data


def main():
    parser = argparse.ArgumentParser(description="Offline PM4 command buffer replay harness.")
    parser.add_argument("--emulator", type=str, help="Path to kyty_emulator executable")
    parser.add_argument("--input", type=str, required=True, help="Input PM4 stream file (.bin or .log)")
    parser.add_argument("--headless", action="store_true", default=True, help="Run headless without presentation")
    parser.add_argument("--parse-only", action="store_true", help="Parse and validate stream without emulator")

    args = parser.parse_args()

    try:
        parsed = parse_stream_file(args.input)
        if args.parse_only or not args.emulator:
            print(json.dumps(parsed, indent=2))
            return 0

        res = replay_stream(args.emulator, args.input, headless=args.headless)
        print(json.dumps(res, indent=2))
        return 0
    except Exception as e:
        print(json.dumps({"status": "error", "error": str(e)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
