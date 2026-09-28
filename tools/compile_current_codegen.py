"""Replay prepared codegen after hook-only edits, preserving verified ancestry."""
from pathlib import Path
import argparse
import json
import subprocess
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
from instrument_continuous import instrument
from instrument_audio_pacing import extract_audio_pacing_fixture, observe_audio_pacing
from codegen_io import capture_output_times, restore_unchanged_times
from codegen_provenance import validate, write_receipt, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    app = Path(__file__).resolve().parents[1]
    # No stamp-only migration: missing preparation ancestry requires full codegen.
    parent = validate(app, replay=True)
    input_hashes = {app / name: sha(app / name) for name in parent['inputs']}
    command = parent['evidence']['command']
    if len(command) != 2:
        raise ValueError('Invalid ancestor generation command')
    previous = capture_output_times(app)
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    generated = app / 'generated'
    sections = tomllib.loads((app / 'config/selected.syms.toml').read_text())['section']
    ancestor_hash = sha(generated / 'codegen-receipt.json')
    (generated / 'codegen-receipt.json').unlink()
    for source in generated.glob('funcs_*.c'):
        source.unlink()
    with (evidence / 'codegen.log').open('w') as log:
        subprocess.run(command, cwd=app / 'config', stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    sources = sorted(generated.glob('*.c'))
    (generated / 'test-fixtures/audio_pacing_functions.inc').write_text(extract_audio_pacing_fixture(sources))
    hooks = 0
    for source in sources:
        text, count = observe_audio_pacing(source.read_text(), str(source))
        if count:
            source.write_text('#include "audio_pacing_hooks.h"\n' + text)
            hooks += count
    if hooks != 4:
        raise ValueError(f'Expected four audio pacing hooks, found {hooks}')
    instrumentation = instrument(generated, sections)
    receipt = {'generation': 'replay', 'command': command,
               'ancestor_receipt_sha256': ancestor_hash,
               'instrumentation': instrumentation,
               'byte_identical_timestamps_preserved': restore_unchanged_times(previous)}
    if any(sha(path) != digest for path, digest in input_hashes.items()):
        raise ValueError('Codegen inputs changed during replay; rerun after edits freeze')
    write_receipt(app, [app / name for name in parent['inputs']], receipt)
    (evidence / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
