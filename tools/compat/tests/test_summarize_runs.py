import json
import sys
import tempfile
import unittest
from pathlib import Path


COMPAT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(COMPAT_DIR))

import summarize_runs


class SummarizeRunsTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self):
        self.temp_dir.cleanup()

    def make_attempt(
        self,
        title_id,
        attempt,
        runner_outcome=None,
        diagnostics_outcome=None,
        gpu=None,
        presentation=None,
        executable="eboot.bin",
    ):
        directory = self.root / title_id / f"attempt-{attempt:03d}"
        directory.mkdir(parents=True)
        if runner_outcome is not None:
            (directory / "run.json").write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "title_id": title_id,
                        "attempt": attempt,
                        "outcome": runner_outcome,
                        "exit_code": 0 if runner_outcome == "completed" else 1,
                        "duration_seconds": 1.25,
                        "diagnostics_present": diagnostics_outcome is not None,
                    }
                ),
                encoding="utf-8",
            )
        if diagnostics_outcome is not None:
            metadata = {
                "build_label": "test-build",
                "platform": "Windows",
                "cpu_name": "Test CPU",
                "title": {
                    "id": title_id,
                    "name": f"Title {title_id}",
                    "content_version": "01.000.000",
                    "executable": executable,
                },
                "configuration": {},
            }
            if gpu is not None:
                metadata["gpu"] = gpu
            diagnostics = {
                "schema_version": 1,
                "metadata": metadata,
                "outcome": diagnostics_outcome,
                "outcome_detail": "details stay private",
                "events": [],
            }
            if presentation is not None:
                diagnostics["presentation"] = presentation
            (directory / "diagnostics.json").write_text(
                json.dumps(diagnostics),
                encoding="utf-8",
            )
        return directory

    def test_missing_diagnostics_uses_runner_outcome_without_claiming_a_rung(self):
        self.make_attempt("PPSA00002", 1, runner_outcome="launch_error")

        summary = summarize_runs.summarize(self.root)

        result = summary["results"][0]
        self.assertEqual("launch_error", result["outcome"])
        self.assertEqual("runner", result["evidence"])
        self.assertEqual("unknown", result["compatibility_rung"])
        self.assertIsNone(result["gpu"])

    def test_unfinished_running_report_is_unknown(self):
        self.make_attempt("PPSA00003", 1, diagnostics_outcome="running")

        summary = summarize_runs.summarize(self.root)

        self.assertEqual("unknown", summary["results"][0]["outcome"])
        self.assertEqual("incomplete", summary["results"][0]["evidence"])

    def test_optional_gpu_metadata_is_preserved_when_present(self):
        gpu = {
            "name": "Fake GPU",
            "vendor_id": 4318,
            "device_id": 1024,
            "driver_version": 123,
        }
        self.make_attempt(
            "PPSA00004",
            1,
            runner_outcome="completed",
            diagnostics_outcome="completed",
            gpu=gpu,
        )

        summary = summarize_runs.summarize(self.root)

        self.assertEqual(gpu, summary["results"][0]["gpu"])
        self.assertEqual("diagnostics", summary["results"][0]["evidence"])

    def test_optional_presentation_evidence_is_preserved_without_awarding_a_rung(self):
        presentation = {"presented_frames": 1440, "current_fps": 59.75}
        self.make_attempt(
            "PPSA00007",
            1,
            runner_outcome="timeout",
            diagnostics_outcome="running",
            presentation=presentation,
        )

        summary = summarize_runs.summarize(self.root)
        result = summary["results"][0]

        self.assertEqual(presentation, result["presentation"])
        self.assertEqual("unknown", result["compatibility_rung"])
        markdown = summarize_runs.render_markdown(summary)
        self.assertIn("| Frames | FPS |", markdown.splitlines()[6])
        self.assertIn("| 1440 | 59.75 |", markdown)

    def test_missing_or_malformed_presentation_evidence_is_ignored(self):
        self.make_attempt(
            "PPSA00008",
            1,
            runner_outcome="timeout",
            diagnostics_outcome="running",
        )
        self.make_attempt(
            "PPSA00009",
            1,
            runner_outcome="timeout",
            diagnostics_outcome="running",
            presentation={"presented_frames": True, "current_fps": "fast"},
        )

        summary = summarize_runs.summarize(self.root)

        self.assertIsNone(summary["results"][0]["presentation"])
        self.assertIsNone(summary["results"][1]["presentation"])

    def test_mixed_attempts_are_ordered_and_latest_is_selected(self):
        self.make_attempt(
            "PPSA00020", 2, runner_outcome="fatal", diagnostics_outcome="fatal"
        )
        self.make_attempt(
            "PPSA00010",
            1,
            runner_outcome="completed",
            diagnostics_outcome="completed",
        )
        self.make_attempt(
            "PPSA00020",
            1,
            runner_outcome="launch_error",
        )

        summary = summarize_runs.summarize(self.root)

        self.assertEqual(
            [("PPSA00010", 1), ("PPSA00020", 1), ("PPSA00020", 2)],
            [(item["title_id"], item["attempt"]) for item in summary["results"]],
        )
        latest = {item["title_id"]: item for item in summary["latest"]}
        self.assertEqual(2, latest["PPSA00020"]["attempt"])
        self.assertEqual("fatal", latest["PPSA00020"]["outcome"])
        self.assertEqual(1, summary["counts"]["completed"])
        self.assertEqual(1, summary["counts"]["fatal"])
        self.assertEqual(1, summary["counts"]["launch_error"])

    def test_outputs_strip_absolute_executable_paths(self):
        private_path = "C:/Users/private/owned-game/eboot.bin"
        self.make_attempt(
            "PPSA00005",
            1,
            runner_outcome="completed",
            diagnostics_outcome="completed",
            executable=private_path,
        )

        summary = summarize_runs.summarize(self.root)
        markdown = summarize_runs.render_markdown(summary)
        serialized = json.dumps(summary)

        self.assertNotIn(private_path, serialized)
        self.assertNotIn("C:/Users/private", markdown)
        self.assertEqual("eboot.bin", summary["results"][0]["executable"])

    def test_write_summaries_creates_deterministic_files(self):
        self.make_attempt(
            "PPSA00006",
            1,
            runner_outcome="completed",
            diagnostics_outcome="completed",
        )
        json_output = self.root / "public" / "summary.json"
        markdown_output = self.root / "public" / "summary.md"

        summarize_runs.write_summaries(self.root, json_output, markdown_output)
        first_json = json_output.read_text(encoding="utf-8")
        first_markdown = markdown_output.read_text(encoding="utf-8")
        summarize_runs.write_summaries(self.root, json_output, markdown_output)

        self.assertEqual(first_json, json_output.read_text(encoding="utf-8"))
        self.assertEqual(first_markdown, markdown_output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
