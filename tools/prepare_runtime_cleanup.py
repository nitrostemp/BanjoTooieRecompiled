#!/usr/bin/env python3
"""Materialize the approved bounded cleanup repair without editing references."""
import argparse
import hashlib
import json
import pathlib
import subprocess

PIN = 'ca568b6ad79b9029d14077f0c3ffa757727c5559'
BEFORE_LF = 'e3ba32a1e06ba1643adc10ccc29ef5e3429e8809270f787a181314fc5704e653'
AFTER = '70582b56f42f0ad6935cb9821f7582a788a12da9f3dff5aa48a67109a9501f74'
PATCH = '9b4261af055d9677c061065c1d7588e79fcec74f679dac6cf12d3c069e2319fb'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def normalize_lf(data):
    return data.replace(b'\r\n', b'\n').replace(b'\r', b'\n')

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--runtime', type=pathlib.Path, required=True)
    p.add_argument('--patch', type=pathlib.Path, required=True)
    p.add_argument('--output', type=pathlib.Path, required=True)
    a = p.parse_args()
    source = a.runtime / 'ultramodern/src/threads.cpp'
    revision = subprocess.check_output(['git', '-C', str(a.runtime), 'rev-parse', 'HEAD'], text=True).strip()
    require(revision == PIN, f'Runtime revision drift: {revision}')
    original = source.read_bytes()
    normalized_original = normalize_lf(original)
    patch = a.patch.read_bytes()
    require(sha(normalized_original) == BEFORE_LF,
            f'Runtime source content drift after LF normalization: {sha(normalized_original)}')
    require(sha(patch) == PATCH, f'Approved patch drift: {sha(patch)}')
    # Exact unified hunk application: no fuzzy search, offset, or external checkout mutation.
    lines = normalized_original.decode('utf-8').splitlines(keepends=True)
    patch_lines = patch.decode('utf-8').splitlines(keepends=True)
    require(patch_lines[3] == '@@ -364,4 +364,13 @@ void ultramodern::cleanup_thread(UltraThreadContext *cur_context) {\n', 'Unexpected approved hunk')
    hunk = patch_lines[4:]
    before = [line[1:] for line in hunk if line.startswith((' ', '-'))]
    after = [line[1:] for line in hunk if line.startswith((' ', '+'))]
    require(len(before) == 4 and len(after) == 13, 'Unexpected hunk lengths')
    require(lines[363:367] == before, 'Exact patch context mismatch')
    result = ''.join(lines[:363] + after + lines[367:]).encode('utf-8')
    require(sha(result) == AFTER, f'Patched source drift: {sha(result)}')
    a.output.parent.mkdir(parents=True, exist_ok=True)
    if not a.output.exists() or a.output.read_bytes() != result:
        a.output.write_bytes(result)
    provenance = dict(runtime_revision=revision, input=str(source.resolve()), input_sha256=sha(original),
                      input_normalized_lf_sha256=sha(normalized_original),
                      patch=str(a.patch.resolve()), patch_sha256=sha(patch), output=str(a.output.resolve()),
                      output_sha256=sha(result), line_endings='input content verified after CRLF/CR to LF normalization; generated source LF; input bytes unchanged',
                      reference_unchanged=source.read_bytes() == original,
                      approval='step1/master-review/s1b2/REVIEW.md',
                      scope='Generic runtime compatibility repair; bounded producer-quiescence contract only',
                      normal_recomp_start_shutdown_certified=False)
    require(provenance['reference_unchanged'], 'Reference changed during materialization')
    a.output.with_suffix('.provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')

if __name__ == '__main__':
    main()
