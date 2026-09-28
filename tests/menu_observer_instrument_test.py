import hashlib,importlib.util,inspect,os,re,tempfile,unittest
from pathlib import Path
APP=Path(__file__).resolve().parents[1]
EVIDENCE=Path(os.environ.get('TOOIE_MENU_OBSERVER_EVIDENCE',str(APP.parent/'step1/track-a/mission-01/game-select-observer-01')))
spec=importlib.util.spec_from_file_location('instrument_menu_test',APP/'tools/instrument_continuous.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  # The original bodies are ROM-derived local evidence and are never tracked.
  if not (EVIDENCE/'original-bodies').is_dir():raise unittest.SkipTest('local ROM-derived menu evidence is unavailable: '+str(EVIDENCE))
  cls.bodies={p.stem:p.read_text() for p in (EVIDENCE/'original-bodies').glob('*.inc')}
 def transform(self,text):
  self.assertTrue(hasattr(m,'observe_menu'),'missing eleven-hook menu transformer')
  return m.observe_menu(text)
 def test_eleven_exact_roundtrip(self):
  body=''.join(self.bodies.values());new,n=self.transform(body);self.assertEqual(n,11)
  self.assertEqual(re.sub(r'^    tooie_observe_menu\([^\n]*\n','',new,flags=re.M),body)
  self.assertEqual(len(re.findall(r'tooie_observe_menu\(',new)),11)
  # Observer is before the original instruction, including a JAL delay-slot setup.
  self.assertIn('tooie_observe_menu(rdram, ctx, 0x80800EB0u);\n    // 0x80800EB0: jal',new)
 def test_fail_closed_full_body(self):
  for name,body in self.bodies.items():
   self.transform(body)
   with self.subTest(name=name),self.assertRaises(ValueError):self.transform(body.replace('uint64_t hi = 0','uint64_t hi = 1',1))
   with self.assertRaises(ValueError):self.transform(body+body)
   with self.assertRaises(ValueError):self.transform(self.transform(body)[0])
 def test_unrelated_scoped_out(self):
  body=next(iter(self.bodies.values())).replace('chgameselect','another_overlay')
  self.assertEqual(self.transform(body),(body,0))
 def test_metadata_bound_relocation_ordinal(self):
  self.assertIn('section_index',inspect.signature(m.observe_menu).parameters,'menu body gating must bind relocation ordinal to selected section metadata')
  body=''.join(self.bodies.values());shifted=body.replace('RELOC_HI16(58,','RELOC_HI16(61,').replace('RELOC_LO16(58,','RELOC_LO16(61,')
  new,n=m.observe_menu(shifted,section_index=61);self.assertEqual(n,11)
  self.assertEqual(re.sub(r'^    tooie_observe_menu\([^\n]*\n','',new,flags=re.M),shifted)
  with self.assertRaises(ValueError):m.observe_menu(shifted,section_index=58)
  with self.assertRaises(ValueError):m.observe_menu(shifted.replace('RELOC_HI16(61,','RELOC_HI16(62,',1),section_index=61)
 def test_instrument_counts_small_and_full(self):
  spec=importlib.util.spec_from_file_location('helper',APP/'tests/instrument_continuous_test.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
  self.transform('')
  for bodies,expected in [([],0),([self.bodies['func_80800D3C_chgameselect']],1),(list(self.bodies.values()),11)]:
   funcs=list(h.SECTIONS[0]['functions'])
   for body in bodies:
    name=re.search(r'void (\w+)',body)[1];pc=int(re.search(r'// 0x([0-9A-F]{8}):',body)[1],16);funcs.append(dict(name=name,vram=pc,size=4))
   with tempfile.TemporaryDirectory() as d:
    p=Path(d)/'fixture.c';p.write_text(h.PREREQUISITES+''.join(bodies).replace('RELOC_HI16(58,','RELOC_HI16(0,').replace('RELOC_LO16(58,','RELOC_LO16(0,'))
    self.assertEqual(m.instrument(d,[{'name':'.chgameselect','functions':funcs}])['menu_observations'],expected)
if __name__=='__main__':unittest.main(verbosity=2)
