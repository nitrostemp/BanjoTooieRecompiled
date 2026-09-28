"""Independent core2 comparison inputs; never supplies guest execution bytes."""
from pathlib import Path
from io import BytesIO
import hashlib,json,zlib
from elftools.elf.elffile import ELFFile

def prepare_core2_reference(app,evidence, *, rom_path=None, decompressed_rom_path=None, elf_path=None):
    proj=app.parent
    rom_path=Path(rom_path) if rom_path else proj/'banjo-tooie/baserom.us.z64'
    decompressed_rom_path=Path(decompressed_rom_path) if decompressed_rom_path else proj/'banjo-tooie/decompressed.us.z64'
    elf_path=Path(elf_path) if elf_path else proj/'banjo-tooie/build/us/banjotooie_decompressed.elf'
    rom=rom_path.read_bytes()
    with BytesIO(elf_path.read_bytes()) as f:
        elf=ELFFile(f)
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        section=elf.get_section_by_name('.core2')
        expected=section.data(); base=section['sh_addr']
    start=symbols['core2_compressed_ROM_START'];end=symbols['core2_compressed_ROM_END']
    offset=start;dest=base;combined=b'';streams=[]
    for name in ['text','data_rodata']:
        size=int.from_bytes(rom[offset:offset+2],'big')*16
        inflate=zlib.decompressobj(-15)
        data=inflate.decompress(rom[offset+2:end])+inflate.flush()
        assert inflate.eof and len(data)==size
        consumed=end-offset-2-len(inflate.unused_data)
        a=b=0
        for value in data:
            a=(a+value)&0xffffffff;b^=(value<<(a&0x17))&0xffffffff
        streams.append(dict(name=name,rom_start=offset,rom_end=offset+2+consumed,
            output_start=dest,output_bytes=len(data),crc=[a,b]))
        combined+=data;offset+=2+consumed;dest=(dest+len(data)+15)&~15
    if combined!=expected or ((offset+15)&~15)!=end:
        difference=next((i for i,(a,b) in enumerate(zip(combined,expected)) if a!=b),None)
        raise RuntimeError(f'Core2 reference mismatch: combined={len(combined)} ELF={len(expected)} first={difference} consumed={offset:#x} aligned={(offset+1)&~1:#x} ROM_end={end:#x} streams={streams}')
    decompressed=decompressed_rom_path.read_bytes()
    assert expected==decompressed[symbols['core2_ROM_START']:symbols['core2_ROM_START']+len(expected)]
    record=dict(core2=dict(vram=base,size=len(expected),rom=symbols['core2_ROM_START'],
        bss_start=symbols['core2_BSS_START'],bss_end=symbols['core2_BSS_END'],sha256=hashlib.sha256(expected).hexdigest()),
        compressed_start=start,compressed_end=end,consumed_end=offset,dma_padding_hex=rom[offset:end].hex(),streams=streams,checksums=[c for s in streams for c in s['crc']],
        elf_sha256=hashlib.sha256(elf_path.read_bytes()).hexdigest(),rom_sha256=hashlib.sha256(rom).hexdigest())
    (app/'generated/core2-reference.json').write_text(json.dumps(record,indent=2)+'\n')
    (app/'generated/core2-expected.bin').write_bytes(expected)
    (evidence/'core2-reference.json').write_text(json.dumps(record,indent=2)+'\n')
    return record

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('--evidence',required=True,type=Path)
    out=parser.parse_args().evidence;out.mkdir(parents=True,exist_ok=True)
    print(json.dumps(prepare_core2_reference(Path(__file__).resolve().parents[1],out),indent=2))
