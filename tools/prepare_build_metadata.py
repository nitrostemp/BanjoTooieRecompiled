#!/usr/bin/env python3
"""Seal distributable metadata; raw ROM-derived comparison bytes stay in source only."""
import argparse,hashlib,json
from pathlib import Path

def sha(data):return hashlib.sha256(data).hexdigest()
def encode(value):return (json.dumps(value,sort_keys=True,separators=(',',':'))+'\n').encode()
def replace_if_changed(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    if not path.exists() or path.read_bytes()!=data:path.write_bytes(data)
def prepare(source,build,verify=False):
    source=source.resolve();build=build.resolve()
    raw_core1=json.loads((source/'generated/boot-reference.json').read_text())
    core1={key:raw_core1[key] for key in (
        'symbols','streams','core1','source_sha256','scratch_base',
        'consumed_rom_end','dma_padding_bytes','core2_range','checksum_words')}
    core1['schema']=1
    core2=json.loads((source/'generated/core2-reference.json').read_text())
    core2.pop('dma_padding_hex',None)
    raw_overlays=json.loads((source/'generated/overlay-validation.json').read_text())
    overlays=[]
    forbidden={'prefix_hex','original_image_hex','canonical_image_hex'}
    for original in raw_overlays['overlays']:
        row={k:v for k,v in original.items() if k not in forbidden}
        for name in forbidden:
            row[name.removesuffix('_hex')+'_sha256']=sha(bytes.fromhex(original[name]))
        row['initialized_bytes']=len(bytes.fromhex(original['canonical_image_hex']))
        overlays.append(row)
    overlay_metadata={
        'schema':2,
        'source_rom_sha256':raw_overlays['source_rom_sha256'],
        'source_elf_sha256':raw_overlays['source_elf_sha256'],
        'derivation':'inflate selected spans from the validated user ROM at runtime',
        'overlays':overlays,
    }
    data={
        'config/tooie.us.toml':(source/'config/tooie.us.toml').read_bytes(),
        'metadata/core2.json':encode(core2),
        'metadata/core1.json':encode(core1),
        'metadata/overlays.json':encode(overlay_metadata),
    }
    rows=[dict(path=name,bytes=len(content),sha256=sha(content)) for name,content in data.items()]
    manifest=encode(dict(schema=1,files=rows));identity=sha(manifest)
    bundle=build/'runtime-data'/identity
    payload={**data,'manifest.json':manifest}
    if not bundle.exists():
        if verify:raise RuntimeError('Configured build metadata bundle is missing; reconfigure the build')
        bundle.mkdir(parents=True,exist_ok=False)
        for name,content in payload.items():
            path=bundle/name;path.parent.mkdir(parents=True,exist_ok=True)
            with path.open('xb') as output:output.write(content)
    for name,content in payload.items():
        path=bundle/name
        if not path.is_file() or path.read_bytes()!=content:
            raise RuntimeError(f'Immutable build metadata is missing or changed; refusing repair: {path}')
    header='#pragma once\n#include <array>\n#include <cstddef>\nnamespace tooie::build_metadata::detail {\n'
    header+='struct ExpectedFile {const char* path;std::size_t bytes;const char* sha256;};\n'
    header+=f'inline constexpr char identity[] = "{identity}";\n'
    header+=f'inline constexpr std::size_t manifest_bytes = {len(manifest)};\n'
    header+=f'inline constexpr std::array<ExpectedFile,{len(rows)}> files{{{{\n'
    header+=''.join('{'+json.dumps(r['path'])+','+str(r['bytes'])+','+json.dumps(r['sha256'])+'},\n' for r in rows)
    header+='}};\n}\n'
    generated=build/'build-metadata';header_path=generated/'build_metadata_manifest.hpp'
    selection=encode(dict(schema=1,identity=identity,bundle=str(bundle),files=rows))
    if verify:
        if header_path.read_bytes()!=header.encode() or (generated/'selection.json').read_bytes()!=selection:
            raise RuntimeError('Configured metadata identity differs from current inputs; reconfigure the build')
    else:
        replace_if_changed(header_path,header.encode());replace_if_changed(generated/'selection.json',selection)
    return dict(identity=identity,bundle=str(bundle),files=rows)
def main():
    p=argparse.ArgumentParser();p.add_argument('--source',required=True,type=Path);p.add_argument('--build',required=True,type=Path)
    p.add_argument('--verify',action='store_true');a=p.parse_args()
    print(json.dumps(prepare(a.source,a.build,a.verify)))
if __name__=='__main__':main()
