import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'instrument_continuous', ROOT / 'tools' / 'instrument_continuous.py')
INSTRUMENT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTRUMENT)


def body(name):
    result = (f'RECOMP_FUNC void {name}(uint8_t* rdram, recomp_context* ctx) {{\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n'
            '    int c1cs = 0;\n')
    if name == 'func_80017404':
        result += ('    // 0x8001744C: sh          $a1, 0x26($sp)\n'
                   '    MEM_H(0X26, ctx->r29) = ctx->r5;\n'
                   '    // 0x80017450: addiu       $a0, $v0, 0x1E0\n'
                   '    ctx->r4 = ADD32(ctx->r2, 0X1E0);\n'
                   '    // 0x80017454: jal         0x80025ED0\n')
    return result + '    return;\n;}\n'


class MusicVolumeInstrumentationTest(unittest.TestCase):
    def test_pins_manager_refresh_and_private_setter(self):
        source = '#include "funcs.h"\n' + body('func_800FB968') + body('func_80017404')
        transformed, count = INSTRUMENT.apply_music_volume_hooks(source, 'fixture')
        self.assertEqual(count, 2)
        self.assertEqual(transformed.count('tooie_music_volume_tick(rdram, ctx);'), 1)
        self.assertEqual(transformed.count('tooie_music_volume_apply(rdram, ctx);'), 1)
        apply_at = transformed.index('tooie_music_volume_apply(rdram, ctx);')
        self.assertLess(transformed.index('0x8001744C'), apply_at)
        self.assertLess(apply_at, transformed.index('0x80017450'))

    def test_rejects_reapplication(self):
        source = '#include "funcs.h"\n' + body('func_80017404')
        transformed, _ = INSTRUMENT.apply_music_volume_hooks(source, 'fixture')
        with self.assertRaisesRegex(ValueError, 'already present'):
            INSTRUMENT.apply_music_volume_hooks(transformed, 'fixture')


if __name__ == '__main__':
    unittest.main()
