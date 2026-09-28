import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "instrument_continuous", ROOT / "tools" / "instrument_continuous.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class HudOrthoInstrumentationTests(unittest.TestCase):
    def test_both_ortho_paths_are_adjusted(self):
        source = (ROOT / "generated" / "funcs_157.c").read_text()
        source = source.replace(
            "    tooie_hud_ortho_adjust(rdram, ctx);\n", "")
        source = source.replace(
            "extern void tooie_hud_ortho_adjust(uint8_t*, recomp_context*);\n", "")
        updated, count = MODULE.apply_hud_ortho_hook(source)
        self.assertEqual(count, 2)
        self.assertEqual(updated.count("    tooie_hud_ortho_adjust(rdram, ctx);\n"), 2)
        self.assertEqual(updated.count(
            "extern void tooie_hud_ortho_adjust(uint8_t*, recomp_context*);\n"), 1)
        with self.assertRaises(ValueError):
            MODULE.apply_hud_ortho_hook(updated)


if __name__ == "__main__":
    unittest.main()
