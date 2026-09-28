"""Apply only the source-pinned Tooie music-volume hooks to generated code."""
from pathlib import Path
import argparse
import hashlib
import json

from instrument_continuous import apply_music_volume_hooks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--backup', type=Path, required=True)
    args = parser.parse_args()
    generated = args.generated.resolve()
    backup = args.backup.resolve()

    # Complete every transform and exact-count check before mutating generated
    # output. An existing hook or changed source anchor therefore leaves the
    # entire directory untouched.
    outputs = []
    for path in sorted(generated.glob('*.c')):
        source = path.read_text()
        transformed, count = apply_music_volume_hooks(source, str(path))
        if count:
            outputs.append((path, source, transformed, count))
    if sum(row[3] for row in outputs) != 2:
        raise RuntimeError(f'expected two music-volume hooks, found '
                           f'{[(path.name, count) for path, _, _, count in outputs]}')

    backup.mkdir(parents=True, exist_ok=True)
    receipt = []
    for path, source, transformed, count in outputs:
        backup_path = backup / path.name
        if backup_path.exists() and backup_path.read_text() != source:
            raise RuntimeError(f'backup already exists with different content: {backup_path}')
        if not backup_path.exists():
            backup_path.write_text(source)
        path.write_text(transformed)
        receipt.append({
            'file': path.name,
            'hooks': count,
            'before_sha256': hashlib.sha256(backup_path.read_bytes()).hexdigest(),
            'after_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        })
    (backup / 'music-volume-hooks.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
