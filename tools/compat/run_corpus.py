#!/usr/bin/env python3
"""Run a local KytyPS5 compatibility corpus without exposing private paths."""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


TITLE_ID_PATTERN = re.compile(r"^PPSA\d{5}$")
FINAL_OUTCOMES = {
    "completed",
    "fatal",
    "host_exception",
    "timeout",
    "launch_error",
}
RESERVED_ARGUMENTS = {"--game", "--game-patch", "--diagnostics-output"}
MANIFEST_FIELDS = {"schema_version", "titles"}
ENTRY_FIELDS = {
    "title_id",
    "game_path",
    "patch_path",
    "timeout_seconds",
    "repeat",
    "arguments",
}


class ManifestError(ValueError):
    """Raised when a local corpus manifest is invalid."""


@dataclass(frozen=True)
class CorpusEntry:
    title_id: str
    game_path: Path
    patch_path: Path | None
    timeout_seconds: float
    repeat: int
    arguments: tuple[str, ...]


@dataclass(frozen=True)
class CorpusManifest:
    entries: tuple[CorpusEntry, ...]


def _require_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ManifestError(f"{field} must be a number")
    number = float(value)
    if number <= 0:
        raise ManifestError(f"{field} must be greater than zero")
    return number


def _resolve_existing_path(base: Path, value: Any, field: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ManifestError(f"{field} must be a non-empty string")
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = base / path
    path = path.resolve()
    if not path.exists():
        raise ManifestError(f"{field} does not exist")
    return path


def load_manifest(path: Path | str) -> CorpusManifest:
    manifest_path = Path(path).resolve()
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"could not read manifest: {error}") from error

    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise ManifestError("manifest schema_version must be 1")
    unknown_manifest_fields = set(document) - MANIFEST_FIELDS
    if unknown_manifest_fields:
        raise ManifestError(
            f"unknown manifest field: {sorted(unknown_manifest_fields)[0]}"
        )
    titles = document.get("titles")
    if not isinstance(titles, list) or not titles:
        raise ManifestError("manifest titles must be a non-empty array")

    entries: list[CorpusEntry] = []
    seen_ids: set[str] = set()
    for index, raw_entry in enumerate(titles):
        prefix = f"titles[{index}]"
        if not isinstance(raw_entry, dict):
            raise ManifestError(f"{prefix} must be an object")
        unknown_entry_fields = set(raw_entry) - ENTRY_FIELDS
        if unknown_entry_fields:
            raise ManifestError(
                f"unknown {prefix} field: {sorted(unknown_entry_fields)[0]}"
            )

        title_id = raw_entry.get("title_id")
        if not isinstance(title_id, str) or not TITLE_ID_PATTERN.fullmatch(title_id):
            raise ManifestError(f"{prefix}.title_id must match PPSA followed by five digits")
        if title_id in seen_ids:
            raise ManifestError(f"duplicate title_id: {title_id}")
        seen_ids.add(title_id)

        game_path = _resolve_existing_path(
            manifest_path.parent, raw_entry.get("game_path"), f"{prefix}.game_path"
        )
        patch_value = raw_entry.get("patch_path")
        patch_path = None
        if "patch_path" in raw_entry and patch_value is None:
            raise ManifestError(f"{prefix}.patch_path must be a non-empty string")
        if patch_value is not None:
            patch_path = _resolve_existing_path(
                manifest_path.parent, patch_value, f"{prefix}.patch_path"
            )
            if not patch_path.is_file():
                raise ManifestError(f"{prefix}.patch_path must be a file")

        timeout_seconds = _require_number(
            raw_entry.get("timeout_seconds", 120), f"{prefix}.timeout_seconds"
        )
        repeat = raw_entry.get("repeat", 1)
        if isinstance(repeat, bool) or not isinstance(repeat, int) or not 1 <= repeat <= 100:
            raise ManifestError(f"{prefix}.repeat must be an integer from 1 to 100")

        arguments = raw_entry.get("arguments", [])
        if not isinstance(arguments, list) or not all(
            isinstance(argument, str) for argument in arguments
        ):
            raise ManifestError(f"{prefix}.arguments must be an array of strings")
        if RESERVED_ARGUMENTS.intersection(arguments):
            raise ManifestError(
                f"{prefix}.arguments cannot override game, patch, or diagnostics paths"
            )

        entries.append(
            CorpusEntry(
                title_id=title_id,
                game_path=game_path,
                patch_path=patch_path,
                timeout_seconds=timeout_seconds,
                repeat=repeat,
                arguments=tuple(arguments),
            )
        )

    return CorpusManifest(tuple(entries))


def _emulator_command(emulator: Path) -> list[str]:
    if emulator.suffix.casefold() == ".py":
        return [sys.executable, str(emulator)]
    return [str(emulator)]


def _next_attempt_number(title_directory: Path) -> int:
    highest = 0
    if title_directory.is_dir():
        for child in title_directory.iterdir():
            match = re.fullmatch(r"attempt-(\d{3,})", child.name)
            if match:
                highest = max(highest, int(match.group(1)))
    return highest + 1


