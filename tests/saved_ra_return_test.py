"""Compile actual generated wrapper/helper snippets without regenerating anything.

Use --mode current to exercise the application's current generated wrappers
directly, stripping observational hooks only; no return correction is reapplied.
Use an explicit retained --source for original/corrected red/green replays.
--evidence requires a fresh directory. --evidence-root creates a fresh child on
each run for repeatable CTest use. Original ELF supplies the math tables and
instruction assertions. No application or reference input is written.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

APP = Path(__file__).resolve().parents[1]
PROJ = APP.parent
NAMES = ['func_800137D4','func_800137F4','func_80013818',
         'func_80013A5C','func_80013A7C','func_80013ABC']
WORDS = {
    0x800137E0:0x03E02825,0x800137E4:0x0C004E06,0x800137E8:0xC43016D0,
    0x800137EC:0x00A00008,0x800137F0:0x46001006,
    0x80013804:0x03E02825,0x80013808:0x0C004E06,0x8001380C:0,
    0x80013810:0x00A00008,0x80013814:0x46001006,
    0x80013A60:0x03E03825,0x80013A64:0x0C004EAF,0x80013A68:0xC42E1700,
    0x80013A74:0x00E00008,0x80013A78:0x46001001,
    0x80013A84:0x03E03825,0x80013A88:0x0C004EAF,0x80013A8C:0,
    0x80013A98:0x00E00008,0x80013A9C:0x46001001,
}

def sha(data):
    return hashlib.sha256(data).hexdigest()

def extract(source):
    snippets=[]
    for name in NAMES:
        match=re.search(r'RECOMP_FUNC void '+name+r'\([^\n]*\n.*?^;}\n',source,re.S|re.M)
        if not match:
            raise ValueError('missing generated function '+name)
        # Existing observational hooks are independent of the faulty control
        # transfer. Strip only those exact hook lines to recover generator text.
        snippets.append(re.sub(r'^    tooie_(?:continuous_poll|overlay_callsite_push|overlay_callsite_pop)\([^\n]*\n','',match[0],flags=re.M))
    return ''.join(snippets)

def elf_core1(path):
    data=path.read_bytes()
    assert data[:6]==b'\x7fELF\x01\x02', 'expected original big-endian ELF32'
    shoff=struct.unpack_from('>I',data,32)[0]
    size,count,names=struct.unpack_from('>HHH',data,46)
    rows=[struct.unpack_from('>10I',data,shoff+i*size) for i in range(count)]
    namesrow=rows[names]; strings=data[namesrow[4]:namesrow[4]+namesrow[5]]
    row=next(row for row in rows if strings[row[0]:].split(b'\0',1)[0]==b'.core1')
    return row[3],data[row[4]:row[4]+row[5]],sha(data)

def validate_transform_contract(module, original, corrected):
    checks=[]
    def rejected(label, changed):
        try:
            module.correct_saved_ra_returns(changed,'negative fixture: '+label)
        except ValueError:
            checks.append(label)
        else:
            raise AssertionError('changed source accepted: '+label)
    rejected('changed saved register',original.replace('ctx->r5 = ctx->r31 | 0;','ctx->r5 = ctx->r30 | 0;',1))
    rejected('changed JR operand',original.replace('jr          $a1','jr          $a2',1))
    rejected('changed return delay math',original.replace('ctx->f0.fl = ctx->f2.fl;','ctx->f0.fl = -ctx->f2.fl;',1))
    rejected('changed original JR PC',original.replace('0x80013810: jr','0x80013818: jr',1))
    rejected('changed helper saved-register use',original.replace('ctx->r9 = S32(0X8004 << 16);','ctx->r5 = S32(0X8004 << 16);',1))
    rejected('missing helper',original.replace('void func_80013818(','void changed_helper(',1))
    first=re.search(r'RECOMP_FUNC void func_800137D4\([^\n]*\n.*?^;}\n',original,re.S|re.M)[0]
    rejected('duplicate wrapper',original+first)
    rejected('already corrected input',corrected)
    outsider='''RECOMP_FUNC void unrelated(uint8_t* rdram, recomp_context* ctx) {
    // 0x80900000: jr          $a1
    // 0x80900004: nop
    LOOKUP_FUNC(ctx->r5)(rdram, ctx);
    return;
;}
'''
    assert module.correct_saved_ra_returns(outsider)==(outsider,[])
    transformed,names=module.correct_saved_ra_returns(original+outsider)
    assert transformed==corrected+outsider
    checks.append('unlisted JR unchanged')
    # Exactly four lookup lines become comments: not a single arithmetic,
    # register, memory or delay-slot statement is added, removed or reordered.
    executable=lambda text: [line for line in text.splitlines()
                             if not line.lstrip().startswith('//') and 'LOOKUP_FUNC(' not in line]
    assert executable(original)==executable(corrected)
    assert original.count('LOOKUP_FUNC(')==4 and corrected.count('LOOKUP_FUNC(')==0
    checks.append('only four lookup statements changed')
    return checks

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    destination=parser.add_mutually_exclusive_group(required=True)
    destination.add_argument('--evidence',type=Path)
    destination.add_argument('--evidence-root',type=Path)
    parser.add_argument('--mode',choices=['original','corrected','current'],required=True)
    parser.add_argument('--source',type=Path,default=APP/'generated/funcs_1.c')
    args=parser.parse_args()
    if args.evidence_root:
        root=args.evidence_root.resolve();root.mkdir(parents=True,exist_ok=True)
        out=Path(tempfile.mkdtemp(prefix=args.mode+'-',dir=root))
    else:
        out=args.evidence.resolve();out.mkdir(parents=True,exist_ok=False)
    input_source=args.source.read_bytes()
    source=extract(input_source.decode().replace('\r\n','\n'))
    (out/('extracted_fixture.inc' if args.mode=='current' else 'original_fixture.inc')).write_text(source)
    original_sha=sha(source.encode())
    transform_checks=[]
    if args.mode=='corrected':
        spec=importlib.util.spec_from_file_location('instrument',APP/'tools/instrument_continuous.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        original=source
        source, corrected=module.correct_saved_ra_returns(source,str(args.source))
        assert set(corrected)==set(NAMES)-{'func_80013818','func_80013ABC'},corrected
        transform_checks=validate_transform_contract(module,original,source)
    (out/'generated_fixture.inc').write_text(source)
    base,core1,elf_sha=elf_core1(PROJ/'banjo-tooie/build/us/banjotooie_decompressed.elf')
    for pc,word in WORDS.items():
        assert struct.unpack_from('>I',core1,pc-base)[0]==word,hex(pc)
    memory=bytearray(8*1024*1024)
    for i,byte in enumerate(core1): memory[((base&0x7FFFFFFF)+i)^3]=byte
    memory_path=out/'original-rdram.bin';memory_path.write_bytes(memory)
    compiler=shutil.which('c++')
    if not compiler: raise RuntimeError('c++ required; run this focused fixture in WSL')
    command=[compiler,'-std=c++20','-O2','-ffp-contract=off','-fno-fast-math',
             '-I'+str(PROJ/'N64Recomp/include'),'-I'+str(out),str(APP/'tests/saved_ra_return_test.cpp'),'-o',str(out/'test')]
    compiled=subprocess.run(command,capture_output=True,text=True,timeout=60)
    (out/'build.log').write_text(compiled.stdout+compiled.stderr)
    compiled.check_returncode()
    result=subprocess.run([str(out/'test'),str(memory_path)],capture_output=True,text=True,timeout=30)
    (out/'run.log').write_text(result.stdout+result.stderr)
    receipt={'mode':args.mode,'command':command,'exit_code':result.returncode,
             'evidence_directory':str(out),
             'source_path':str(args.source.resolve()),'source_sha256':sha(input_source),
             'extracted_fixture_sha256':original_sha,
             'original_fixture_sha256':original_sha if args.mode!='current' else None,
             'tested_fixture_sha256':sha(source.encode()),
             'return_correction_applied':args.mode=='corrected',
             'transform_contract_checks':transform_checks,
             'tool_sha256':sha((APP/'tools/instrument_continuous.py').read_bytes()),
             'native_test_sha256':sha((APP/'tests/saved_ra_return_test.cpp').read_bytes()),
             'runner_sha256':sha(Path(__file__).read_bytes()),
             'original_elf_sha256':elf_sha,'original_instruction_words_verified':{f'{pc:08X}':f'{word:08X}' for pc,word in WORDS.items()},
             'output':result.stdout+result.stderr}
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))
    return result.returncode

if __name__=='__main__':
    raise SystemExit(main())
