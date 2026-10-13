import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SHADER_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SHADER_DIR))

import reduce_shader


class ReduceShaderTests(unittest.TestCase):
    def test_binary_bytecode_is_formatted_as_a_standalone_cpp_test(self):
        dwords = reduce_shader.bytes_to_dwords(
            b"\x44\x33\x22\x11\x88\x77\x66\x55"
        )

        source = reduce_shader.generate_cpp_test(
            dwords,
            stage="cs",
            test_name="CheckReducedComputeShader",
            expected_opcode=14,
        )

        self.assertIn("void CheckReducedComputeShader()", source)
        self.assertIn("const uint32_t code[] = {", source)
        self.assertIn("0x11223344u, 0x55667788u", source)
        self.assertIn("MakeCompileOptions(ShaderType::Compute)", source)
        self.assertIn("CheckSpirvBinaryValidates(result.spirv);", source)
        self.assertIn("SpirvContainsOpcode(result.spirv, 14u)", source)

    def test_cli_output_excludes_input_path_game_name_and_metadata(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            input_path = root / "ASTRO BOT 0008_new_shader_cs_deadbeef.bin"
            output_path = root / "reduced.cpp"
            input_path.write_bytes(
                b"\x00\x00\x81\xbf\x00\x00\x80\xbf"
            )

            process = subprocess.run(
                [
                    sys.executable,
                    str(SHADER_DIR / "reduce_shader.py"),
                    "--input",
                    str(input_path),
                    "--stage",
                    "cs",
                    "--output",
                    str(output_path),
                ],
                capture_output=True,
                text=True,
            )

            self.assertEqual(process.returncode, 0, process.stderr)
            source = output_path.read_text(encoding="utf-8")
            self.assertNotIn(str(input_path), source)
            self.assertNotIn("ASTRO BOT", source)
            self.assertNotIn("0008_new_shader", source)
            self.assertNotIn("deadbeef", source.lower())
            self.assertNotIn("user_data", source)
            self.assertNotIn("shader_hash", source)

    def test_minimization_removes_unused_blocks_and_trailing_nops(self):
        trailing_nop = reduce_shader.S_NOP_0
        dwords = [0x11111111, 0xAAAAAAAA, 0x22222222, 0xBBBBBBBB]
        dwords.extend([trailing_nop, trailing_nop])

        def preserves_failure(candidate):
            return 0x11111111 in candidate and 0x22222222 in candidate

        minimized = reduce_shader.minimize_dwords(dwords, preserves_failure)

        self.assertEqual(minimized, [0x11111111, 0x22222222])
        self.assertTrue(preserves_failure(minimized))

    def test_binary_must_contain_complete_dwords(self):
        with self.assertRaisesRegex(ValueError, "multiple of 4"):
            reduce_shader.bytes_to_dwords(b"\x01\x02\x03")

    def test_invalid_cpp_identifier_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "C\\+\\+ identifier"):
            reduce_shader.generate_cpp_test(
                [0xBF810000], stage="cs", test_name="bad-name"
            )


if __name__ == "__main__":
    unittest.main()
