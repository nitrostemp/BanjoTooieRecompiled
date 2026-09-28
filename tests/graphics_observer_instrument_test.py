"""Exact original scheduler/task anchors; never write generated source."""
import importlib.util
from pathlib import Path
import re
import unittest
import tempfile
APP=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('instrument',APP/'tools/instrument_continuous.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
NAMES=('func_80014A88','func_80013E34','func_80013EF0','func_80013FE4','func_800146D8')
def originals():
    found={}
    for path in (APP/'generated').glob('*.c'):
        text=path.read_text()
        for name in NAMES:
            match=re.search(r'RECOMP_FUNC void '+name+r'\([^\n]*\n.*?^;}\n',text,re.S|re.M)
            if match:
                if name in found:raise AssertionError('Duplicate generated target')
                found[name]=re.sub(r'^    tooie_(?:continuous_poll|overlay_callsite_push|overlay_callsite_pop|observe_graphics)\([^\n]*\n','',match[0],flags=re.M)
    if set(found)!=set(NAMES):raise AssertionError('Expected actual selected core1 functions')
    return ''.join(found[n] for n in NAMES)
class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.source=originals()
    def test_actual_five_hooks_exact_roundtrip(self):
        text,count=module.observe_graphics(self.source)
        self.assertEqual(count,5)
        self.assertEqual(re.sub(r'^    tooie_observe_graphics\([^\n]*\n','',text,flags=re.M),self.source)
        self.assertIn('osRecvMesg_recomp(rdram, ctx);\n    tooie_observe_graphics(rdram, ctx, 0x80014AACu);\n        goto after_0;',text)
        for pc in ['80013ECC','80013F7C','80014070']:
            self.assertIn('ctx->r4 = ctx->r16 | 0;\n    tooie_observe_graphics(rdram, ctx, 0x'+pc+'u);\n    osSpTaskStartGo_recomp(rdram, ctx);',text)
        self.assertIn('ctx->r4 = ADD32(ctx->r4, -0XC10);\n    tooie_observe_graphics(rdram, ctx, 0x800147A8u);',text)
    def test_wrong_pc_call_delay_or_target_fails(self):
        mutations=[('// 0x80014AA4:','// 0x80014AA0:'),('0x8002E1F0','0x8002E1F4'),
            ('ctx->r6 = ADD32(0, 0X1);','ctx->r6 = ADD32(0, 0X0);'),
            ('ctx->r4 = ctx->r16 | 0;','ctx->r4 = ctx->r17 | 0;'),
            ('ctx->r4 = ADD32(ctx->r4, -0XC10);','ctx->r4 = ADD32(ctx->r4, -0XC14);'),
            ('osSpTaskStartGo_recomp(rdram, ctx);','wrong_task_call(rdram, ctx);')]
        for old,new in mutations:
            with self.subTest(old=old),self.assertRaises(ValueError):module.observe_graphics(self.source.replace(old,new))
    def test_function_scope_and_partial_fixture(self):
        for name in NAMES:
            body=re.search(r'RECOMP_FUNC void '+name+r'\([^\n]*\n.*?^;}\n',self.source,re.S|re.M)[0]
            self.assertEqual(module.observe_graphics(body)[1],1)
            renamed=body.replace('RECOMP_FUNC void '+name+'(','RECOMP_FUNC void unrelated(')
            self.assertEqual(module.observe_graphics(renamed),(renamed,0))
    def test_duplicate_and_double_transform_rejected(self):
        with self.assertRaises(ValueError):module.observe_graphics(self.source+self.source)
        text,_=module.observe_graphics(self.source)
        with self.assertRaises(ValueError):module.observe_graphics(text)
    def test_full_pass_counts_and_zero_target_fixture(self):
        helper_spec=importlib.util.spec_from_file_location('callsite_test',APP/'tests/instrument_continuous_test.py')
        helper=importlib.util.module_from_spec(helper_spec);helper_spec.loader.exec_module(helper)
        for include,expected in [(True,5),(False,0)]:
            functions=list(helper.SECTIONS[0]['functions'])
            if include:functions += [{'name':n,'vram':int(n[5:],16),'size':0x400} for n in NAMES]
            with tempfile.TemporaryDirectory() as folder:
                path=Path(folder)/'fixture.c';path.write_text(helper.PREREQUISITES+(self.source if include else ''))
                counts=module.instrument(folder,[{'functions':functions}])
            self.assertEqual(counts['graphics_observations'],expected)
if __name__=='__main__':unittest.main(verbosity=2)
