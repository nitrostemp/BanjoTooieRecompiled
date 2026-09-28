"""Four const-context observations; no writes or pacing adaptations.
Call before the general continuous instrumenter adds entry/label polls.
Bodies are the retained115 raw generated core1 functions (observation/poll
lines removed); their original instructions, delays and C operations are pinned.
The caller supplies the C-safe audio_pacing_hooks.h include for generated files.
"""
import hashlib
import re
TARGETS={
 'func_800128C0':('568516b89c471deb6de5bc32b6457e05456652077e301f0c0968031091025be1',(0x80012934,0x80012998)),
 'func_800129FC':('bd969df88a0deb42afb5b487d66c0938d243b63e869d69b72308363a31b66abd',(0x80012a2c,0x80012af0)),
}
FUNCTION_PATTERN=r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n'

def extract_audio_pacing_fixture(paths):
    """Extract the pristine target bodies for local transform regression tests."""
    found={}
    for path in paths:
        text=path.read_text()
        for match in re.finditer(FUNCTION_PATTERN,text,re.S|re.M):
            name=match[1]
            if name not in TARGETS:continue
            if name in found:raise ValueError(f'duplicate audio pacing function {name}: {path}')
            found[name]=match[0]
    if set(found)!=set(TARGETS):raise ValueError(f'missing audio pacing fixture functions: {sorted(set(TARGETS)-set(found))}')
    for name,(pin,_) in TARGETS.items():
        if hashlib.sha256(found[name].encode()).hexdigest()!=pin:
            raise ValueError(f'changed audio pacing fixture body {name}')
    return ''.join(found[name] for name in TARGETS)

def observe_audio_pacing(text, source='<generated>'):
    found={};count=0
    for match in re.finditer(FUNCTION_PATTERN,text,re.S|re.M):
        if match[1] in TARGETS:
            if match[1] in found:raise ValueError(f'{source}: duplicate audio pacing function {match[1]}')
            found[match[1]]=match[0]
    for name,body in found.items():
        pin,sites=TARGETS[name]
        if hashlib.sha256(body.encode()).hexdigest()!=pin:raise ValueError(f'{source}: changed/already instrumented audio pacing body {name}')
        updated=body
        for pc in sites:
            anchor=f'    // 0x{pc:08X}:'
            if body.count(anchor)!=1:raise ValueError(f'{source}: audio pacing anchor count {pc:08X}')
            updated=updated.replace(anchor,f'    tooie_observe_audio_pacing(rdram, ctx, 0x{pc:08X}u);\n'+anchor,1)
            count+=1
        text=text.replace(body,updated,1)
    return text,count
