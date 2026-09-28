import importlib.util,re,tempfile,unittest
from pathlib import Path
APP=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('instrument',APP/'tools/instrument_continuous.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  found=[]
  for p in (APP/'generated').glob('*.c'):
   found+=re.findall(r'RECOMP_FUNC void gspropsDll_entrypoint_1\([^\n]*\n.*?^;}\n',p.read_text(),re.S|re.M)
  assert len(found)==1
  cls.body=re.sub(r'^    tooie_(?:continuous_poll|overlay_callsite_push|overlay_callsite_pop|observe_map_actor_list)\([^\n]*\n','',found[0],flags=re.M)
 def test_roundtrip(self):
  new,n=m.observe_map_actor_list(self.body);self.assertEqual(n,1)
  self.assertEqual(re.sub(r'^    tooie_observe_map_actor_list\([^\n]*\n','',new,flags=re.M),self.body)
  self.assertIn('L_808001E0:\n    tooie_observe_map_actor_list(rdram, ctx, 0x808001E0u);\n    // 0x808001E0: blez',new)
 def test_exact_function_body_fail_closed(self):
  for old,new in [('ctx->r20 = ctx->r5 | 0;','ctx->r20 = 1;'),('0x808001E0: blez','0x808001E0: bgtz'),('func_80108DC0(rdram, ctx);','other(rdram, ctx);'),('ctx->r4 = MEM_HU(ctx->r17, 0X8);','ctx->r4 = MEM_HU(ctx->r17, 0XA);')]:
   self.assertIn(old,self.body)
   with self.subTest(old=old),self.assertRaises(ValueError):m.observe_map_actor_list(self.body.replace(old,new))
  with self.assertRaises(ValueError):m.observe_map_actor_list(self.body+self.body)
  with self.assertRaises(ValueError):m.observe_map_actor_list(m.observe_map_actor_list(self.body)[0])
 def test_scope(self):
  renamed=self.body.replace('void gspropsDll_entrypoint_1(','void other(')
  self.assertEqual(m.observe_map_actor_list(renamed),(renamed,0))
 def test_instrument_counts(self):
  spec=importlib.util.spec_from_file_location('helper',APP/'tests/instrument_continuous_test.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
  for present in [False,True]:
   funcs=list(h.SECTIONS[0]['functions'])
   if present:funcs+=[dict(name='gspropsDll_entrypoint_1',vram=0x808000a0,size=0x204)]
   with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'fixture.c';p.write_text(h.PREREQUISITES+(self.body if present else ''))
    self.assertEqual(m.instrument(d,[{'functions':funcs}])['map_actor_list_observations'],int(present))
    if present:self.assertIn('tooie_continuous_poll(rdram, ctx, 0x808001E0u);\n    tooie_observe_map_actor_list',p.read_text())
if __name__=='__main__':unittest.main(verbosity=2)
