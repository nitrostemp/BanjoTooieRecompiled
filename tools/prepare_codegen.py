"""Regenerate CPU translations from an explicit user-owned NTSC-U ROM."""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess, toml
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
from prepare_core1_metadata import prepare_core1_metadata
from prepare_main_metadata import prepare_main_metadata
from instrument_continuous import instrument
from instrument_audio_pacing import extract_audio_pacing_fixture,observe_audio_pacing
from prepare_core2_reference import prepare_core2_reference
from prepare_overlay_validation import prepare as prepare_overlay_validation
from prepare_boot_reference import prepare as prepare_boot_reference
from prepare_native_fixture import prepare as prepare_native_fixture
from prepare_ipl3_shape import prepare as prepare_ipl3_shape
from codegen_io import capture_output_times, restore_unchanged_times
from codegen_provenance import REQUIRED_INPUTS, write_receipt

ROM_SHA256='9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583'
DECOMP_SHA256='8c9d316b2edca686ec8393ddf95d480a7dfd9879ef434dbcfc4ea3000f2ffe89'

def validate_pinned_hashes():
    for label,value in [('ROM_SHA256',ROM_SHA256),('DECOMP_SHA256',DECOMP_SHA256)]:
        if len(value)!=64 or any(character not in '0123456789abcdef' for character in value):
            raise RuntimeError(f'invalid pinned SHA-256 literal {label}: {value!r}')

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def require_hash(path, expected, label):
    actual=sha(path)
    if actual!=expected: raise RuntimeError(f'{label} identity mismatch: {path} ({actual})')
def relative(path,base): return Path(os.path.relpath(Path(path).resolve(),Path(base).resolve())).as_posix()
def run(command,cwd,log):
    log.parent.mkdir(parents=True,exist_ok=True)
    with log.open('w') as output:
        result=subprocess.run([str(x) for x in command],cwd=cwd,stdout=output,stderr=subprocess.STDOUT)
    if result.returncode: raise RuntimeError(f'command failed ({result.returncode}); see {log}')
def comparison_sha(path):
    data=Path(path).read_bytes()
    if Path(path).suffix.lower() not in {'.c','.cpp','.h','.hpp','.inl','.json','.toml','.txt'}:
        return hashlib.sha256(data).hexdigest()
    text=data.decode('utf-8')
    return hashlib.sha256(text.replace('\r\n','\n').encode()).hexdigest()
def generated_registry_count(path,expected_sections):
    text=Path(path).read_text()
    entry_pattern=(r'^    \{ \.func = [A-Za-z_]\w*, \.offset = 0x[0-9A-F]{8}, '
                   r'\.rom_size = 0x[0-9A-F]{8} \},$')
    entries=re.findall(entry_pattern,text,re.M)
    arrays=re.findall(r'^static FuncEntry .+_funcs\[\] = \{$',text,re.M)
    if len(entries)!=text.count('{ .func = '):
        raise RuntimeError(f'generated FuncEntry format drift: {path}')
    if len(arrays)!=expected_sections:
        raise RuntimeError(f'generated section registry mismatch: expected {expected_sections}, found {len(arrays)}')
    if not entries:raise RuntimeError(f'generated registry is empty: {path}')
    # register_overlays.cpp appends the one core1 osInitialize adapter.
    return len(entries)+1
def compare_reference(generated,reference):
    manifest=lambda root:{p.name:comparison_sha(p) for p in sorted(root.iterdir()) if p.is_file()}
    actual,expected=manifest(generated),manifest(reference)
    normalized_line_endings=[]
    for name in sorted(set(actual)&set(expected)):
        current,retained=generated/name,reference/name
        if actual[name]==expected[name] and sha(current)!=sha(retained):
            normalized_line_endings.append(name)
    # Fresh decomp checkouts can encode a different absolute build path in DWARF.
    # These fields document the ELF container. Generated text otherwise compares
    # exact content after CRLF-to-LF normalization; binary evidence stays byte exact.
    normalized_metadata=[]
    for name,field in [('core2-reference.json','elf_sha256'),
                       ('overlay-validation.json','source_elf_sha256')]:
        current,retained=generated/name,reference/name
        if current.is_file() and retained.is_file() and actual[name]!=expected[name]:
            current_data=json.loads(current.read_text());retained_data=json.loads(retained.read_text())
            current_value=current_data.pop(field,None);retained_value=retained_data.pop(field,None)
            if current_data==retained_data:
                actual[name]=expected[name]
                normalized_metadata.append(dict(file=name,field=field,
                    generated=current_value,reference=retained_value,
                    reason='ELF container hash may differ because debug paths are checkout-specific'))
    approved_new={'boot-reference.json','core1-expected.bin','native_fixture.h','ipl3_shape.hpp'}
    result=dict(reference=str(reference),expected_files=len(expected),generated_files=len(actual),
        missing=sorted(set(expected)-set(actual)),extra=sorted(set(actual)-set(expected)-approved_new),
        changed=sorted(n for n in set(actual)&set(expected) if actual[n]!=expected[n]),
        normalized_metadata=normalized_metadata,normalized_line_endings=normalized_line_endings)
    if result['missing'] or result['extra'] or result['changed']:
        raise RuntimeError(f'generated corpus differs from retained reference: {result}')
    return result

