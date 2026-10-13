#!/usr/bin/env python3
"""Generate deterministic, sanitized summaries from compatibility attempts."""

from __future__ import annotations

import argparse
import json
import math
import re
from collections import Counter
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
COUNTED_OUTCOMES = (
    "completed",
    "fatal",
    "host_exception",
    "timeout",
    "launch_error",
    "unknown",
)
CONFIGURATION_FIELDS = ("vulkan_validation", "shader_validation")


def _read_json(path: Path) -> dict[str, Any] | None:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return document if isinstance(document, dict) else None


def _string(value: Any, default: str = "unknown") -> str:
    return value if isinstance(value, str) and value else default


def _filename_leaf(value: Any) -> str:
    if not isinstance(value, str) or not value:
        return "unknown"
    normalized = value.replace("\\", "/").rstrip("/")
    return normalized.rsplit("/", 1)[-1] or "unknown"


def _attempt_number(directory: Path, runner: dict[str, Any] | None) -> int:
    if runner is not None:
        attempt = runner.get("attempt")
        if isinstance(attempt, int) and not isinstance(attempt, bool) and attempt > 0:
            return attempt
    match = re.fullmatch(r"attempt-(\d+)", directory.name)
    return int(match.group(1)) if match else 0


def _title_id(
    directory: Path,
    runner: dict[str, Any] | None,
    diagnostics: dict[str, Any] | None,
) -> str | None:
    candidates: list[Any] = []
    if runner is not None:
        candidates.append(runner.get("title_id"))
    if diagnostics is not None:
        metadata = diagnostics.get("metadata")
        if isinstance(metadata, dict):
            title = metadata.get("title")
            if isinstance(title, dict):
                candidates.append(title.get("id"))
    candidates.append(directory.parent.name)
    for candidate in candidates:
        if isinstance(candidate, str) and TITLE_ID_PATTERN.fullmatch(candidate):
            return candidate
    return None


def _gpu_metadata(metadata: dict[str, Any]) -> dict[str, Any] | None:
    gpu = metadata.get("gpu")
    if not isinstance(gpu, dict):
        return None
    return {
        "name": _string(gpu.get("name")),
        "vendor_id": gpu.get("vendor_id") if isinstance(gpu.get("vendor_id"), int) else 0,
        "device_id": gpu.get("device_id") if isinstance(gpu.get("device_id"), int) else 0,
        "driver_version": (
            gpu.get("driver_version")
            if isinstance(gpu.get("driver_version"), int)
            else 0
        ),
    }


def _presentation_metadata(document: dict[str, Any] | None) -> dict[str, Any] | None:
    if document is None:
        return None
    presentation = document.get("presentation")
    if not isinstance(presentation, dict):
        return None
    presented_frames = presentation.get("presented_frames")
    current_fps = presentation.get("current_fps")
    if (
        not isinstance(presented_frames, int)
        or isinstance(presented_frames, bool)
        or presented_frames < 0
        or not isinstance(current_fps, (int, float))
        or isinstance(current_fps, bool)
        or not math.isfinite(current_fps)
        or current_fps < 0
    ):
        return None
    return {
        "presented_frames": presented_frames,
        "current_fps": current_fps,
    }


def _non_negative_integer(value: Any) -> int | None:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        return None
    return value


def _unsupported_import_metadata(
    document: dict[str, Any] | None,
) -> dict[str, int] | None:
    if document is None:
        return None
    unsupported_imports = document.get("unsupported_imports")
    if not isinstance(unsupported_imports, dict):
        return None
    entries = unsupported_imports.get("entries")
    dropped_unique = _non_negative_integer(
        unsupported_imports.get("dropped_unique", 0)
    )
    if not isinstance(entries, list) or dropped_unique is None:
        return None

    unique = 0
    calls = 0
    invalid_entry = False
    for entry in entries:
        if not isinstance(entry, dict):
            invalid_entry = True
            continue
        identifiers = (
            entry.get("symbol"),
            entry.get("program"),
            entry.get("type"),
            entry.get("binding"),
        )
        call_count = _non_negative_integer(entry.get("call_count"))
        if (
            any(not isinstance(value, str) or not value for value in identifiers)
            or call_count is None
        ):
            invalid_entry = True
            continue
        unique += 1
        calls += call_count

    if invalid_entry and unique == 0 and dropped_unique == 0:
        return None
    return {
        "unique": unique,
        "calls": calls,
        "dropped_unique": dropped_unique,
    }


def _configuration_metadata(metadata: dict[str, Any]) -> dict[str, str]:
    configuration = metadata.get("configuration")
    if not isinstance(configuration, dict):
        configuration = {}
    return {
        field: _string(configuration.get(field)) for field in CONFIGURATION_FIELDS
    }


