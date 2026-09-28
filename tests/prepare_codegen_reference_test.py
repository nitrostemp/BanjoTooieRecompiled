import tempfile
import unittest
from pathlib import Path

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from prepare_codegen import compare_reference, generated_registry_count


class ReferenceComparisonTest(unittest.TestCase):
    def test_only_crlf_is_normalized_and_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / "generated"
            retained = root / "retained"
            generated.mkdir()
            retained.mkdir()
            (generated / "funcs_1.c").write_bytes(b"one\ntwo\n")
            (retained / "funcs_1.c").write_bytes(b"one\r\ntwo\r\n")

            result = compare_reference(generated, retained)

            self.assertEqual(result["changed"], [])
            self.assertEqual(result["normalized_line_endings"], ["funcs_1.c"])

    def test_content_drift_still_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / "generated"
            retained = root / "retained"
            generated.mkdir()
            retained.mkdir()
            (generated / "funcs_1.c").write_bytes(b"one\nchanged\n")
            (retained / "funcs_1.c").write_bytes(b"one\r\ntwo\r\n")

            with self.assertRaisesRegex(RuntimeError, "funcs_1.c"):
                compare_reference(generated, retained)

    def test_binary_crlf_bytes_are_not_normalized(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = root / "generated"
            retained = root / "retained"
            generated.mkdir()
            retained.mkdir()
            (generated / "fixture.bin").write_bytes(b"one\ntwo\n")
            (retained / "fixture.bin").write_bytes(b"one\r\ntwo\r\n")

            with self.assertRaisesRegex(RuntimeError, "fixture.bin"):
                compare_reference(generated, retained)


class RegistryCountTest(unittest.TestCase):
    def test_counts_generated_entries_plus_core1_adapter(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recomp_overlays.inl"
            path.write_text(
                "static FuncEntry section_0_boot_funcs[] = {\n"
                "    { .func = first, .offset = 0x00000000, .rom_size = 0x00000010 },\n"
                "};\n"
                "static FuncEntry section_1_core1_funcs[] = {\n"
                "    { .func = second, .offset = 0x00000010, .rom_size = 0x00000020 },\n"
                "};\n")

            self.assertEqual(generated_registry_count(path, 2), 3)

    def test_format_drift_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recomp_overlays.inl"
            path.write_text(
                "static FuncEntry section_0_boot_funcs[] = {\n"
                "    { .func = first, .offset = 0, .rom_size = 0x10 },\n"
                "};\n")

            with self.assertRaisesRegex(RuntimeError, "FuncEntry format drift"):
                generated_registry_count(path, 1)


if __name__ == "__main__":
    unittest.main()
