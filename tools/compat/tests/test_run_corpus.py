import json
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path


COMPAT_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(COMPAT_DIR))

import run_corpus


class CorpusRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.game = self.root / "games with spaces" / "雪" / "PPSA00001"
        self.game.mkdir(parents=True)
        self.emulator = self.root / "fake emulator.py"
        self.emulator.write_text(
            textwrap.dedent(
                """
                import json
                import sys
                import time
                from pathlib import Path

                args = sys.argv[1:]
                mode = args[args.index("--fake-mode") + 1]
                report = Path(args[args.index("--diagnostics-output") + 1])
                print("fake emulator stdout", flush=True)
                print("fake emulator stderr", file=sys.stderr, flush=True)
                if mode == "sleep":
                    time.sleep(5)
                elif mode == "exit":
                    raise SystemExit(7)
                else:
                    report.write_text(json.dumps({
                        "schema_version": 1,
                        "metadata": {
                            "build_label": "fake-build",
                            "platform": sys.platform,
                            "cpu_name": "fake-cpu",
                            "title": {
                                "id": "PPSA00001",
                                "name": "Fake Game",
                                "content_version": "01.000.000",
                                "executable": "eboot.bin"
                            },
                            "configuration": {}
                        },
                        "outcome": "completed",
                        "outcome_detail": "fake completion",
                        "events": []
                    }), encoding="utf-8")
                """
            ),
            encoding="utf-8",
        )
        self.output = self.root / "output"

    def tearDown(self):
        self.temp_dir.cleanup()

    def write_manifest(self, **overrides):
        entry = {
            "title_id": "PPSA00001",
            "game_path": str(self.game),
            "timeout_seconds": 2,
            "repeat": 1,
            "arguments": ["--fake-mode", "success"],
        }
        entry.update(overrides)
        manifest = self.root / "corpus.json"
        manifest.write_text(
            json.dumps({"schema_version": 1, "titles": [entry]}),
            encoding="utf-8",
        )
        return manifest

    def test_valid_launch_handles_spaces_and_unicode(self):
        results = run_corpus.run_corpus(
            self.write_manifest(), self.emulator, self.output
        )

        self.assertEqual("completed", results[0]["outcome"])
        attempt = self.output / "PPSA00001" / "attempt-001"
        self.assertTrue((attempt / "diagnostics.json").is_file())
        self.assertIn("fake emulator stdout", (attempt / "stdout.txt").read_text())
        self.assertIn("fake emulator stderr", (attempt / "stderr.txt").read_text())

        command = (attempt / "command.json").read_text(encoding="utf-8")
        self.assertNotIn(str(self.game), command)
        self.assertNotIn(str(self.emulator), command)
        self.assertIn("<game:PPSA00001>", command)
        self.assertIn("<emulator>", command)

    def test_invalid_title_id_is_rejected(self):
        with self.assertRaises(run_corpus.ManifestError):
            run_corpus.load_manifest(self.write_manifest(title_id="CUSA00001"))

    def test_missing_game_path_is_rejected(self):
        with self.assertRaises(run_corpus.ManifestError):
            run_corpus.load_manifest(
                self.write_manifest(game_path=str(self.root / "missing"))
            )

    def test_unknown_entry_field_is_rejected(self):
        with self.assertRaises(run_corpus.ManifestError):
            run_corpus.load_manifest(self.write_manifest(typo_field=True))

    def test_null_patch_path_is_rejected(self):
        with self.assertRaises(run_corpus.ManifestError):
            run_corpus.load_manifest(self.write_manifest(patch_path=None))

    def test_timeout_terminates_and_classifies_attempt(self):
        started = time.monotonic()
        results = run_corpus.run_corpus(
            self.write_manifest(
                timeout_seconds=0.2, arguments=["--fake-mode", "sleep"]
            ),
            self.emulator,
            self.output,
        )

        self.assertLess(time.monotonic() - started, 3)
        self.assertEqual("timeout", results[0]["outcome"])
        self.assertIsNone(results[0]["exit_code"])
        stdout = (
            self.output / "PPSA00001" / "attempt-001" / "stdout.txt"
        ).read_text(encoding="utf-8")
        self.assertEqual(1, stdout.count("fake emulator stdout"))

    def test_nonzero_exit_without_report_is_launch_error(self):
        results = run_corpus.run_corpus(
            self.write_manifest(arguments=["--fake-mode", "exit"]),
            self.emulator,
            self.output,
        )

        self.assertEqual("launch_error", results[0]["outcome"])
        self.assertEqual(7, results[0]["exit_code"])

    def test_repeated_attempts_are_isolated(self):
        results = run_corpus.run_corpus(
            self.write_manifest(repeat=2), self.emulator, self.output
        )

        self.assertEqual(2, len(results))
        self.assertTrue(
            (self.output / "PPSA00001" / "attempt-001" / "run.json").is_file()
        )
        self.assertTrue(
            (self.output / "PPSA00001" / "attempt-002" / "run.json").is_file()
        )

    def test_generated_public_metadata_contains_no_absolute_paths(self):
        private_log = self.root / "private logs"
        run_corpus.run_corpus(
            self.write_manifest(
                arguments=[
                    "--fake-mode",
                    "success",
                    "--shader-log-folder",
                    str(private_log),
                ]
            ),
            self.emulator,
            self.output,
        )
        attempt = self.output / "PPSA00001" / "attempt-001"

        public_metadata = (attempt / "run.json").read_text(encoding="utf-8")
        public_command = (attempt / "command.json").read_text(encoding="utf-8")
        self.assertNotIn(str(self.root), public_metadata)
        self.assertNotIn(str(self.game), public_metadata)
        self.assertNotIn(str(self.emulator), public_metadata)
        self.assertNotIn(str(private_log), public_command)
        self.assertIn("<path>", public_command)


if __name__ == "__main__":
    unittest.main()