def _summarize_attempt(directory: Path) -> dict[str, Any] | None:
    runner = _read_json(directory / "run.json")
    diagnostics = _read_json(directory / "diagnostics.json")
    title_id = _title_id(directory, runner, diagnostics)
    if title_id is None:
        return None

    metadata: dict[str, Any] = {}
    if diagnostics is not None and isinstance(diagnostics.get("metadata"), dict):
        metadata = diagnostics["metadata"]
    title = metadata.get("title") if isinstance(metadata.get("title"), dict) else {}

    diagnostic_outcome = diagnostics.get("outcome") if diagnostics is not None else None
    runner_outcome = runner.get("outcome") if runner is not None else None
    if diagnostic_outcome in FINAL_OUTCOMES:
        outcome = diagnostic_outcome
        evidence = "diagnostics"
    elif runner_outcome in FINAL_OUTCOMES:
        outcome = runner_outcome
        evidence = "runner"
    else:
        outcome = "unknown"
        evidence = "incomplete"

    exit_code = runner.get("exit_code") if runner is not None else None
    if not isinstance(exit_code, int) or isinstance(exit_code, bool):
        exit_code = None
    duration = runner.get("duration_seconds") if runner is not None else None
    if isinstance(duration, bool) or not isinstance(duration, (int, float)):
        duration = None

    return {
        "title_id": title_id,
        "title_name": _string(title.get("name")),
        "content_version": _string(title.get("content_version")),
        "executable": _filename_leaf(title.get("executable")),
        "build_label": _string(metadata.get("build_label")),
        "platform": _string(metadata.get("platform")),
        "cpu_name": _string(metadata.get("cpu_name")),
        "gpu": _gpu_metadata(metadata),
        "configuration": _configuration_metadata(metadata),
        "presentation": _presentation_metadata(diagnostics),
        "unsupported_imports": _unsupported_import_metadata(diagnostics),
        "attempt": _attempt_number(directory, runner),
        "outcome": outcome,
        "evidence": evidence,
        "compatibility_rung": "unknown",
        "exit_code": exit_code,
        "duration_seconds": duration,
    }


def summarize(
    input_path: Path | str,
    required_configuration: dict[str, str] | None = None,
) -> dict[str, Any]:
    root = Path(input_path)
    results: list[dict[str, Any]] = []
    if root.is_dir():
        for directory in root.rglob("attempt-*"):
            if not directory.is_dir():
                continue
            result = _summarize_attempt(directory)
            if result is not None and (
                required_configuration is None
                or all(
                    result["configuration"].get(key) == value
                    for key, value in required_configuration.items()
                )
            ):
                results.append(result)

    results.sort(
        key=lambda item: (
            item["title_id"],
            item["attempt"],
            item["content_version"],
            item["platform"],
        )
    )
    counts = Counter(item["outcome"] for item in results)

    latest_by_title: dict[str, dict[str, Any]] = {}
    for result in results:
        previous = latest_by_title.get(result["title_id"])
        if previous is None or result["attempt"] >= previous["attempt"]:
            latest_by_title[result["title_id"]] = {
                "title_id": result["title_id"],
                "attempt": result["attempt"],
                "outcome": result["outcome"],
                "content_version": result["content_version"],
                "platform": result["platform"],
                "configuration": result["configuration"],
            }

    return {
        "schema_version": 1,
        "counts": {outcome: counts[outcome] for outcome in COUNTED_OUTCOMES},
        "latest": [latest_by_title[key] for key in sorted(latest_by_title)],
        "results": results,
    }


def _markdown_cell(value: Any) -> str:
    if value is None:
        return "unknown"
    return str(value).replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def render_markdown(summary: dict[str, Any]) -> str:
    counts = summary["counts"]
    lines = [
        "# KytyPS5 Compatibility Run Summary",
        "",
        (
            "Outcomes: "
            + ", ".join(f"{name}={counts[name]}" for name in COUNTED_OUTCOMES)
            + "."
        ),
        "",
        "Process outcomes do not establish a compatibility rung; manual evidence is required.",
        "",
        (
            "| Title ID | Version | Platform | Attempt | Outcome | Evidence | GPU | "
            "Vulkan validation | Shader validation | Unsupported imports | Frames | FPS |"
        ),
        "| --- | --- | --- | ---: | --- | --- | --- | --- | --- | --- | ---: | ---: |",
    ]
    for result in summary["results"]:
        gpu_name = result["gpu"]["name"] if result["gpu"] is not None else "unknown"
        presentation = result["presentation"]
        presented_frames = (
            presentation["presented_frames"] if presentation is not None else "unknown"
        )
        current_fps = presentation["current_fps"] if presentation is not None else "unknown"
        unsupported_imports = result["unsupported_imports"]
        unsupported_import_summary = (
            f"{unsupported_imports['unique']} symbols / "
            f"{unsupported_imports['calls']} calls"
            if unsupported_imports is not None
            else "unknown"
        )
        lines.append(
            "| "
            + " | ".join(
                _markdown_cell(value)
                for value in (
                    result["title_id"],
                    result["content_version"],
                    result["platform"],
                    result["attempt"],
                    result["outcome"],
                    result["evidence"],
                    gpu_name,
                    result["configuration"]["vulkan_validation"],
                    result["configuration"]["shader_validation"],
                    unsupported_import_summary,
                    presented_frames,
                    current_fps,
                )
            )
            + " |"
        )
    return "\n".join(lines) + "\n"


def write_summaries(
    input_path: Path | str,
    json_path: Path | str,
    markdown_path: Path | str,
    required_configuration: dict[str, str] | None = None,
) -> dict[str, Any]:
    summary = summarize(input_path, required_configuration=required_configuration)
    json_output = Path(json_path)
    markdown_output = Path(markdown_path)
    json_output.parent.mkdir(parents=True, exist_ok=True)
    markdown_output.parent.mkdir(parents=True, exist_ok=True)
    json_output.write_text(
        json.dumps(summary, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    markdown_output.write_text(render_markdown(summary), encoding="utf-8")
    return summary


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    parser.add_argument("--markdown", type=Path, required=True)
    parser.add_argument("--vulkan-validation", choices=("true", "false"))
    parser.add_argument("--shader-validation", choices=("true", "false"))
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    required_configuration = {
        key: value
        for key, value in (
            ("vulkan_validation", args.vulkan_validation),
            ("shader_validation", args.shader_validation),
        )
        if value is not None
    }
    summary = write_summaries(
        args.input,
        args.json,
        args.markdown,
        required_configuration=required_configuration or None,
    )
    print(f"summarized {len(summary['results'])} compatibility attempts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
