"""Derive S1-B3 boundaries/arguments from the pinned linked ELF."""
from pathlib import Path
from io import BytesIO
import hashlib,json
from elftools.elf.elffile import ELFFile

def prepare_main_metadata(app: Path, evidence: Path, *, elf_path: Path | None = None):
    path=elf_path or app.parent/'banjo-tooie/build/us/banjotooie_decompressed.elf'
    with BytesIO(path.read_bytes()) as f:
        elf=ELFFile(f)
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        section=elf.get_section_by_name('.core1')
        base,data=section['sh_addr'],section.data()
    def word(pc):return int.from_bytes(data[pc-base:pc-base+4],'big')
    instructions={0x8001257C:0xAFA00010,0x80012584:0x24060006,0x80012564:0x240F0014,
        0x800131DC:0x24060001,0x800131F4:0x24060010,0x80013208:0x24040096,
        0x80013210:0x24070010,0x8001DD70:0xAC90001C,0x8001DD74:0x24180001,
        0x8001DD78:0xAE18000C,0x8001DD7C:0xAE000008,0x8001DD80:0xAE000004,
        0x80013698:0x0C00B944,0x8001369C:0x00402025,0x800124EC:0x27BDFFE8}
    for pc,expected in instructions.items():assert word(pc)==expected,(hex(pc),hex(word(pc)),hex(expected))
    values=dict(main_thread=symbols['D_80045788'],main_odd_storage=symbols['D_80045938'],
        main_stack_start=symbols['D_80043388'],main_stack_top=symbols['D_80045788'],
        main_entry=symbols['func_800124EC'],main_id=word(0x80012584)&0xffff,
        main_priority=word(0x80012564)&0xffff,main_arg=0,
        queue1=symbols['D_8007695C'],queue1_buffer=symbols['D_80076958'],queue1_capacity=word(0x800131DC)&0xffff,
        queue2=symbols['D_800769B8'],queue2_buffer=symbols['D_80076978'],queue2_capacity=word(0x800131F4)&0xffff,
        pi_priority=word(0x80013208)&0xffff,main_record_end=symbols['D_80045938'],main_odd_end=symbols['D_800459C8'])
    values['main_stack_size']=values['main_stack_top']-values['main_stack_start']
    values['main_first_sp']=values['main_stack_top']-0x10
    assert values['main_stack_size']==0x2400
    header='#pragma once\n#include <cstdint>\nnamespace tooie::main_meta {\n'
    header+=''.join(f'inline constexpr uint32_t {k}=0x{v:08X}u;\n' for k,v in values.items())+'}\n'
    dest=app/'generated/main_metadata.hpp'
    dest.write_text(header)
    evidence.mkdir(parents=True,exist_ok=True)
    (evidence/'main-metadata.json').write_text(json.dumps(dict(elf=str(path.resolve()),elf_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),constants=values,instruction_checks={hex(k):hex(v) for k,v in instructions.items()},header_sha256=hashlib.sha256(dest.read_bytes()).hexdigest()),indent=2)+'\n')
