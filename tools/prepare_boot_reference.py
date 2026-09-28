"""Generate the local core1 decompression oracle from explicit inputs."""
from pathlib import Path
import argparse,hashlib,json,zlib
from elftools.elf.elffile import ELFFile

def prepare(rom_path,decompressed_path,elf_path,output):
    output.mkdir(parents=True,exist_ok=True);rom=rom_path.read_bytes();decompressed=decompressed_path.read_bytes()
    with elf_path.open('rb') as f:
        elf=ELFFile(f);syms={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        section=elf.get_section_by_name('.core1');expected=section.data();core1_rom=syms['core1_ROM_START'];base=section['sh_addr']
        wanted=['core1_compressed_ROM_START','core1_compressed_ROM_END','core2_compressed_ROM_START','core2_compressed_ROM_END','core1_ROM_START','core1_VRAM','core1_TEXT_SIZE','core1_DATA_SIZE','core1_RODATA_SIZE','core1_BSS_SIZE','core1_BSS_START','D_8000E800','D_800064F0','D_80006F48','D_80004240']
        symbols={k:syms[k] for k in wanted if k in syms};boot_symbols={k:v for k,v in syms.items() if k.startswith('boot_') and ('Clock' in k or 'Pi' in k or 'finalrom' in k or 'ExceptionPreamble' in k)}
    if expected!=decompressed[core1_rom:core1_rom+len(expected)]:raise RuntimeError('ELF/decompressed core1 mismatch')
    def crc(data):
        a=b=0
        for byte in data:a=(a+byte)&0xffffffff;b^=(byte<<(a&0x17))&0xffffffff
        return [a,b]
    start=syms['core1_compressed_ROM_START'];end=syms['core1_compressed_ROM_END'];offset=start;dest=base;combined=b'';streams=[]
    for name in ['text','data_rodata']:
        length=int.from_bytes(rom[offset:offset+2],'big')*16;inflater=zlib.decompressobj(-15);data=inflater.decompress(rom[offset+2:end])+inflater.flush()
        if not inflater.eof or len(data)!=length:raise RuntimeError(f'core1 {name} inflate mismatch')
        consumed=end-offset-2-len(inflater.unused_data);streams.append(dict(name=name,rom_start=offset,rom_end=offset+2+consumed,
            header_bytes=2,deflate_bytes=consumed,output_start=dest,output_bytes=len(data),output_end=dest+len(data),
            next_output=(dest+len(data)+15)&~15,crc=crc(data),sha256=hashlib.sha256(data).hexdigest()))
        offset+=2+consumed;dest=(dest+len(data)+15)&~15;combined+=data
    if combined!=expected or streams[0]['output_bytes']!=syms['core1_TEXT_SIZE'] or (offset+1)&~1!=end:raise RuntimeError('core1 oracle mismatch')
    (output/'core1-expected.bin').write_bytes(expected)
    reference=dict(symbols=symbols,boot_symbols=boot_symbols,streams=streams,core1=dict(rom=core1_rom,vram=base,size=len(expected),sha256=hashlib.sha256(expected).hexdigest()),
        source_sha256=hashlib.sha256(rom).hexdigest(),elf_sha256=hashlib.sha256(elf_path.read_bytes()).hexdigest(),scratch_base=0x80200000,
        consumed_rom_end=offset,dma_padding_bytes=end-offset,dma_padding_hex=rom[offset:end].hex(),core2_range=[syms['core2_compressed_ROM_START'],syms['core2_compressed_ROM_END']],checksum_words=[word for s in streams for word in s['crc']])
    (output/'boot-reference.json').write_text(json.dumps(reference,indent=2)+'\n');return reference
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--rom',required=True,type=Path);p.add_argument('--decompressed-rom',required=True,type=Path);p.add_argument('--elf',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();print(json.dumps(prepare(a.rom.resolve(),a.decompressed_rom.resolve(),a.elf.resolve(),a.output.resolve()),indent=2))
if __name__=='__main__':main()
