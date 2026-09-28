"""Apply only the pinned cheat/cutscene hooks to existing generated sources."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from instrument_continuous import apply_game_feature_followup_hooks


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--kind', choices=('lifecycle', 'restore', 'camera', 'all'), default='lifecycle')
    args = parser.parse_args()
    generated = args.generated.resolve()
    backup = args.backup.resolve()

    outputs = []
    for path in sorted(generated.glob('*.c')):
        source = path.read_text()
        transformed, count = apply_game_feature_followup_hooks(
            source, str(path), lifecycle=args.kind in ('lifecycle', 'all'),
            restore=args.kind in ('restore', 'all'),
            camera=args.kind in ('camera', 'all'))
        if count:
            outputs.append((path, source, transformed, count))
    expected = {'lifecycle': 2, 'restore': 1, 'camera': 1, 'all': 4}[args.kind]
    if sum(row[3] for row in outputs) != expected:
        raise RuntimeError(f'expected {expected} {args.kind} hooks, found {[(p.name, c) for p,_,_,c in outputs]}')

    backup.mkdir(parents=True, exist_ok=True)
    receipt = []
    for path, source, transformed, count in outputs:
        backup_path = backup / path.name
        if backup_path.exists() and backup_path.read_text() != source:
            raise RuntimeError(f'backup already exists with different content: {backup_path}')
        if not backup_path.exists():
            backup_path.write_text(source)
        path.write_text(transformed)
        receipt.append({'file': path.name, 'hooks': count,
                        'before_sha256': sha256(backup_path), 'after_sha256': sha256(path)})
    (backup / 'game-feature-hooks.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
