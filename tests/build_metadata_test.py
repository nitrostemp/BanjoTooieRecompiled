"""Isolated safe-metadata materializer and sibling-only resolver tests."""
import argparse,json,shutil,subprocess,sys,tempfile
from pathlib import Path

SOURCE_NAMES=['config/tooie.us.toml','generated/boot-reference.json','generated/core2-reference.json','generated/overlay-validation.json']
PAYLOAD_NAMES=['config/tooie.us.toml','metadata/core2.json','metadata/core1.json','metadata/overlays.json']

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--app',type=Path,required=True)
    parser.add_argument('--compiler',required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--msvc',action='store_true');a=parser.parse_args()
    a.output.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix='attempt-',dir=a.output.resolve()));app=a.app.resolve()
    inputs=work/'live';build=work/'build';build.mkdir()
    for name in SOURCE_NAMES:
        target=inputs/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(app/name,target)
    tool=app/'tools/prepare_build_metadata.py';cmd=[sys.executable,str(tool),'--source',str(inputs),'--build',str(build)]
    records=[]
    def call(command,expected=0,label=''):
        r=subprocess.run(command,capture_output=True,text=True,timeout=90,cwd=work)
        records.append(dict(case=label,command=[str(x) for x in command],exit=r.returncode,stdout=r.stdout,stderr=r.stderr))
        assert r.returncode==expected,(label,r.returncode,r.stdout,r.stderr)
        return r
    call(cmd,label='materialize')
    selection=json.loads((build/'build-metadata/selection.json').read_text());identity=selection['identity']
    sealed=build/'runtime-data'/identity
    original={name:(sealed/name).read_bytes() for name in PAYLOAD_NAMES}
    assert not list(sealed.rglob('*.bin'))
    text=(sealed/'metadata/overlays.json').read_text()
    assert all(name not in text for name in ('prefix_hex','original_image_hex','canonical_image_hex'))
    core1=(sealed/'metadata/core1.json').read_text()
    assert all(name not in core1 for name in ('dma_padding_hex','elf_sha256','boot_symbols'))
    times={name:(sealed/name).stat().st_mtime_ns for name in PAYLOAD_NAMES}
    call(cmd,label='repeat-does-not-rewrite')
    assert times=={name:(sealed/name).stat().st_mtime_ns for name in PAYLOAD_NAMES}
    probe=build/('probe.exe' if a.msvc else 'probe')
    compile=[a.compiler,'/nologo','/std:c++20','/EHsc',f'/I{app/"src"}',f'/I{build/"build-metadata"}',
             str(app/'tests/build_metadata_probe.cpp'),str(app/'src/build_metadata.cpp'),str(app/'src/platform_support.cpp'),
             f'/Fe:{probe}','bcrypt.lib'] if a.msvc else [a.compiler,'-std=c++20','-Wall','-Wextra','-Werror',
             '-I'+str(app/'src'),'-I'+str(build/'build-metadata'),str(app/'tests/build_metadata_probe.cpp'),
             str(app/'src/build_metadata.cpp'),str(app/'src/platform_support.cpp'),'-lcrypto','-o',str(probe)]
    call(compile,label='compile-real-resolver')
    launch=work/'retained/app.exe';sibling=launch.parent/'runtime-data'/identity
    call([str(probe),str(launch)],3,label='missing-sibling-rejected')
    shutil.copytree(sealed,sibling)
    result=call([str(probe),str(launch)],label='exact-sibling');assert str(sibling) in result.stdout
    for name in PAYLOAD_NAMES:
        path=sibling/name;mutated=bytearray(original[name]);mutated[-1]^=1;path.write_bytes(mutated)
        call([str(probe),str(launch)],3,label='sibling-tampered-'+name);path.unlink()
        call([str(probe),str(launch)],3,label='sibling-missing-'+name);path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(original[name])
    call([str(probe),str(launch),'mutate-after-load'],label='verified-buffer-retained')
    (sibling/'metadata/core2.json').write_bytes(original['metadata/core2.json'])
    (inputs/'config/tooie.us.toml').write_text((inputs/'config/tooie.us.toml').read_text()+'\n# next build\n')
    call(cmd,label='new-input-new-identity-old-bundle-preserved')
    next_identity=json.loads((build/'build-metadata/selection.json').read_text())['identity']
    assert next_identity!=identity and all((sealed/name).read_bytes()==original[name] for name in PAYLOAD_NAMES)
    query=call([str(probe),'--build-metadata-info'],label='metadata-query-uses-executable-sibling')
    assert json.loads(query.stdout)['identity']==identity
    (work/'receipt.json').write_text(json.dumps(dict(cases=len(records),records=records,identity=identity,
        next_identity=next_identity),indent=2)+'\n')
    print(f'PASS sibling-only safe build metadata: {len(records)} cases; {work / "receipt.json"}')
if __name__=='__main__':main()
