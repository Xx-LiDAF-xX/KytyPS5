#!/usr/bin/env python3
"""Reduce guest shader bytecode and synthesize sanitized C++ regression tests."""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path
from typing import Callable, Sequence


S_NOP_0 = 0xBF800000

_STAGES = {
    "cs": ("cs", "Compute"),
    "compute": ("cs", "Compute"),
    "vs": ("vs", "Vertex"),
    "vertex": ("vs", "Vertex"),
    "ps": ("ps", "Pixel"),
    "pixel": ("ps", "Pixel"),
}

FailureOracle = Callable[[Sequence[int]], bool]


def canonicalize_stage(stage: str) -> tuple[str, str]:
    """Return the short stage name and matching ``ShaderType`` enumerator."""
    normalized = stage.strip().lower()
    try:
        return _STAGES[normalized]
    except KeyError as exc:
        raise ValueError(
            f"Invalid shader stage '{stage}'. Expected one of: cs, compute, vs, vertex, ps, pixel"
        ) from exc


def bytes_to_dwords(data: bytes) -> list[int]:
    """Decode little-endian shader bytecode into complete 32-bit dwords."""
    if len(data) % 4 != 0:
        raise ValueError(
            f"Shader bytecode size {len(data)} is not a multiple of 4 bytes"
        )
    if not data:
        return []
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def dwords_to_bytes(dwords: Sequence[int]) -> bytes:
    """Encode dwords for a reduction oracle without leaking source metadata."""
    if not dwords:
        return b""
    return struct.pack(f"<{len(dwords)}I", *(word & 0xFFFFFFFF for word in dwords))


def strip_trailing_nops(dwords: Sequence[int]) -> list[int]:
    """Remove only unambiguous trailing ``s_nop 0`` instructions."""
    end = len(dwords)
    while end and dwords[end - 1] == S_NOP_0:
        end -= 1
    return list(dwords[:end])


def minimize_dwords(
    dwords: Sequence[int], preserves_failure: FailureOracle | None = None
) -> list[int]:
    """Minimize dwords while retaining a caller-defined failure condition.

    The unconditional pass removes trailing ``s_nop 0`` instructions. More
    aggressive delta debugging is performed only when an oracle is supplied;
    every accepted candidate must still satisfy that oracle.
    """
    current = strip_trailing_nops(dwords)
    if preserves_failure is None:
        return current
    if not preserves_failure(current):
        raise ValueError("Initial shader does not satisfy the failure oracle")

    granularity = 2
    while len(current) >= 2:
        chunk_size = (len(current) + granularity - 1) // granularity
        reduced = False

        for start in range(0, len(current), chunk_size):
            candidate = current[:start] + current[start + chunk_size :]
            candidate = strip_trailing_nops(candidate)
            if not candidate:
                continue
            if preserves_failure(candidate):
                current = candidate
                granularity = max(2, granularity - 1)
                reduced = True
                break

        if reduced:
            continue
        if granularity >= len(current):
            break
        granularity = min(len(current), granularity * 2)

    return current


def _validate_identifier(identifier: str) -> str:
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", identifier) is None:
        raise ValueError(f"Invalid C++ identifier: {identifier!r}")
    return identifier


def _format_dwords(dwords: Sequence[int], words_per_line: int = 6) -> str:
    lines = []
    for start in range(0, len(dwords), words_per_line):
        words = ", ".join(
            f"0x{word & 0xFFFFFFFF:08x}u"
            for word in dwords[start : start + words_per_line]
        )
        lines.append(f"      {words},")
    return "\n".join(lines)


def generate_cpp_test(
    dwords: Sequence[int],
    stage: str,
    test_name: str = "CheckReducedShader",
    expected_opcode: int = 14,
) -> str:
    """Create a metadata-free test body for ``tests/shaderCfgTests.cpp``."""
    if not dwords:
        raise ValueError("Cannot synthesize a test from empty shader bytecode")
    _, shader_type = canonicalize_stage(stage)
    identifier = _validate_identifier(test_name)
    if not 0 <= expected_opcode <= 0xFFFF:
        raise ValueError("SPIR-V opcode must be between 0 and 65535")

    formatted = _format_dwords(dwords)
    return (
        f"void {identifier}() {{\n"
        "  const uint32_t code[] = {\n"
        f"{formatted}\n"
        "  };\n"
        f"  const auto options = MakeCompileOptions(ShaderType::{shader_type});\n"
        "  const auto result = RecompileForTest(code, options);\n"
        "  Check(!result.spirv.empty(), \"reduced shader emitted no SPIR-V\");\n"
        "  CheckSpirvBinaryValidates(result.spirv);\n"
        f"  Check(SpirvContainsOpcode(result.spirv, {expected_opcode}u),\n"
        "        \"reduced shader omitted the expected SPIR-V opcode\");\n"
        "}\n"
    )


def _parse_int(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer value: {value!r}") from exc


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Reduce guest shader bytecode and synthesize a sanitized C++ regression test."
    )
    parser.add_argument("--input", "-i", type=Path, required=True)
    parser.add_argument("--stage", required=True, help="Shader stage (cs, vs, ps)")
    parser.add_argument("--output", "-o", type=Path, required=True)
    parser.add_argument(
        "--minimize",
        action="store_true",
        help="Remove safe trailing no-ops before generating the test",
    )
    parser.add_argument("--test-name", default="CheckReducedShader")
    parser.add_argument(
        "--expected-opcode",
        type=_parse_int,
        default=14,
        help="SPIR-V opcode the generated test must contain (default: OpMemoryModel)",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        dwords = bytes_to_dwords(args.input.read_bytes())
        input_dwords = len(dwords)
        if args.minimize:
            dwords = minimize_dwords(dwords)
        source = generate_cpp_test(
            dwords,
            stage=args.stage,
            test_name=args.test_name,
            expected_opcode=args.expected_opcode,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(source, encoding="utf-8", newline="\n")
        print(
            json.dumps(
                {
                    "status": "ok",
                    "stage": canonicalize_stage(args.stage)[0],
                    "input_dwords": input_dwords,
                    "output_dwords": len(dwords),
                }
            )
        )
        return 0
    except (OSError, ValueError) as exc:
        print(json.dumps({"status": "error", "error": str(exc)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
