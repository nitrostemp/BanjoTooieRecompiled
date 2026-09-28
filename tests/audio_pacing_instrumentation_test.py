"""Read-only transform tests against the locally generated pristine fixture."""
from pathlib import Path
import importlib.util,re,unittest
APP=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('pacing',APP/'tools/instrument_audio_pacing.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
FIXTURE=APP/'generated/test-fixtures/audio_pacing_functions.inc'
if not FIXTURE.is_file():
 raise RuntimeError(f'missing local generated fixture: {FIXTURE}; run tools/generate_local.py')
SOURCE=FIXTURE.read_text()
class Tests(unittest.TestCase):
 def test_exact_four_and_roundtrip(self):
  result,count=m.observe_audio_pacing(SOURCE);self.assertEqual(count,4)
  self.assertEqual(re.sub(r'^    tooie_observe_audio_pacing\([^\n]*\n','',result,flags=re.M),SOURCE)
  for pc in ['80012934','80012998','80012A2C','80012AF0']:
   self.assertIn(f'tooie_observe_audio_pacing(rdram, ctx, 0x{pc}u);\n    // 0x{pc}:',result)
 def test_changed_instruction_register_and_delay(self):
  for old,new in [('0x80012A24:','0x80012A20:'),('ctx->r6 = ADD32(0, 0X1);','ctx->r6 = ADD32(0, 0X0);'),('ctx->r3 = S32(U32(ctx->r2) >> 2);','ctx->r3 = S32(U32(ctx->r2) >> 1);'),('0X115','0X116')]:
   with self.subTest(old=old):
    self.assertIn(old,SOURCE)
    with self.assertRaises(ValueError):m.observe_audio_pacing(SOURCE.replace(old,new))
 def test_duplicate_double_transform(self):
  with self.assertRaises(ValueError):m.observe_audio_pacing(SOURCE+SOURCE)
  with self.assertRaises(ValueError):m.observe_audio_pacing(m.observe_audio_pacing(SOURCE)[0])
 def test_unrelated_zero_partial_scope(self):
  self.assertEqual(m.observe_audio_pacing('nothing'),('nothing',0))
  for name in ['func_800128C0','func_800129FC']:
   body=re.search(r'RECOMP_FUNC void '+name+r'\([^\n]*\n.*?^;}\n',SOURCE,re.M|re.S)[0]
   self.assertEqual(m.observe_audio_pacing(body)[1],2)
   renamed=body.replace('void '+name+'(','void unrelated(')
   self.assertEqual(m.observe_audio_pacing(renamed),(renamed,0))
if __name__=='__main__':unittest.main(verbosity=2)