def _write_json(path: Path, document: Any) -> None:
    path.write_text(
        json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def _public_command(
    command: Sequence[str],
    emulator: Path,
    entry: CorpusEntry,
    diagnostics: Path,
) -> list[str]:
    replacements = {
        str(emulator): "<emulator>",
        str(entry.game_path): f"<game:{entry.title_id}>",
        str(diagnostics): "<diagnostics>",
    }
    if entry.patch_path is not None:
        replacements[str(entry.patch_path)] = f"<patch:{entry.title_id}>"
    public: list[str] = []
    for argument in command:
        sanitized = replacements.get(argument)
        if sanitized is None:
            path_value = argument.split("=", 1)[-1]
            looks_absolute = Path(path_value).is_absolute() or bool(
                re.match(r"^[A-Za-z]:[\\/]", path_value)
            )
            if looks_absolute:
                sanitized = (
                    f"{argument.split('=', 1)[0]}=<path>"
                    if "=" in argument
                    else "<path>"
                )
            else:
                sanitized = argument
        public.append(sanitized)
    if emulator.suffix.casefold() == ".py" and public:
        public[0] = "<python>"
    return public


def _terminate_process_tree(process: subprocess.Popen[bytes]) -> None:
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if process.poll() is None:
            process.kill()
        return

    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=2)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def _read_report_outcome(path: Path) -> str | None:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    outcome = document.get("outcome") if isinstance(document, dict) else None
    return outcome if outcome in FINAL_OUTCOMES else None


def _run_attempt(
    emulator: Path,
    entry: CorpusEntry,
    attempt_number: int,
    attempt_directory: Path,
) -> dict[str, Any]:
    attempt_directory.mkdir(parents=True, exist_ok=False)
    diagnostics = attempt_directory / "diagnostics.json"
    command = _emulator_command(emulator)
    command.extend(["--game", str(entry.game_path)])
    if entry.patch_path is not None:
        command.extend(["--game-patch", str(entry.patch_path)])
    command.extend(["--diagnostics-output", str(diagnostics)])
    command.extend(entry.arguments)

    _write_json(
        attempt_directory / "command.json",
        {
            "schema_version": 1,
            "title_id": entry.title_id,
            "attempt": attempt_number,
            "timeout_seconds": entry.timeout_seconds,
            "command": _public_command(command, emulator, entry, diagnostics),
        },
    )

    creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    started = time.monotonic()
    timed_out = False
    exit_code: int | None = None
    stdout = b""
    stderr = b""
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=creation_flags,
            start_new_session=os.name != "nt",
        )
        try:
            stdout, stderr = process.communicate(timeout=entry.timeout_seconds)
            exit_code = process.returncode
        except subprocess.TimeoutExpired as error:
            timed_out = True
            stdout = error.output or b""
            stderr = error.stderr or b""
            _terminate_process_tree(process)
            remaining_stdout, remaining_stderr = process.communicate()
            stdout += remaining_stdout or b""
            stderr += remaining_stderr or b""
    except OSError as error:
        stderr = str(error).encode("utf-8", errors="replace")

    duration = time.monotonic() - started
    (attempt_directory / "stdout.txt").write_bytes(stdout)
    (attempt_directory / "stderr.txt").write_bytes(stderr)

    report_outcome = _read_report_outcome(diagnostics)
    if timed_out:
        outcome = "timeout"
        exit_code = None
    elif report_outcome is not None:
        outcome = report_outcome
    elif exit_code == 0:
        outcome = "completed"
    else:
        outcome = "launch_error"

    result = {
        "schema_version": 1,
        "title_id": entry.title_id,
        "attempt": attempt_number,
        "outcome": outcome,
        "exit_code": exit_code,
        "duration_seconds": round(duration, 6),
        "diagnostics_present": diagnostics.is_file(),
    }
    _write_json(attempt_directory / "run.json", result)
    return result


def run_corpus(
    manifest_path: Path | str,
    emulator_path: Path | str,
    output_path: Path | str,
    title_id: str | None = None,
) -> list[dict[str, Any]]:
    manifest = load_manifest(manifest_path)
    emulator = Path(emulator_path).resolve()
    if not emulator.is_file():
        raise ManifestError("emulator does not exist or is not a file")
    if title_id is not None and not TITLE_ID_PATTERN.fullmatch(title_id):
        raise ManifestError("--title-id must match PPSA followed by five digits")

    selected = [
        entry for entry in manifest.entries if title_id is None or entry.title_id == title_id
    ]
    if not selected:
        raise ManifestError(f"title_id is not present in the manifest: {title_id}")

    output = Path(output_path).resolve()
    output.mkdir(parents=True, exist_ok=True)
    results: list[dict[str, Any]] = []
    for entry in selected:
        title_directory = output / entry.title_id
        attempt_number = _next_attempt_number(title_directory)
        for _ in range(entry.repeat):
            attempt_directory = title_directory / f"attempt-{attempt_number:03d}"
            results.append(
                _run_attempt(emulator, entry, attempt_number, attempt_directory)
            )
            if os.name == "nt" and results[-1].get("duration_seconds", 0) > 2.0:
                time.sleep(2.0)
            attempt_number += 1
    return results


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--title-id")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        results = run_corpus(
            args.manifest, args.emulator, args.output, title_id=args.title_id
        )
    except ManifestError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    for result in results:
        print(
            f"{result['title_id']} attempt-{result['attempt']:03d}: "
            f"{result['outcome']}"
        )
    return 0 if all(result["outcome"] == "completed" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