def main():
    validate_pinned_hashes()
    app=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rom',required=True,type=Path)
    p.add_argument('--decomp-root',required=True,type=Path)
    p.add_argument('--n64recomp',required=True,type=Path)
    p.add_argument('--evidence',required=True,type=Path)
    p.add_argument('--coverage',type=Path,default=app/'config/startup_coverage.json')
    p.add_argument('--work-dir',type=Path,default=app/'.codegen')
    p.add_argument('--reference-generated',type=Path)
    a=p.parse_args();rom=a.rom.resolve();decomp=a.decomp_root.resolve();exe=a.n64recomp.resolve()
    evidence=a.evidence.resolve();coverage_file=a.coverage.resolve();work=a.work_dir.resolve()
    generated=app/'generated';config_dir=app/'config'
    stable_inputs = [app / name for name in REQUIRED_INPUTS
                     if name.startswith('tools/') or name in {
                         'config/startup_coverage.json', 'config/boundary_adjustments.json',
                         'config/runtime_exclusions.json'}]
    stable_hashes = {path: sha(path) for path in stable_inputs}
    elf=decomp/'build/us/banjotooie_decompressed.elf';decomp_rom=decomp/'decompressed.us.z64'
    linked_rom=decomp/'build/us/banjotooie_decompressed.z64';overlays_file=decomp/'overlays.us.toml'
    require_hash(rom,ROM_SHA256,'NTSC-U ROM');require_hash(decomp_rom,DECOMP_SHA256,'decompressed ROM')
    require_hash(linked_rom,DECOMP_SHA256,'rebuilt decompressed ROM')
    if not elf.is_file():raise FileNotFoundError(elf)
    if not decomp_rom.read_bytes()==linked_rom.read_bytes():
        raise RuntimeError(f'rebuilt ROM differs from canonical decompressed ROM: {linked_rom}')
    for path in [exe,overlays_file,coverage_file,config_dir/'boundary_adjustments.json',config_dir/'runtime_exclusions.json']:
        if not path.is_file():raise FileNotFoundError(path)
    evidence.mkdir(parents=True,exist_ok=True);(evidence/'logs').mkdir(exist_ok=True);work.mkdir(parents=True,exist_ok=True);generated.mkdir(exist_ok=True)
    previous=capture_output_times(app)
    (generated/'codegen-receipt.json').unlink(missing_ok=True)

    overlays=tomllib.loads(overlays_file.read_text())['overlay']
    if len(overlays)!=884:raise RuntimeError(f'expected 884 overlay slots, found {len(overlays)}')
    relocatable=work/'relocatable-sections.txt'
    relocatable.write_text(''.join(f'.{row["name"]}\n' for row in overlays if row.get('name') and not row.get('empty')))
    direct=work/'direct-elf.toml'
    direct.write_text(toml.dumps({'input':dict(entrypoint=0x80000400,elf_path=relative(elf,work),
        output_func_path='direct-output',relocatable_sections_path=relocatable.name,bss_section_suffix='_bss')}))
    run([exe,direct,'--dump-context'],work,evidence/'logs/dump-context.log')
    dump=work/'dump.toml'
    if not dump.is_file():raise RuntimeError(f'N64Recomp did not create {dump}')
    sections=tomllib.loads(dump.read_text())['section'];coverage=json.loads(coverage_file.read_text());names=[r['name'] for r in coverage['sections']]
    if len(names)!=len(set(names)):raise RuntimeError('duplicate coverage section')
    selected=[s for s in sections if s['name'] in names]
    if len(selected)!=len(names):raise RuntimeError(f'missing coverage sections: {sorted(set(names)-{s["name"] for s in selected})}')
    for adj in json.loads((config_dir/'boundary_adjustments.json').read_text()):
        matching=[f for s in selected for f in s['functions'] if f['name']==adj['function']]
        if matching:
            if len(matching)!=1 or matching[0]['size']!=adj['original_size']:raise RuntimeError(f'boundary drift: {adj["function"]}')
            matching[0]['size']=adj['size']
        else:
            if adj['original_size']!=0:raise RuntimeError(f'missing boundary: {adj["function"]}')
            next(s for s in selected if s['name']==adj['section'])['functions'].append(dict(name=adj['function'],vram=adj['vram'],size=adj['size']))
    for section in selected:
        if section['name'] not in {'.boot','.core1','.core2'}:section.setdefault('relocs',[])
    renames=[]
    for section in selected:
        for function in section['functions']:
            if function['name']=='wait_one_frame':
                if function['vram']!=0x80015778:raise RuntimeError('wait_one_frame address drift')
                renames.append(dict(original='wait_one_frame',native_name='tooie_wait_one_frame',vram=function['vram'],reason='Guest/runtime name collision'));function['name']='tooie_wait_one_frame'
            if function['name']=='osInitialize' and function['vram']==0x8002D7B0:
                renames.append(dict(original='osInitialize',native_name='tooie_core1_osInitialize',vram=function['vram'],reason='Distinct core1 FINALROM/J instance'));function['name']='tooie_core1_osInitialize'
    if len(renames)!=2:raise RuntimeError(f'expected two renames: {renames}')
    selected_path=config_dir/'selected.syms.toml';selected_path.write_text(toml.dumps({'section':selected}))
    (config_dir/'symbol_renames.json').write_text(json.dumps(renames,indent=2)+'\n')
    stable=['*']+['.'+r['name'] if r.get('name') and not r.get('empty') and '.'+r['name'] in names else '*' for r in overlays]
    if stable[350]!='.chweldarbossfireball' or stable[679]!='.gcstatusDll':raise RuntimeError('overlay slot drift')
    overlay_ids=config_dir/'overlay-ids.txt';overlay_ids.write_text('\n'.join(stable)+'\n')
    coverage['selected_ids']={name:i for i,name in enumerate(stable) if name!='*'};(evidence/'startup-coverage.json').write_text(json.dumps(coverage,indent=2)+'\n')
    prepare_core1_metadata(app,evidence,elf_path=elf,baseline_symbols=selected_path);prepare_main_metadata(app,evidence,elf_path=elf)
    prepare_core2_reference(app,evidence,rom_path=rom,decompressed_rom_path=decomp_rom,elf_path=elf)
    prepare_overlay_validation(app.parent,generated/'overlay-validation.json',coverage=coverage_file,source=decomp,rom=rom,elf_path=elf)
    prepare_boot_reference(rom,decomp_rom,elf,generated)
    prepare_native_fixture(decomp,selected_path,generated/'native_fixture.h',elf)
    prepare_ipl3_shape(rom,generated/'ipl3_shape.hpp')

    config={'input':dict(entrypoint=0x80000400,symbols_file_path=selected_path.name,rom_file_path=relative(decomp_rom,config_dir),
        output_func_path=relative(generated,config_dir),functions_per_output_file=50,relocatable_sections_path=overlay_ids.name,
        recomp_include='#include "recomp.h"\n#include "boot_hooks.h"'),
        'patches':{'ignored':json.loads((config_dir/'runtime_exclusions.json').read_text())}}
    hooks=[(f,pc,f'tooie_boot_hook(rdram, ctx, {stage});') for f,pc,stage in [('func_80000450',0x80000450,0),('func_80000560',0x80000560,1),('func_80000560',0x800005E4,2),('func_80000450',0x80000518,3)]]
    hooks += [(f,pc,f'if (tooie_thread_hooks_active()) tooie_thread_hook(rdram, ctx, {stage});') for f,pc,stage in [('func_80012030',0x80012030,0),('func_80012030',0x80012054,1),('func_80012030',0x80012064,2),('func_8001DCB0',0x8001DCE4,3),('func_8001DCB0',0x8001DCEC,4),('func_80013620',0x80013660,5),('func_80013620',0x80013668,6)]]
    hooks += [(f,pc,f'if (tooie_main_hooks_active()) tooie_main_hook(rdram, ctx, {stage});') for f,pc,stage in [('func_80013678',0x80013678,0),('func_800131C0',0x800131E0,1),('func_800131C0',0x800131F8,2),('func_800131C0',0x8001320C,3),('func_800131C0',0x80013214,4),('func_8001DD28',0x8001DD5C,5),('func_8001DD28',0x8001DD64,6),('func_80012548',0x80012588,7),('func_80013678',0x80013698,8),('func_80013678',0x800136A0,9)]]
    hooks += [('rom_read_word',0x8001E210,'tooie_rom_read_word(rdram, ctx); return;')]
    hooks += [('func_80019EC0',pc,f'tooie_core2_hook(rdram, ctx, {stage});') for pc,stage in [(0x80019EC0,0),(0x80019F70,1),(0x80019F7C,2),(0x80019F9C,3),(0x80019FF0,4)]]
    config['patches']['hook']=[dict(func=f,before_vram=pc,text=text) for f,pc,text in hooks]
    config_path=config_dir/'tooie.us.toml';config_path.write_text(toml.dumps(config))
    for stale in generated.glob('funcs_*.c'):stale.unlink()
    for name in ['funcs.h','lookup.cpp','recomp_overlays.inl']:
        path=generated/name
        if path.exists():path.unlink()
    run([exe,config_path],config_dir,evidence/'logs/codegen.log')
    counts={'expected_registry_entries':generated_registry_count(generated/'recomp_overlays.inl',len(selected)),
            'selected_sections':len(selected)}
    (generated/'coverage_metadata.json').write_text(json.dumps(counts,indent=2)+'\n')
    (generated/'coverage_metadata.hpp').write_text('#pragma once\n#include <cstddef>\nnamespace tooie::coverage_meta {\n'+''.join(f'inline constexpr std::size_t {k}={v};\n' for k,v in counts.items())+'}\n')
    pristine_sources=sorted(generated.glob('*.c'))
    fixture_dir=generated/'test-fixtures';fixture_dir.mkdir(exist_ok=True)
    (fixture_dir/'audio_pacing_functions.inc').write_text(extract_audio_pacing_fixture(pristine_sources))
    pacing=[]
    for source in pristine_sources:
        transformed,count=observe_audio_pacing(source.read_text(),str(source))
        if count:pacing.append((source,'#include "audio_pacing_hooks.h"\n'+transformed,count))
    if sum(x[2] for x in pacing)!=4:raise RuntimeError('expected four audio pacing hooks')
    for source,text,_ in pacing:source.write_text(text)
    (evidence/'audio-pacing-instrumentation.json').write_text(json.dumps([dict(file=str(s.relative_to(app)),hooks=c) for s,_,c in pacing],indent=2)+'\n')
    instrumentation=instrument(generated,selected);(evidence/'continuous-instrumentation.json').write_text(json.dumps(instrumentation,indent=2)+'\n')
    comparison=compare_reference(generated,a.reference_generated.resolve()) if a.reference_generated else None
    inputs=[rom,decomp_rom,linked_rom,elf,overlays_file,coverage_file,config_dir/'boundary_adjustments.json',config_dir/'runtime_exclusions.json',Path(__file__).resolve(),app/'tools/prepare_core1_metadata.py',app/'tools/prepare_main_metadata.py',app/'tools/instrument_continuous.py',app/'tools/instrument_audio_pacing.py',app/'tools/prepare_core2_reference.py',app/'tools/prepare_overlay_validation.py',app/'tools/prepare_boot_reference.py',app/'tools/prepare_native_fixture.py',app/'tools/prepare_ipl3_shape.py',app/'tools/codegen_io.py']
    receipt=dict(command=[str(exe),str(config_path)],generator_sha256=sha(exe),symbol_renames=renames,coverage=counts,
        instrumentation=instrumentation,inputs={str(x):sha(x) for x in inputs},reference_comparison=comparison,
        byte_identical_timestamps_preserved=restore_unchanged_times(previous))
    (evidence/'codegen.json').write_text(json.dumps(receipt,indent=2)+'\n')
    if any(sha(path) != digest for path, digest in stable_hashes.items()):
        raise RuntimeError('Codegen inputs changed during generation; rerun after edits freeze')
    write_receipt(app, inputs + [exe] + [app / name for name in REQUIRED_INPUTS],
                  {'generation': 'full', 'command': receipt['command'],
                   'evidence_receipt_sha256': sha(evidence/'codegen.json')})
    return 0
if __name__=='__main__':raise SystemExit(main())
