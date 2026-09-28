"""Generate the representative overlay test fixture from local decomp outputs."""
from pathlib import Path
from elftools.elf.elffile import ELFFile
import argparse,struct
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
def prepare(decomp,selected_path,output,elf_path=None):
    sections=tomllib.loads(selected_path.read_text())['section'];section=next(s for s in sections if s['name']=='.chweldarbossfireball')
    rom=(decomp/'decompressed.us.z64').read_bytes();data=rom[section['rom']:section['rom']+section['size']];words=list(struct.unpack('>'+'I'*(len(data)//4),data))
    raw=(decomp/'assets/overlays/chweldarbossfireball/relocs.bin').read_bytes();packed=struct.unpack('<'+'H'*(len(raw)//2),raw);targets=[];hi=None
    for i,p in enumerate(packed):
        kind,off=p&3,p&~3;word=words[off//4]
        if kind==0:target=word
        elif kind==1:target=((word&0x3ffffff)<<2)|0x80000000
        elif kind==2:
            hi=(word&0xffff)<<16
            if i+1>=len(packed) or packed[i+1]&3!=3:raise RuntimeError('HI16 without LO16')
            lo=words[(packed[i+1]&~3)//4]&0xffff;target=(hi+(lo if lo<0x8000 else lo-0x10000))&0xffffffff
        else:
            if hi is None:raise RuntimeError('LO16 without HI16')
            lo=word&0xffff;target=(hi+(lo if lo<0x8000 else lo-0x10000))&0xffffffff
        targets.append(target)
    elf_path=elf_path or decomp/'build/us/banjotooie_decompressed.elf'
    with elf_path.open('rb') as stream:
        elf=ELFFile(stream);symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    bss_size=symbols['chweldarbossfireball_BSS_SIZE']
    fixture=['/* Generated from the local user-owned ROM; do not commit. */',f'#define OVERLAY_SECTION {sections.index(section)}',f'#define OVERLAY_VRAM 0x{section["vram"]:08X}u',f'#define OVERLAY_BSS_SIZE {bss_size}',
        'static const uint32_t original_words[] = {'+','.join(f'0x{x:08X}u' for x in words)+'};','static const uint16_t packed_relocs[] = {'+','.join(f'0x{x:04X}' for x in packed)+'};','static const uint32_t reloc_targets[] = {'+','.join(f'0x{x:08X}u' for x in targets)+'};']
    output.write_text('\n'.join(fixture)+'\n')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--decomp-root',required=True,type=Path);p.add_argument('--symbols',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--elf',type=Path);a=p.parse_args();prepare(a.decomp_root.resolve(),a.symbols.resolve(),a.output.resolve(),a.elf.resolve() if a.elf else None)
if __name__=='__main__':main()
