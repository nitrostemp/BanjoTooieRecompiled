#!/usr/bin/env python3
"""Materialize the mission-01 queue repair without editing the pinned runtime."""
import argparse
import hashlib
import json
import pathlib
import subprocess

PIN = 'ca568b6ad79b9029d14077f0c3ffa757727c5559'
BEFORE_LF = 'b9bb55edeb9cb99a153d00d80a13c5a09140113403499af7656fd9489d38f305'
AFTER = '67f48b19bc92f2cd8abba882b53209d5bbba1d297c0ad0c159108eb4faa8e621'
PATCH = '713659775b22eeac5c1e6f23322af35f68d66eec580819e6c0e41ee5a67b56da'

def sha(data):
    return hashlib.sha256(data).hexdigest()

def normalize_lf(data):
    return data.replace(b'\r\n', b'\n').replace(b'\r', b'\n')

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=pathlib.Path, required=True)
    parser.add_argument('--patch', type=pathlib.Path, required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    output = args.output.resolve()
    require(output != runtime and runtime not in output.parents, 'Output must be outside the reference runtime')
    source = runtime / 'ultramodern/src/threadqueue.cpp'
    revision = subprocess.check_output(['git', '-C', str(runtime), 'rev-parse', 'HEAD'], text=True).strip()
    require(revision == PIN, f'Runtime revision drift: {revision}')
    original = source.read_bytes()
    normalized_original = normalize_lf(original)
    patch = args.patch.read_bytes()
    require(sha(normalized_original) == BEFORE_LF,
            f'Runtime source content drift after LF normalization: {sha(normalized_original)}')
    require(sha(patch) == PATCH, f'Mission threadqueue patch drift: {sha(patch)}')
    lines = normalized_original.decode('utf-8').splitlines(keepends=True)
    patch_lines = patch.decode('utf-8').splitlines(keepends=True)
    require(patch_lines[:2] == ['--- a/ultramodern/src/threadqueue.cpp\n',
                              '+++ b/ultramodern/src/threadqueue.cpp\n'], 'Unexpected patch file headers')
    hunk_starts = [i for i, line in enumerate(patch_lines) if line.startswith('@@')]
    require(len(hunk_starts) == 2, 'Expected exactly two pinned queue hunks')
    # Fixed contexts against the original pin, not a fuzzy patch application.
    # Neither hunk changes line count, so both use original line coordinates.
    for number, (start, count) in enumerate(((14, 7), (42, 14))):
        at = hunk_starts[number]
        require(patch_lines[at] == f'@@ -{start+1},{count} +{start+1},{count} @@\n',
                'Unexpected exact queue hunk')
        end = hunk_starts[number+1] if number+1 < len(hunk_starts) else len(patch_lines)
        hunk = patch_lines[at+1:end]
        require(all(line.startswith((' ', '-', '+')) for line in hunk), 'Unexpected hunk line')
        before = [line[1:] for line in hunk if line.startswith((' ', '-'))]
        after = [line[1:] for line in hunk if line.startswith((' ', '+'))]
        require(len(before) == count and len(after) == count, 'Unexpected hunk lengths')
        require(lines[start:start+count] == before, 'Exact patch context mismatch')
        lines[start:start+count] = after
    result = ''.join(lines).encode('utf-8')
    require(sha(result) == AFTER, f'Patched source drift: {sha(result)}')
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != result:
        output.write_bytes(result)
    provenance = dict(runtime_revision=revision, input=str(source), input_sha256=sha(original),
                      input_normalized_lf_sha256=sha(normalized_original),
                      patch=str(args.patch.resolve()), patch_sha256=sha(patch), output=str(output),
                      output_sha256=sha(result), line_endings='input content verified after CRLF/CR to LF normalization; generated source LF; input bytes unchanged',
                      reference_unchanged=source.read_bytes() == original,
                      authority='step1/TRACK_A_MISSION_01.md operating rule 5 and G1',
                      scope='Queue traversal, empty termination, null queue preservation, and original libultra FIFO among equal priorities; preserve removal metadata and descending priority order',
                      equal_priority_order='FIFO in continuous and diagnostic modes; original __osEnqueueThread at 0x80032770',
                      scheduler_or_shutdown_certified=False)
    require(provenance['reference_unchanged'], 'Reference changed during materialization')
    output.with_suffix('.provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')

if __name__ == '__main__':
    main()
