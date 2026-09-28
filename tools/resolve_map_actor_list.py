"""Resolve only a complete observed map list against the exact original registry.

Writes a NEW proof, never edits coverage. Does not invoke guest getters, enumerate
candidate actors from the registry, or follow constructor/update descendants.
"""
import argparse,hashlib,json,re,struct
from pathlib import Path
ELF_SHA='331fe74764f14ef480688da128bb0c75ee388c416e6af7de38beff7defb8373f'
ROM_SHA='9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583'
def require(ok,message):
 if not ok:raise ValueError(message)
def integer(x):return type(x) is int
def file_hash(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
 return h.hexdigest()
def address(value,n,alignment=4):
 require(isinstance(value,str) and re.fullmatch(r'0x[0-9a-fA-F]{8}',value),'invalid address encoding')
 a=int(value,16);require(a&0xe0000000 in (0x80000000,0xa0000000) and a%alignment==0 and (a&0x1fffffff)+n<=0x800000,'invalid guest range');return a
class Registry:
 def __init__(self,path):
  self.path=Path(path);self.data=self.path.read_bytes();require(hashlib.sha256(self.data).hexdigest()==ELF_SHA,'original ELF identity mismatch')
  h=struct.unpack_from('>16sHHIIIIIHHHHHH',self.data)
  sh=[struct.unpack_from('>IIIIIIIIII',self.data,h[6]+i*h[11]) for i in range(h[12])]
  s=sh[h[13]];names=self.data[s[4]:s[4]+s[5]];name=lambda s:names[s[0]:names.index(b'\0',s[0])].decode()
  self.sections={name(s):s for s in sh};self.stub_names={}
  for s in sh:
   if s[1]!=2:continue
   st=sh[s[6]];strings=self.data[st[4]:st[4]+st[5]]
   for off in range(s[4],s[4]+s[5],s[9]):
    n,v,size,info,other,sec=struct.unpack_from('>IIIBBH',self.data,off)
    if not n:continue
    sym=strings[n:strings.index(b'\0',n)].decode();m=re.fullmatch(r'_(.+)_entrypoint_(\d+)',sym)
    if m and size==8 and sec<len(sh) and name(sh[sec])=='.core2':self.stub_names[v]=(m[1],int(m[2]))
 def word(self,sec,pc):
  s=self.sections[sec];require(s[3]<=pc and pc+4<=s[3]+s[5],'ELF range');return struct.unpack_from('>I',self.data,s[4]+pc-s[3])[0]
 def lookup(self,marker):
  if not 0xb6<=marker<0x546:return dict(kind='out_of_registry_range')
  row=0x80800090+4*(marker-0xb6);target=self.word('.gemarkersDll',row)
  result=dict(registry_row=f'0x{row:08X}',target=f'0x{target:08X}')
  if target==0:return dict(**result,kind='null_registry_getter')
  require(target in self.stub_names,'non-stub registry target requires separate source review')
  a,b=self.word('.core2',target),self.word('.core2',target+4)
  require(a&0xfc00003f==12 and b>>16 in (0x2008,0x2408,0x3408,0x3808) and (b&0xffff)%4==0,'unrecognized original overlay stub')
  name,entry=self.stub_names[target];require(entry==(b&0xffff)//4,'stub symbol/entry disagreement')
  return dict(**result,kind='overlay_descriptor',stable_id=(a>>6)&0xfffff,section='.'+name,entry_index=entry,entry_offset_bytes=b&0xffff,stub_words=[f'{a:08X}',f'{b:08X}'])
def resolve(event,registry):
 require(event.get('event')=='map_actor_list_planned' and event.get('complete') is True and event.get('reason')=='','incomplete or wrong event')
 require(event.get('overlay_id')==727 and event.get('original_function')=='gspropsDll_entrypoint_1' and event.get('guest_pc')=='0x808001E0','wrong original hook')
 require(event.get('guest_memory_modified') is False and event.get('guest_registers_modified') is False and event.get('constructor_completion_claim') is False,'wrong observation contract')
 count=event.get('requested_count');require(integer(count) and 0<=count<=500 and integer(event.get('record_count')) and event['record_count']==count,'invalid count')
 require(integer(event.get('current_map')) and 0<=event['current_map']<=0xffff,'invalid map')
 records=event.get('records');require(isinstance(records,list) and len(records)==count,'partial records')
 if count:address(event.get('array_address'),count*4)
 output=[]
 for i,r in enumerate(records):
  require(integer(r.get('index')) and r['index']==i,'row index/order mismatch');address(r.get('record_address'),20)
  marker=r.get('marker_id');require(integer(marker) and 0<=marker<=0xffff,'bad marker')
  raw=r.get('record20_hex');require(isinstance(raw,str) and re.fullmatch('[0-9a-fA-F]{40}',raw),'incomplete record bytes')
  require(int(raw[16:20],16)==marker,'record/marker disagreement')
  output.append(dict(**r,**registry.lookup(marker)))
 return dict(records=output,recommended_ids=sorted({r['stable_id'] for r in output if r['kind']=='overlay_descriptor'}),
             current_map=event['current_map'],record_count=count,plan_condition='earlier constructors return normally and future list/marker records remain unchanged',
             constructor_completion_claim=False,descendants_included=False)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--elf',type=Path,required=True)
 p.add_argument('--sequence',type=int,required=True);p.add_argument('--expect-exe-sha256',required=True);p.add_argument('--expect-metadata-identity',required=True);p.add_argument('--output',type=Path,required=True)
 args=p.parse_args();require(not args.output.exists(),'output exists');registry=Registry(args.elf)
 require(all(re.fullmatch('[0-9a-f]{64}',s) for s in (args.expect_exe_sha256,args.expect_metadata_identity)),'expected identity must be SHA256')
 before=file_hash(args.trace);headers=[];events=[];last_map=None;snapshot_map=None
 with args.trace.open() as f:
  for line_number,line in enumerate(f,1):
   row=json.loads(line)
   require(integer(row.get('sequence')) and row['sequence']==line_number,'trace sequence/order mismatch')
   if row.get('event')=='run_header':headers.append(row)
   if row.get('sequence')==args.sequence:
    events.append(row);snapshot_map=last_map
   if row.get('event')=='title_current_map_stored' and row.get('sequence',args.sequence)>=0 and row['sequence']<args.sequence and row.get('global_map_target') is True and row.get('store_valid') is True:last_map=row
 require(len(headers)==1 and len(events)==1,'ambiguous/missing header or snapshot');header,event=headers[0],events[0]
 require(header.get('rom_sha256')==ROM_SHA and header.get('executable_sha256')==args.expect_exe_sha256 and header.get('build_metadata_identity')==args.expect_metadata_identity,'run identity mismatch')
 require(snapshot_map and snapshot_map.get('stored_map')==event.get('current_map'),'snapshot lacks matching preceding current-map store')
 result=resolve(event,registry)
 require(file_hash(args.trace)==before,'trace changed during resolution')
 result.update(status='COMPLETE_OBSERVED_PLAN_ONLY',original_elf_sha256=ELF_SHA,trace_path=str(args.trace.resolve()),trace_sha256=before,run_header=header,snapshot_sequence=args.sequence,map_store_sequence=snapshot_map['sequence'])
 with args.output.open('x') as f:json.dump(result,f,indent=2);f.write('\n')
 print(json.dumps(dict(output=str(args.output),recommended_ids=result['recommended_ids'],record_count=result['record_count'])))
if __name__=='__main__':main()
