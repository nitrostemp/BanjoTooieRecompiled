"""Derive bounded core1 constants from the retained ELF; no guest-memory writes."""
from pathlib import Path
from io import BytesIO
import hashlib
import json
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
from elftools.elf.elffile import ELFFile


def prepare_core1_metadata(app: Path, evidence: Path, *, elf_path: Path | None = None,
                           baseline_symbols: Path | None = None):
    elf_path = elf_path or app.parent / 'banjo-tooie/build/us/banjotooie_decompressed.elf'
    with BytesIO(elf_path.read_bytes()) as stream:
        elf = ELFFile(stream)
        syms = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        core1 = elf.get_section_by_name('.core1')
        base, data = core1['sh_addr'], core1.data()
    def word(pc):
        offset = pc - base
        assert 0 <= offset <= len(data) - 4
        return int.from_bytes(data[offset:offset + 4], 'big')
    # Reject a changed instruction sequence instead of carrying forward stale
    # immediate/field semantics. These are independently disassembled in core-audit.md.
    expected = {
        0x80013648: 0xAFAE0014, 0x8001364C: 0xAFA00010,
        0x80013650: 0xAFA00018, 0x80013658: 0x24060001,
        0x8001DCF8: 0xAC90001C, 0x8001DCFC: 0xAE00000C,
        0x8001DD00: 0xAE000008, 0x8001DD04: 0xAE000004,
        0x800318C8: 0x3408FF03, 0x800318F0: 0x35CE0800,
        0x8002D7D4: 0x3C012000, 0x8002D7E0: 0x3C040100,
        0x8002D7E8: 0x34840800,
    }
    for pc, instruction in expected.items():
        assert word(pc) == instruction, (hex(pc), hex(word(pc)), hex(instruction))
    def materialized(hi_pc, lo_pc):
        hi, lo = word(hi_pc), word(lo_pc)
        assert hi >> 26 == 15  # LUI
        assert lo >> 26 in (9, 13)  # ADDIU or ORI
        low = lo & 0xFFFF
        if lo >> 26 == 9 and low & 0x8000:
            low -= 0x10000
        return (((hi & 0xFFFF) << 16) + low) & 0xFFFFFFFF
    values = {
        'bss_start': syms['core1_BSS_START'], 'bss_end': syms['core1_BSS_END'],
        'bss_size': syms['core1_BSS_SIZE'], 'core1_entry': syms['func_80012030'],
        'os_initialize': syms['osInitialize'], 'finalrom': syms['__osFinalrom'],
        'exception_preamble': syms['__osExceptionPreamble'],
        'clock_rate': syms['osClockRate'], 'vi_clock': syms['osViClock'],
        'pi_dom1_type': syms['__Dom1SpeedParam'] + 4,
        'pi_dom2_type': syms['__Dom2SpeedParam'] + 4,
        'tv_type': syms['osTvType'], 'reset_type': syms['osResetType'],
        'nmi_buffer': syms['osAppNMIBuffer'], 'global_int_mask': syms['__OSGlobalIntMask'],
        'idle_thread': syms['D_800775F0'], 'idle_odd_storage': syms['D_800777A0'],
        'idle_entry': syms['func_80013678'], 'idle_stack_start': syms['D_800773F0'],
        'idle_stack_top': materialized(0x80013624, 0x8001362C),
        'idle_id': word(0x80013658) & 0xFFFF,
        'idle_priority': 0,  # SW ZERO into argument slot, checked above.
        'idle_arg': 0,  # SW ZERO into argument slot, checked above.
        'thread_table': syms['D_8007D820'], 'odd_save_offset': word(0x8001DCF8) & 0xFFFF,
        'first_ra': materialized(0x80031880, 0x800318A4),
        'saved_sr': word(0x800318C8) & 0xFFFF,
        'fcsr': materialized(0x8002D7E0, 0x8002D7E8),
        'cu1_mask': (word(0x8002D7D4) & 0xFFFF) << 16,
    }
    if baseline_symbols is None:
        baseline_symbols = app / 'config/selected.syms.toml'
    original_sections = tomllib.loads(baseline_symbols.read_text())['section']
    init_functions = [f for s in original_sections for f in s['functions']
                      if f['name'] in ('osInitialize', 'tooie_core1_osInitialize')
                      and f['vram'] == values['os_initialize']]
    assert len(init_functions) == 1
    values['os_initialize_size'] = init_functions[0]['size']
    values['idle_stack_size'] = values['idle_stack_top'] - values['idle_stack_start']
    values['first_sp'] = values['idle_stack_top'] - 0x10
    values['first_sr'] = values['saved_sr'] & ~2  # ERET clears EXL.
    assert values['bss_size'] == values['bss_end'] - values['bss_start']
    assert values['first_ra'] == syms['__osCleanupThread']
    assert values['idle_stack_size'] == 0x200
    assert materialized(0x80013630, 0x80013644) == values['idle_thread']
    assert materialized(0x80013634, 0x80013640) == values['idle_odd_storage']
    assert materialized(0x80013638, 0x8001363C) == values['idle_entry']
    header = '#pragma once\n#include <cstdint>\nnamespace tooie::core1_meta {\n'
    header += ''.join(f'inline constexpr uint32_t {key} = 0x{value:08X}u;\n' for key, value in values.items())
    header += '}\n'
    output = app / 'generated/core1_metadata.hpp'
    output.parent.mkdir(exist_ok=True)
    output.write_text(header)
    evidence.mkdir(parents=True, exist_ok=True)
    record = dict(elf=str(elf_path.resolve()),
                  elf_sha256=hashlib.sha256(elf_path.read_bytes()).hexdigest(),
                  constants=values, instruction_checks={f'{pc:08X}': f'{value:08X}' for pc, value in expected.items()},
                  header_sha256=hashlib.sha256(output.read_bytes()).hexdigest())
    (evidence / 'core1-metadata.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


if __name__ == '__main__':
    import argparse
    app = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, default=app.parent / 'step1/track-a/s1b2-continuation')
    prepare_core1_metadata(app, parser.parse_args().evidence.resolve())
