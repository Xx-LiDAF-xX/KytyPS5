import json
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SHADER_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SHADER_DIR))

import recompile_shader


class RecompileShaderTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.fake_emulator = self.root / "fake_emulator.py"
        self.fake_emulator.write_text(
            textwrap.dedent(
                """
                import json
                import sys
                from pathlib import Path

                args = sys.argv[1:]
                if "--recompile-shader" not in args:
                    print("unknown arguments", file=sys.stderr)
                    sys.exit(1)

                idx = args.index("--recompile-shader")
                stage = args[idx + 1]
                input_file = Path(args[idx + 2])

                if not input_file.exists():
                    print(f"File not found: {input_file}", file=sys.stderr)
                    sys.exit(2)

                out_file = None
                if "--out" in args:
                    out_file = Path(args[args.index("--out") + 1])
                    out_file.write_bytes(b"\\x03\\x02\\x23\\x07fake_spirv_binary")

                stats = {
                    "status": "ok",
                    "stage": stage,
                    "input_size_bytes": input_file.stat().st_size,
                    "output_size_dwords": 8,
                }
                print(json.dumps(stats), flush=True)
                """
            ),
            encoding="utf-8",
        )

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_invalid_stage_is_rejected(self):
        dummy_input = self.root / "test.bin"
        dummy_input.write_bytes(b"\x00" * 16)
        with self.assertRaises(ValueError):
            recompile_shader.run_recompile(
                emulator=sys.executable,
                stage="invalid_stage",
                input_path=dummy_input,
            )

    def test_missing_input_file_is_rejected(self):
        missing_input = self.root / "does_not_exist.bin"
        with self.assertRaises(FileNotFoundError):
            recompile_shader.run_recompile(
                emulator=sys.executable,
                stage="cs",
                input_path=missing_input,
            )

    def test_successful_recompile_with_mock_emulator(self):
        dummy_input = self.root / "valid_shader.bin"
        dummy_input.write_bytes(b"\x11\x22\x33\x44" * 4)
        out_spv = self.root / "output.spv"

        result = recompile_shader.run_recompile(
            emulator=sys.executable,
            stage="cs",
            input_path=dummy_input,
            output_path=out_spv,
            extra_args=[str(self.fake_emulator)],
        )

        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["stage"], "cs")
        self.assertTrue(out_spv.exists())
        self.assertGreater(out_spv.stat().st_size, 0)

    def test_canonicalize_stage(self):
        self.assertEqual(recompile_shader.canonicalize_stage("compute"), "cs")
        self.assertEqual(recompile_shader.canonicalize_stage("CS"), "cs")
        self.assertEqual(recompile_shader.canonicalize_stage("vertex"), "vs")
        self.assertEqual(recompile_shader.canonicalize_stage("VS"), "vs")
        self.assertEqual(recompile_shader.canonicalize_stage("pixel"), "ps")
        self.assertEqual(recompile_shader.canonicalize_stage("PS"), "ps")
        with self.assertRaises(ValueError):
            recompile_shader.canonicalize_stage("geometry")


if __name__ == "__main__":
    unittest.main()
