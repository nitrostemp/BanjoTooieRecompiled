"""Pinned source seams for replay-local scheduler pacing."""
import importlib.util
import re
import unittest
from pathlib import Path

APP = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('instrument', APP / 'tools/instrument_continuous.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def function(name):
    for path in (APP / 'generated').glob('*.c'):
        match = re.search(rf'RECOMP_FUNC void {name}\([^\n]*\n.*?^;}}\n', path.read_text(), re.S | re.M)
        if match:
            body = match[0]
            body = re.sub(r'^    ctx->r2 = .*tooie_replay_timing_scheduler_divisor\([^\n]*\n', '', body, flags=re.M)
            body = re.sub(r'^    tooie_replay_timing_begin_record\(\);\n', '', body, flags=re.M)
            body = re.sub(r'^    tooie_replay_timing_record_divisor\([^\n]*\n', '', body, flags=re.M)
            return body
    raise AssertionError(f'missing generated function {name}')


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = '#include "funcs.h"\n' + function('func_8001608C') + function('func_800151BC')

    def test_exact_three_hooks_and_order(self):
        changed, count = m.apply_replay_timing_hooks(self.source)
        self.assertEqual(count, 4)
        self.assertEqual(changed.count('extern void tooie_replay_timing_begin_record(void);'), 1)
        self.assertEqual(changed.count('extern void tooie_replay_timing_record_divisor(uint32_t, int);'), 1)
        self.assertEqual(changed.count('extern uint32_t tooie_replay_timing_scheduler_divisor(uint32_t, int);'), 1)
        self.assertIn('tooie_continuous_poll(rdram, ctx, 0x8001608Cu);\n    tooie_replay_timing_begin_record();', changed)
        self.assertIn('tooie_replay_timing_record_divisor((uint32_t)MEM_W(0X50, ctx->r29), ctx->r2 != 0);\n        goto after_3;', changed)
        self.assertIn('after_4:\n    ctx->r2 = (gpr)(int32_t)tooie_replay_timing_scheduler_divisor((uint32_t)ctx->r2, 1);\n    // 0x8001520C:', changed)
        self.assertIn('after_6:\n    ctx->r2 = (gpr)(int32_t)tooie_replay_timing_scheduler_divisor((uint32_t)ctx->r2, 0);\n    // 0x8001524C:', changed)

    def test_duplicate_and_anchor_changes_fail_closed(self):
        changed, _ = m.apply_replay_timing_hooks(self.source)
        with self.assertRaises(ValueError):
            m.apply_replay_timing_hooks(changed)
        for old, new in [
            ('_glrecord_entrypoint_0(rdram, ctx);', 'wrong_record(rdram, ctx);'),
            ('tooie_continuous_poll(rdram, ctx, 0x8001608Cu);',
             'wrong_replay_record_entry(rdram, ctx);'),
            ('after_4:', 'after_first:'),
            ('after_6:', 'after_second:'),
            ('0x8001520C:', '0x80015210:'),
            ('0x8001524C:', '0x80015250:'),
        ]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                m.apply_replay_timing_hooks(self.source.replace(old, new))

    def test_game_delta_samples_after_replay_controller(self):
        body = function('func_800123F4')
        body = re.sub(r'^    tooie_game_delta_observed\([^\n]*\n', '', body, flags=re.M)
        changed, count = m.apply_game_delta_observer('#include "funcs.h"\n' + body)
        self.assertEqual(count, 1)
        self.assertIn('func_8001608C(rdram, ctx);\n        goto after_4;\n    // 0x8001245C: nop\n\n    after_4:\n    tooie_game_delta_observed', changed)
        self.assertNotIn('after_3:\n    tooie_game_delta_observed', changed)
        with self.assertRaises(ValueError):
            m.apply_game_delta_observer(('#include "funcs.h"\n' + body).replace('0x8001245C:', '0x80012460:'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
