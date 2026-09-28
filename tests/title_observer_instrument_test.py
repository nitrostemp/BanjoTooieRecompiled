"""Exact source anchors for title state; generated inputs read only."""
import importlib.util,re,tempfile,unittest
from pathlib import Path
APP=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('instrument',APP/'tools/instrument_continuous.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
NAMES={'func_808005AC_chintroticker':6,'func_80800738_gcfrontend':1,'gsworldDll_entrypoint_2':1,'gsattract_entrypoint_1':1}
def originals():
    found={}
    for p in (APP/'generated').glob('*.c'):
        for match in re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n',p.read_text(),re.S|re.M):
            if match[1] in NAMES:
                if match[1] in found:raise AssertionError('duplicate actual function')
                body = re.sub(r'^    tooie_(?:continuous_poll|overlay_callsite_push|overlay_callsite_pop|observe_title|scene_world_call_started|scene_world_call_finished)\([^\n]*\n','',match[0],flags=re.M)
                body = re.sub(r'^    ctx->r2 = tooie_cutscene_skip_input\(\(int\)ctx->r2\);\n', '', body, flags=re.M)
                found[match[1]] = body
    assert set(found)==set(NAMES)
    return found
class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.parts=originals();cls.source=''.join(cls.parts.values())
    def test_nine_hooks_exact_roundtrip_and_order(self):
        changed,count=m.observe_title(self.source);self.assertEqual(count,9)
        self.assertEqual(re.sub(r'^    tooie_observe_title\([^\n]*\n','',changed,flags=re.M),self.source)
        self.assertIn('MEM_H(0X2, ctx->r4) = ctx->r5;\n    tooie_observe_title(rdram, ctx, 0x808002C4u);',changed)
        self.assertIn('ctx->r6 = ADD32(0, 0X1);\n    tooie_observe_title(rdram, ctx, 0x808007ACu);\n    func_800A791C',changed)
        self.assertIn('tooie_observe_title(rdram, ctx, 0x8080066Cu);\n    _gcfrontend_entrypoint_10',changed)
        self.assertIn('MEM_W(0X2C, ctx->r19) = ctx->f10.u32l;\n    tooie_observe_title(rdram, ctx, 0x808006C4u);',changed)
        self.assertIn('MEM_W(0X4, ctx->r5) = ctx->f8.u32l;\n    tooie_observe_title(rdram, ctx, 0x80800050u);',changed)
    def test_scope_duplicate_and_double_transform(self):
        for name,n in NAMES.items():
            body=self.parts[name];self.assertEqual(m.observe_title(body)[1],n)
            renamed=body.replace('void '+name+'(','void unrelated(');self.assertEqual(m.observe_title(renamed),(renamed,0))
        with self.assertRaises(ValueError):m.observe_title(self.source+self.source)
        with self.assertRaises(ValueError):m.observe_title(m.observe_title(self.source)[0])
    def test_pc_instruction_delay_target_changes_fail_closed(self):
        mutations=[('0x80800614: lbu','0x80800610: lbu'),('ctx->r2 = MEM_BU(ctx->r19, 0X79);','ctx->r2 = MEM_BU(ctx->r19, 0X78);'),
            ('0x80800644: beq','0x80800644: bne'),('0x80800654: bne','0x80800654: beq'),
            ('0x80800664: bne','0x80800664: beq'),('_gcfrontend_entrypoint_10(rdram, ctx);','wrong(rdram, ctx);'),
            ('func_800DA298(rdram, ctx);','wrong_flag(rdram, ctx);'),('func_800A8264(rdram, ctx);','wrong_warp(rdram, ctx);'),
            ('func_80015FA0(rdram, ctx);','wrong_start(rdram, ctx);'),('func_800EA05C(rdram, ctx);','wrong_map(rdram, ctx);'),
            ('ctx->r6 = ADD32(0, 0X1);','ctx->r6 = ADD32(0, 0X2);'),('MEM_H(0X2, ctx->r4) = ctx->r5;','MEM_H(0X4, ctx->r4) = ctx->r5;'),
            ('MEM_W(0X2C, ctx->r19) = ctx->f10.u32l;','MEM_W(0X28, ctx->r19) = ctx->f10.u32l;'),
            ('MEM_W(0X4, ctx->r5) = ctx->f8.u32l;','MEM_W(0X8, ctx->r5) = ctx->f8.u32l;')]
        for old,new in mutations:
            with self.subTest(old=old),self.assertRaises(ValueError):m.observe_title(self.source.replace(old,new))
    def test_counts_partial_and_zero(self):
        self.assertEqual(m.observe_title('unrelated text'),('unrelated text',0))
        spec=importlib.util.spec_from_file_location('helper',APP/'tests/instrument_continuous_test.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
        for include,expected in [(False,0),(True,9)]:
            funcs=list(h.SECTIONS[0]['functions'])
            if include:
                funcs += [{'name':name,'vram':0x80800000,'size':0x800} for name in NAMES]
                funcs += [{'name':'_gcfrontend_entrypoint_10','vram':0x80088180,'size':8}]
            with tempfile.TemporaryDirectory() as folder:
                (Path(folder)/'fixture.c').write_text('#include "funcs.h"\n'+h.PREREQUISITES+(self.source if include else ''))
                self.assertEqual(m.instrument(folder,[{'functions':funcs}])['title_observations'],expected)
                if include:self.assertIn('tooie_observe_title(rdram, ctx, 0x8080066Cu);\n    tooie_overlay_callsite_push(0x8080066Cu);\n    _gcfrontend_entrypoint_10', (Path(folder)/'fixture.c').read_text())
if __name__=='__main__':unittest.main(verbosity=2)
