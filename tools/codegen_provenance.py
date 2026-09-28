"""Fail-closed local codegen provenance. Receipts are emitted only by generators."""
from pathlib import Path
import argparse
import hashlib
import json
import re

RECEIPT = 'codegen-receipt.json'
REQUIRED_INPUTS = tuple('tools/' + name + '.py' for name in (
    'prepare_codegen', 'compile_current_codegen', 'codegen_provenance', 'codegen_io',
    'instrument_continuous', 'instrument_audio_pacing', 'prepare_core1_metadata',
    'prepare_main_metadata', 'prepare_core2_reference', 'prepare_overlay_validation',
    'prepare_boot_reference', 'prepare_native_fixture', 'prepare_ipl3_shape')) + (
    'config/startup_coverage.json', 'config/boundary_adjustments.json',
    'config/runtime_exclusions.json', 'config/tooie.us.toml',
    'config/selected.syms.toml', 'config/overlay-ids.txt', 'config/symbol_renames.json')


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def outputs(root):
    directory = root / 'generated'
    return {p.relative_to(directory).as_posix(): sha(p)
            for p in sorted(directory.rglob('*'))
            if p.is_file() and p.name != RECEIPT
            # RSP and dependency overlays have separate generators/receipts;
            # CPU regeneration must not bless outputs it did not reproduce.
            and (p.parent == directory or p.relative_to(directory).parts[0] == 'test-fixtures')}


def write_receipt(root, inputs, evidence):
    root = Path(root).resolve()
    manifest = {}
    for path in inputs:
        path = Path(path).resolve()
        name = path.relative_to(root).as_posix() if path.is_relative_to(root) else str(path)
        manifest[name] = sha(path)
    receipt = dict(schema=1, inputs=manifest, outputs=outputs(root), evidence=evidence)
    destination = root / 'generated' / RECEIPT
    temporary = destination.with_suffix('.tmp')
    temporary.write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
    temporary.replace(destination)
    return receipt


def validate(root, required_inputs=REQUIRED_INPUTS, replay=False):
    root = Path(root).resolve()
    path = root / 'generated' / RECEIPT
    try:
        receipt = json.loads(path.read_text())
    except (OSError, ValueError) as error:
        raise ValueError('Missing/invalid codegen receipt; run full prepare_codegen.py') from error
    if receipt.get('schema') != 1 or not isinstance(receipt.get('inputs'), dict):
        raise ValueError('Unsupported codegen receipt')
    missing = set(required_inputs) - receipt['inputs'].keys()
    if missing:
        raise ValueError(f'Codegen receipt missing required input: {sorted(missing)}')
    # Replay regenerates these scripts' outputs, but cannot re-attest changed
    # preparation tools, prepared config, generator binary or metadata outputs.
    replay_inputs = {'tools/instrument_continuous.py', 'tools/instrument_audio_pacing.py',
                     'tools/compile_current_codegen.py'} if replay else set()
    for name, expected in receipt['inputs'].items():
        if name in replay_inputs:
            continue
        source = root / name
        if not source.is_file() or sha(source) != expected:
            raise ValueError(f'Codegen input changed: {name}; regenerate before building')
    actual = outputs(root)
    expected = receipt.get('outputs', {})
    if replay:
        def inherited(name):
            return not (name.endswith('.c') or name in {
                'funcs.h', 'lookup.cpp', 'recomp_overlays.inl',
                'test-fixtures/audio_pacing_functions.inc'})
        actual = {k: v for k, v in actual.items() if inherited(k)}
        expected = {k: v for k, v in expected.items() if inherited(k)}
    if not expected or actual != expected:
        raise ValueError('Codegen output set/content differs from receipt; regenerate before building')
    return receipt


def validate_sdl(path, pinned, observer=''):
    if observer and not re.fullmatch('[0-9a-fA-F]{64}', observer):
        raise ValueError('SDL observer override must be exactly 64 hexadecimal characters')
    actual = sha(path)
    expected = observer.lower() if observer else pinned.lower()
    if actual != expected:
        raise ValueError(f'SDL DLL hash mismatch: expected {expected}, found {actual}')
    return actual


def validate_build(root, executable):
    validate(root)
    executable = Path(executable)
    try:
        receipt = json.loads(executable.with_name('codegen-build-receipt.json').read_text())
    except (OSError, ValueError) as error:
        raise ValueError('Missing codegen build receipt; rebuild executable') from error
    expected = {'schema': 1, 'executable_sha256': sha(executable),
                'codegen_receipt_sha256': sha(Path(root) / 'generated' / RECEIPT)}
    if receipt != expected:
        raise ValueError('Executable/codegen build receipt mismatch; rebuild executable')
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--attest-build', type=Path)
    args = parser.parse_args()
    validate(args.root)
    if args.attest_build:
        receipt = {'schema': 1, 'executable_sha256': sha(args.attest_build),
                   'codegen_receipt_sha256': sha(args.root / 'generated' / RECEIPT)}
        destination = args.attest_build.with_name('codegen-build-receipt.json')
        destination.write_text(json.dumps(receipt, indent=2) + '\n')
    print('Codegen provenance verified')


if __name__ == '__main__':
    main()
