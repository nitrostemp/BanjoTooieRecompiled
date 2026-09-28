import importlib.util
import re
import unittest
from pathlib import Path


APP = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("instrument", APP / "tools/instrument_continuous.py")
instrument = importlib.util.module_from_spec(spec)
spec.loader.exec_module(instrument)


class SaveProgressPauseHookTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        generated = (APP / "generated" / "funcs_381.c").read_text(encoding="utf-8")
        body = re.search(
            r"RECOMP_FUNC void gcnewpause_entrypoint_2\(uint8_t\* rdram, recomp_context\* ctx\) \{\n.*?^;}\n",
            generated, re.S | re.M).group(0)
        # Recreate the raw generator entry shape: generic continuous-poll and
        # this hook are both post-generation adaptations.
        body = body.replace("    tooie_continuous_poll(rdram, ctx, 0x80800100u);\n", "", 1)
        body = body.replace("    tooie_save_progress_pause_tick(rdram, ctx, (uint32_t)ctx->r4);\n", "", 1)
        cls.source = '#include "recomp.h"\n#include "boot_hooks.h"\n#include "funcs.h"\n\n' + body

    def test_unique_pinned_pause_entry(self):
        transformed, count = instrument.apply_save_progress_pause_hook(self.source)
        self.assertEqual(count, 1)
        self.assertEqual(transformed.count("tooie_save_progress_pause_tick"), 2)  # declaration and call
        self.assertIn("tooie_save_progress_pause_tick(rdram, ctx, (uint32_t)ctx->r4);", transformed)
        with self.assertRaises(ValueError):
            instrument.apply_save_progress_pause_hook(transformed)

    def test_rejects_changed_entry_shape(self):
        changed = self.source.replace("uint64_t hi = 0, lo = 0, result = 0;", "uint64_t hi = 1, lo = 0, result = 0;", 1)
        with self.assertRaises(ValueError):
            instrument.apply_save_progress_pause_hook(changed)


if __name__ == "__main__":
    unittest.main()
