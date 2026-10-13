#!/usr/bin/env python3
"""Standalone shader recompilation CLI runner and analysis tool."""

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any, Sequence

VALID_STAGES = {
    "cs": "cs",
    "compute": "cs",
    "vs": "vs",
    "vertex": "vs",
    "ps": "ps",
    "pixel": "ps",
}


def canonicalize_stage(stage: str) -> str:
    normalized = stage.strip().lower()
    if normalized not in VALID_STAGES:
        raise ValueError(
            f"Invalid shader stage '{stage}'. Expected one of: cs, compute, vs, vertex, ps, pixel"
        )
    return VALID_STAGES[normalized]


def run_recompile(
    emulator: str | Path,
    stage: str,
    input_path: str | Path,
    output_path: str | Path | None = None,
    disasm: bool = False,
    validate: bool = True,
    extra_args: Sequence[str] | None = None,
) -> dict[str, Any]:
    canon_stage = canonicalize_stage(stage)
    in_file = Path(input_path).resolve()
    if not in_file.exists():
        raise FileNotFoundError(f"Shader input file not found: {in_file}")

    cmd: list[str] = [str(emulator)]
    if extra_args:
        cmd.extend(extra_args)

    cmd.extend(["--recompile-shader", canon_stage, str(in_file)])

    if output_path is not None:
        out_file = Path(output_path).resolve()
        out_file.parent.mkdir(parents=True, exist_ok=True)
        cmd.extend(["--out", str(out_file)])

    if disasm:
        cmd.append("--disasm")

    if validate:
        cmd.append("--validate")

    proc = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    if proc.returncode != 0:
        error_msg = proc.stderr.strip() or proc.stdout.strip() or f"exited with code {proc.returncode}"
        raise RuntimeError(f"Shader recompilation failed: {error_msg}")

    # Parse JSON output from stdout
    output_text = proc.stdout.strip()
    try:
        # Find json object in stdout
        start_idx = output_text.find("{")
        end_idx = output_text.rfind("}")
        if start_idx != -1 and end_idx != -1:
            return json.loads(output_text[start_idx : end_idx + 1])
        return {"status": "ok", "raw_output": output_text}
    except json.JSONDecodeError:
        return {"status": "ok", "raw_output": output_text}


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Recompile PS5 guest shaders to SPIR-V using KytyPS5 emulator engine."
    )
    parser.add_argument("--emulator", type=Path, default="_Build/compat-foundation/kyty_emulator.exe")
    parser.add_argument("--stage", required=True, help="Shader stage (cs, vs, ps)")
    parser.add_argument("--input", "-i", type=Path, required=True, help="Input guest shader binary (.bin)")
    parser.add_argument("--output", "-o", type=Path, help="Output SPIR-V binary (.spv)")
    parser.add_argument("--disasm", action="store_true", help="Print disassembly")
    parser.add_argument("--no-validate", action="store_true", help="Skip SPIR-V validation")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        result = run_recompile(
            emulator=args.emulator,
            stage=args.stage,
            input_path=args.input,
            output_path=args.output,
            disasm=args.disasm,
            validate=not args.no_validate,
        )
        print(json.dumps(result, indent=2))
        return 0
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
