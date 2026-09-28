"""Focused original-function transform tests; never edit generated input."""
import importlib.util
from pathlib import Path
import re
import unittest
import tempfile

APP=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('instrument',APP/'tools/instrument_continuous.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
NAMES=('func_80800168_glglobalsettings','glglobalsettings_entrypoint_1')

def widescreen_original():
    found=[]
    for path in (APP/'generated').glob('*.c'):
        text=path.read_text()
        match=re.search(r'RECOMP_FUNC void func_80800034_glglobalsettings\([^\n]*\n.*?^;}\n',text,re.S|re.M)
        if match:found.append(match[0])
    if len(found)!=1:raise AssertionError('Expected one actual global-settings import function')
    return re.sub(r'^    tooie_(?:continuous_poll|overlay_callsite_push|overlay_callsite_pop|widescreen_apply_profile)\([^\n]*\n','',found[0],flags=re.M)

def originals():
    found={}
    for path in (APP/'generated').glob('*.c'):
        text=path.read_text()
        for name in NAMES:
            match=re.search(r'RECOMP_FUNC void '+name+r'\([^\n]*\n.*?^;}\n',text,re.S|re.M)
            if match:
                if name in found:raise AssertionError('Duplicate generated target')
                found[name]=re.sub(r'^    tooie_(?:continuous_poll|overlay_callsite_push|overlay_callsite_pop|observe_global_settings)\([^\n]*\n','',match[0],flags=re.M)
    if set(found)!=set(NAMES):raise AssertionError('Expected actual selected generated functions')
    return ''.join(found[name] for name in NAMES)

class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.source=originals()
    def test_actual_bodies_five_hooks_and_exact_roundtrip(self):
        after,count=module.observe_global_settings(self.source)
        self.assertEqual(count,5)
        self.assertEqual(re.sub(r'^    tooie_observe_global_settings\([^\n]*\n','',after,flags=re.M),self.source)
        self.assertIn('func_808000A4_glglobalsettings(rdram, ctx);\n    tooie_observe_global_settings(ctx, 0x808001B0u);',after)
        self.assertIn('ctx->r4 = MEM_W(ctx->r29, 0X18);\n    tooie_observe_global_settings(ctx, 0x80800250u);',after)
        self.assertIn('_glglobaldata_entrypoint_2(rdram, ctx);\n    tooie_observe_global_settings(ctx, 0x80800258u);',after)
    def test_no_target_functions_means_zero(self):
        source=self.source.replace('func_80800168_glglobalsettings','unrelated_selector').replace('glglobalsettings_entrypoint_1','unrelated_overlay')
        self.assertEqual(module.observe_global_settings(source),(source,0))
    def test_widescreen_hook_is_pinned_and_reproducible(self):
        source=widescreen_original()
        after,count=module.apply_widescreen_profile(source)
        self.assertEqual(count,1)
        self.assertIn('ctx->r4 = ctx->r2 | 0;\n    tooie_widescreen_apply_profile(ctx);\n    glglobalsettings_entrypoint_4(rdram, ctx);',after)
        self.assertEqual(after.replace('    tooie_widescreen_apply_profile(ctx);\n',''),source)
        with self.assertRaises(ValueError):module.apply_widescreen_profile(after)
        with self.assertRaises(ValueError):module.apply_widescreen_profile(source.replace('ctx->r4 = ctx->r2 | 0;','ctx->r4 = 0 | 0;'))
    def test_presence_counts_allow_single_function_fixture(self):
        for name,expected in [(NAMES[0],1),(NAMES[1],4)]:
            body=re.search(r'RECOMP_FUNC void '+name+r'\([^\n]*\n.*?^;}\n',self.source,re.S|re.M)[0]
            self.assertEqual(module.observe_global_settings(body)[1],expected)
    def test_wrong_original_pc_rejected(self):
        for pc in ['808001A8','808001AC','8080022C','80800230','80800240','80800244','80800250','80800254']:
            with self.subTest(pc=pc),self.assertRaises(ValueError):module.observe_global_settings(self.source.replace('// 0x'+pc+':','// 0xDEADBEEF:'))
    def test_wrong_call_or_delay_operation_rejected(self):
        for old,new in [('ctx->r5 = ctx->r17 | 0;','ctx->r5 = ctx->r18 | 0;'),('ctx->r4 = ADD32(ctx->r29, 0X18);','ctx->r4 = ADD32(ctx->r29, 0X14);'),('_glglobaldata_entrypoint_2(rdram, ctx);','wrong_import(rdram, ctx);')]:
            with self.subTest(old=old),self.assertRaises(ValueError):module.observe_global_settings(self.source.replace(old,new))
    def test_duplicate_function_and_repeated_transform_rejected(self):
        with self.assertRaises(ValueError):module.observe_global_settings(self.source+self.source)
        after,_=module.observe_global_settings(self.source)
        with self.assertRaises(ValueError):module.observe_global_settings(after)
    def test_full_pass_preserves_existing_callsite_order(self):
        helper_spec=importlib.util.spec_from_file_location('callsite_test',APP/'tests/instrument_continuous_test.py')
        helper=importlib.util.module_from_spec(helper_spec);helper_spec.loader.exec_module(helper)
        functions=[*helper.SECTIONS[0]['functions'],
            {'name':NAMES[0],'vram':0x80800168,'size':0xB0},
            {'name':NAMES[1],'vram':0x80800218,'size':0x60},
            {'name':'_glglobaldata_entrypoint_2','vram':0x800885B8,'size':8}]
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'fixture.c';path.write_text(helper.PREREQUISITES+self.source)
            counts=module.instrument(folder,[{'functions':functions}]);text=path.read_text()
        self.assertEqual(counts['global_settings_observations'],5)
        self.assertIn('tooie_observe_global_settings(ctx, 0x80800250u);\n    tooie_overlay_callsite_push(0x80800250u);\n    _glglobaldata_entrypoint_2(rdram, ctx);\n    tooie_overlay_callsite_pop();\n    tooie_observe_global_settings(ctx, 0x80800258u);',text)

if __name__=='__main__':unittest.main(verbosity=2)
