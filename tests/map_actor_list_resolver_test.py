from pathlib import Path
import importlib.util,json,subprocess,sys,tempfile,unittest
APP=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('resolver',APP/'tools/resolve_map_actor_list.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):cls.registry=m.Registry(APP.parent/'banjo-tooie/build/us/banjotooie_decompressed.elf')
 def event(self):
  rows=[]
  for i,marker in enumerate([0x111,0x111,0,0xffff]):
   b=bytearray(20);b[8:10]=marker.to_bytes(2,'big');rows.append(dict(index=i,record_address=f'0x{0x80300000+i*20:08X}',marker_id=marker,record20_hex=b.hex()))
  return dict(event='map_actor_list_planned',complete=True,reason='',overlay_id=727,original_function='gspropsDll_entrypoint_1',guest_pc='0x808001E0',array_address='0x80200000',requested_count=4,record_count=4,records=rows,current_map=0x14f,guest_memory_modified=False,guest_registers_modified=False,constructor_completion_claim=False)
 def test_actual_registry_duplicate_preservation(self):
  r=m.resolve(self.event(),self.registry)
  self.assertEqual(r['recommended_ids'],[471]);self.assertEqual(r['records'][0]['stable_id'],471)
  self.assertEqual(r['records'][1]['stable_id'],471);self.assertEqual(r['records'][0]['section'],'.chwallsnakebaddy')
  self.assertEqual(r['records'][2]['kind'],'out_of_registry_range');self.assertEqual(len(r['records']),4)
 def test_incomplete_and_malformed_rejected(self):
  for k,v in [('complete',False),('requested_count',501),('requested_count',-1),('record_count',3),('reason','invalid_record'),('guest_pc','0x808001E4'),('guest_memory_modified',True),('constructor_completion_claim',True),('array_address','0x00200000')]:
   e=self.event();e[k]=v
   with self.subTest(k=k),self.assertRaises(ValueError):m.resolve(e,self.registry)
  for k,v in [('marker_id',0x112),('index',1),('record_address','0x807ffff0'),('record20_hex','00')]:
   e=self.event();e['records'][0][k]=v
   with self.subTest(k=k),self.assertRaises(ValueError):m.resolve(e,self.registry)
 def test_empty(self):
  e=self.event();e.update(requested_count=0,record_count=0,records=[],array_address='0x00000000')
  self.assertEqual(m.resolve(e,self.registry)['recommended_ids'],[])
 def test_original_elf_identity_required(self):
  with self.assertRaises(ValueError):m.Registry(APP/'src/map_actor_list.hpp')
 def test_cli_identity_map_and_exclusive_output(self):
  with tempfile.TemporaryDirectory() as folder:
   f=Path(folder);trace=f/'trace.jsonl';out=f/'proof.json'
   header=dict(event='run_header',sequence=1,rom_sha256=m.ROM_SHA,executable_sha256='a'*64,build_metadata_identity='b'*64)
   store=dict(event='title_current_map_stored',sequence=2,global_map_target=True,store_valid=True,stored_map=0x14f)
   event=self.event();event['sequence']=3
   def write():trace.write_text('\n'.join(map(json.dumps,[header,store,event]))+'\n')
   write();cmd=[sys.executable,str(APP/'tools/resolve_map_actor_list.py'),'--trace',str(trace),'--elf',str(self.registry.path),'--sequence','3','--expect-exe-sha256','a'*64,'--expect-metadata-identity','b'*64,'--output',str(out)]
   result=subprocess.run(cmd,capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stderr)
   self.assertEqual(json.loads(out.read_text())['recommended_ids'],[471]);original=out.read_bytes()
   self.assertNotEqual(subprocess.run(cmd,capture_output=True).returncode,0);self.assertEqual(out.read_bytes(),original)
   cmd[-1]=str(f/'rejected.json');header['executable_sha256']='c'*64;write()
   self.assertNotEqual(subprocess.run(cmd,capture_output=True).returncode,0);self.assertFalse(Path(cmd[-1]).exists())
   header['executable_sha256']='a'*64;store['stored_map']=0x158;write()
   self.assertNotEqual(subprocess.run(cmd,capture_output=True).returncode,0);self.assertFalse(Path(cmd[-1]).exists())
   store['stored_map']=0x14f
   # A lower sequence physically after the snapshot is not a preceding store.
   trace.write_text('\n'.join(map(json.dumps,[header,event,store]))+'\n')
   self.assertNotEqual(subprocess.run(cmd,capture_output=True).returncode,0);self.assertFalse(Path(cmd[-1]).exists())
   for bad_sequence in [True,1,4]:
    store['sequence']=bad_sequence;write()
    self.assertNotEqual(subprocess.run(cmd,capture_output=True).returncode,0);self.assertFalse(Path(cmd[-1]).exists())
if __name__=='__main__':unittest.main(verbosity=2)
