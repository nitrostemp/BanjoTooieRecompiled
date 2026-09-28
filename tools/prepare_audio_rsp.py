"""Generate local Tooie audio RSP code and ROM identity data."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,toml,zlib
ROM_SHA256='9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583'
DECOMP_SHA256='8c9d316b2edca686ec8393ddf95d480a7dfd9879ef434dbcfc4ea3000f2ffe89'
PIN='ffb39cdad1da5de07eaaa48bd1db4a89a7986771'
def sha(data):return hashlib.sha256(data).hexdigest()
def validate_pinned_hashes():
    for label,value in [('ROM_SHA256',ROM_SHA256),('DECOMP_SHA256',DECOMP_SHA256)]:
        if len(value)!=64 or any(character not in '0123456789abcdef' for character in value):raise RuntimeError(f'invalid pinned SHA-256 literal {label}: {value!r}')
def checked(path,expected):
    data=path.read_bytes()
    if sha(data)!=expected:raise RuntimeError(f'identity mismatch: {path}')
    return data
def main():
    validate_pinned_hashes()
    app=Path(__file__).resolve().parents[1];p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rom',required=True,type=Path);p.add_argument('--decompressed-rom',required=True,type=Path)
    p.add_argument('--rsp-recomp',required=True,type=Path);p.add_argument('--n64recomp-source',required=True,type=Path)
    p.add_argument('--output',type=Path,default=app/'generated/audio_mission01');p.add_argument('--evidence',required=True,type=Path)
    p.add_argument('--reference-generated',type=Path);a=p.parse_args();out=a.output.resolve();evidence=a.evidence.resolve()
    source=a.n64recomp_source.resolve();pin=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
    if pin!=PIN:raise RuntimeError(f'unexpected N64Recomp pin: {pin}')
    original=checked(a.rom.resolve(),ROM_SHA256);decomp=checked(a.decompressed_rom.resolve(),DECOMP_SHA256)
    core=b''
    for start,end in [(0x1E29B60,0x1E3F718),(0x1E3F718,0x1E42550)]:
        block=zlib.decompress(original[start+2:end],-15)
        if len(block)!=int.from_bytes(original[start:start+2],'big')*16:raise RuntimeError('core1 stream mismatch')
        core+=block
    if core!=decomp[0x1E29B60:0x1E29B60+len(core)]:raise RuntimeError('original/decompressed core1 mismatch')
    out.mkdir(parents=True,exist_ok=True);evidence.mkdir(parents=True,exist_ok=True);identities={}
    header=['// Generated from the local user-owned ROM; do not commit.','#pragma once','#include <array>','#include <cstdint>','namespace tooie::audio_rsp::identity {']
    for name,offset,size,vram in [('code',0x1E4F3B0,0x17D0,0x80037880),('data',0x1E59B40,0x740,0x80042010),('boot',0x1E50B80,0xD0,0x80039050)]:
        data=core[offset-0x1E29B60:offset-0x1E29B60+size];identities[name]=dict(decompressed_rom_offset=hex(offset),size=size,guest_symbol=hex(vram),sha256=sha(data))
        header += [f'inline constexpr char {name}_sha256[] = "{sha(data)}";',f'inline constexpr std::array<uint8_t, {size}> {name} = {{']
        header += ['    '+', '.join(f'0x{x:02x}' for x in data[i:i+16])+',' for i in range(0,size,16)];header.append('};')
    header.append('}');(out/'audio_identity.hpp').write_text('\n'.join(header)+'\n')
    config={'text_offset':0x1E4F3B0,'text_size':0xF80,'text_address':0x04001080,'rom_file_path':str(a.decompressed_rom.resolve()),
        'output_file_path':str(out/'tooie_audio_rsp_generated.cpp'),'output_function_name':'tooie_n_aspMain_mission',
        'extra_indirect_branch_targets':[0x1AE8,0x143C,0x1240,0x1D84,0x126C,0x1B20,0x12A8,0x1214,0x141C,0x1310,0x13CC,0x12E4,0x1FB0,0x1358,0x16EC,0x1408],
        'overlay_slots':[{'text_address':0x04001238,'overlays':[{'offset':0x1B8,'size':0xDC8},{'offset':0xF80,'size':0x848}]}]}
    config_path=out/'audio_mission01.toml';config_path.write_text(toml.dumps(config));command=[str(a.rsp_recomp.resolve()),str(config_path)]
    result=subprocess.run(command,capture_output=True,text=True);(evidence/'generation.log').write_text(result.stdout+result.stderr)
    if result.returncode:raise RuntimeError(f'RSPRecomp failed ({result.returncode})')
    reference=None
    if a.reference_generated:
        expected=(a.reference_generated.resolve()/'tooie_audio_rsp_generated.cpp').read_bytes();actual=(out/'tooie_audio_rsp_generated.cpp').read_bytes()
        reference=dict(expected_sha256=sha(expected),actual_sha256=sha(actual),matches=expected==actual)
        if expected!=actual:raise RuntimeError(f'audio RSP output differs: {reference}')
    shutil.copyfile(source/'LICENSE',out/'N64Recomp-LICENSE.txt')
    receipt=dict(generator_source_pin=pin,generator_binary_sha256=sha(a.rsp_recomp.resolve().read_bytes()),command=command,
        original_sha256=sha(original),decompressed_sha256=sha(decomp),identities=identities,reference_comparison=reference,
        artifacts={str(x.name):sha(x.read_bytes()) for x in sorted(out.iterdir()) if x.is_file()})
    (evidence/'generation-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
if __name__=='__main__':main()
