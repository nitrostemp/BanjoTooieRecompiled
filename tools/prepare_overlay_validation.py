"""Read-only original-ROM/ELF cross-checks for selected live overlay validation."""
from pathlib import Path
from io import BytesIO
import argparse,collections,hashlib,json,struct,zlib
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
from elftools.elf.elffile import ELFFile

def word(b,o): return int.from_bytes(b[o:o+4],'big')
def half(b,o): return int.from_bytes(b[o:o+2],'big')
def put(b,o,v): b[o:o+4]=(v&0xffffffff).to_bytes(4,'big')
def align(n,a): return (n+a-1)&-a

def coverage_path(proj, *, app=None, coverage=None):
    """Choose one explicit input; preserve the original project-app default."""
    return Path(coverage) if coverage is not None else (Path(app) if app is not None else proj/'TooieRecomp')/'config/startup_coverage.json'

def prepare(proj,output, *, app=None, coverage=None, source=None, rom=None, elf_path=None):
    source=Path(source) if source is not None else proj/'banjo-tooie'
    rom_path=Path(rom) if rom is not None else source/'baserom.us.z64'
    rom=rom_path.read_bytes()
    slots=tomllib.loads((source/'overlays.us.toml').read_text())['overlay']
    selected={s['name'][1:] for s in json.loads(coverage_path(proj,app=app,coverage=coverage).read_text())['sections']}
    elf_path=Path(elf_path) if elf_path is not None else source/'build/us/banjotooie_decompressed.elf';rows=[]
    with BytesIO(elf_path.read_bytes()) as stream:
        elf=ELFFile(stream);symtab=elf.get_section_by_name('.symtab');symbols=list(symtab.iter_symbols())
        by_name={s.name:s['st_value'] for s in symbols};table=by_name['overlay_table_compressed_ROM_START']
        for id,slot in enumerate(slots,1):
            name=slot.get('name')
            if name not in selected:continue
            first=table+word(rom,table+(id-1)*4);last=table+word(rom,table+id*4)
            assert first<last<=len(rom),(id,first,last)
            encoded=rom[first:last];head=bytearray(encoded[:16]);crc=[0,0]
            if head[15]&0x80:
                inf=zlib.decompressobj(-15);body=inf.decompress(encoded[18:])+inf.flush()
                assert inf.eof and len(body)==half(encoded,16)*16,(id,'inflate')
                for value in body:
                    crc[0]=(crc[0]+value)&0xffffffff;crc[1]^=(value<<(crc[0]&0x17))&0xffffffff
                put(head,0,word(head,0)^crc[0]);put(head,8,word(head,8)^crc[1])
            else:body=encoded[16:]
            image=head+body
            entries=half(image,8);count=half(image,10);secondary=half(image,12)
            records=align(0x38+entries*4+image[14],4);secondary_off=align(records+count*2,4)
            text=align(secondary_off+secondary*4,16)
            size=sum(half(image,o)*16 for o in (0,2,4));bss=half(image,6)*16
            assert secondary==0,(id,'selected secondary stream needs independent validator support')
            assert text+size<=len(image),(id,text,size,len(image))
            key=word(rom,0x40+id*4)&0xffff
            packed=[half(image,records+i*2)^key for i in range(count)]
            canonical=bytearray(image[text:text+size]);base=0x80800000
            for record in packed:
                off=record&~3;kind=record&3;assert off+4<=len(canonical)
                value=word(canonical,off)
                if kind==0:value|=base
                elif kind==1:value|=(base&0x0fffffff)>>2
                elif kind==2:value|=base>>16
                put(canonical,off,value)
            sec=elf.get_section_by_name('.'+name);assert sec['sh_addr']==base and canonical==sec.data(),(id,'canonical raw source differs from ELF')
            own={elf.get_section_index('.'+name),elf.get_section_index('.'+name+'_bss')}
            relsec=elf.get_section_by_name('.rel.'+name);elf_records=[]
            if relsec:
                for rel in relsec.iter_relocations():
                    if symbols[rel['r_info_sym']]['st_shndx'] in own:
                        elf_records.append((rel['r_offset']-base)|{2:0,4:1,5:2,6:3}[rel['r_info_type']])
            assert collections.Counter(elf_records)==collections.Counter(packed),(id,'packed/ELF relocation mismatch')
            relocation=[];hi=None
            for i,record in enumerate(packed):
                off=record&~3;kind=record&3;value=word(canonical,off)
                if kind==0:target=value
                elif kind==1:target=(value&0x03ffffff)*4|(base&0xf0000000)
                elif kind==2:
                    hi=(value&0xffff)<<16
                    assert i+1<len(packed) and packed[i+1]&3==3
                    low=word(canonical,packed[i+1]&~3)&0xffff
                    target=(hi+(low if low<0x8000 else low-0x10000))&0xffffffff
                else:
                    assert hi is not None
                    low=value&0xffff;target=(hi+(low if low<0x8000 else low-0x10000))&0xffffffff
                relocation.append(dict(offset=off,kind=kind,target=target))
            rows.append(dict(id=id,name=name,rom_start=first,rom_end=last,original_key=key,crc=crc,
                raw_source_sha256=hashlib.sha256(encoded).hexdigest(),canonical_base=base,text_offset=text,
                text_bytes=half(image,0)*16,bss_bytes=bss,entries=entries,packed_count=count,secondary_count=secondary,
                prefix_hex=image[:text].hex(),original_image_hex=image[text:text+size].hex(),canonical_image_hex=canonical.hex(),relocations=relocation,
                relocation_counts=[sum(r['kind']==k for r in relocation) for k in range(4)]))
    result=dict(schema=1,source_rom_sha256=hashlib.sha256(rom).hexdigest(),source_elf_sha256=hashlib.sha256(elf_path.read_bytes()).hexdigest(),overlays=rows)
    output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({r['id']:{k:r[k] for k in ('name','rom_start','original_key','text_offset','relocation_counts')} for r in rows},indent=2))
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--app',type=Path)
    parser.add_argument('--coverage',type=Path)
    args=parser.parse_args()
    prepare(Path(__file__).resolve().parents[2],args.output,app=args.app,coverage=args.coverage)
