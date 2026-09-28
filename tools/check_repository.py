#!/usr/bin/env python3
"""ROM-free tracked-source/configuration checks; not game-build acceptance."""
import ast
import json
import re
import subprocess
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_ROOTS = {'inputs', 'deps', 'downloads', 'generated', 'build', 'dist', 'local',
                   'evidence', 'artifacts', 'logs', 'captures', 'dumps', 'profiles',
                   'personal-play', 'saves', 'secrets', '.codegen', '.local-evidence', '.claude'}
FORBIDDEN_EXTENSIONS = {'.z64', '.n64', '.v64', '.elf', '.eep', '.sra', '.fla',
                        '.sav', '.bin', '.exe', '.dll', '.obj', '.pdb', '.lib', '.map',
                        '.ilk', '.dmp', '.jsonl', '.log'}
LOCAL_ONLY = {'src/frontend_graphics_tab.cpp', 'src/frontend_file.cpp',
              'src/frontend_config_page_options_menu.cpp', 'src/frontend_input_events.cpp',
              'src/recompinput/input_types.h', 'tests/fixtures/audio_pacing_functions.inc',
              'config/selected.syms.toml', 'config/tooie.us.toml',
              'config/overlay-ids.txt', 'config/symbol_renames.json'}
# Public documentation boundary: player, contributor, build and licensing
# material only. Plans, audits, handoffs, investigations and status logs stay
# in ignored local/ storage.
PUBLIC_DOCS = {'docs/ARCHITECTURE.md', 'docs/CHECKSUM_PROVENANCE.md', 'docs/CODEGEN.md',
               'docs/PLAYER_GUIDE.md', 'docs/RELEASE_NOTES.md', 'docs/WINDOWS.md'}
LOCAL_INSTRUCTIONS = {'AGENTS.md', 'CLAUDE.md'}

def main():
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    names = [n for n in names if n]
    if not names:
        raise RuntimeError('No tracked/staged source files to review')
    for name in names:
        path = Path(name)
        if (name in LOCAL_ONLY or path.parts[0] in FORBIDDEN_ROOTS or path.parts[0].startswith(('package', 'build-'))
                or path.suffix.lower() in FORBIDDEN_EXTENSIONS
                or path.name == '.env' or path.name.startswith('.env.') and path.name != '.env.example'):
            raise RuntimeError(f'Excluded payload is tracked: {name}')
        if (path.parts[0] == 'docs' and name not in PUBLIC_DOCS
                or name in LOCAL_INSTRUCTIONS):
            raise RuntimeError(f'Internal or unlisted documentation is tracked: {name} '
                               '(keep working notes under local/, or add public docs to PUBLIC_DOCS)')
        content = (ROOT / path).read_bytes()
        if path.suffix == '.py':
            ast.parse(content.decode('utf-8-sig'), filename=name)
        elif path.suffix == '.json':
            json.loads(content)
        elif path.suffix == '.svg':
            ET.fromstring(content)
        if path.suffix not in {'.ttf', '.png'}:
            if re.search(rb'(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----)', content):
                raise RuntimeError(f'Possible credential material: {name}')
            if re.search(rb'(?:[A-Z]:[/\\]Users[/\\][a-z0-9_-]+[/\\]|/mnt/[a-z]/Users/[a-z0-9_-]+/|/home/[a-z0-9_-]+/)', content, re.I):
                raise RuntimeError(f'Former private workspace path is tracked: {name}')
    lock = json.loads((ROOT / 'dependencies.lock.json').read_text(encoding='utf-8'))
    for name, spec in lock['dependencies'].items():
        if not spec['url'].startswith('https://'):
            raise RuntimeError(f'Unexpected dependency transport: {name}')
        directory = Path(spec['directory'])
        if directory.is_absolute() or '..' in directory.parts:
            raise RuntimeError(f'Unsafe dependency directory: {name}')
        if spec['kind'] == 'git':
            if not re.fullmatch('[0-9a-f]{40}', spec['commit']):
                raise RuntimeError(f'Dependency is not commit pinned: {name}')
            if any(not re.fullmatch('[0-9a-f]{40}', pin) for pin in spec.get('submodules', {}).values()):
                raise RuntimeError(f'Submodule is not commit pinned: {name}')
        elif spec['kind'] == 'archive':
            if not re.fullmatch('[0-9a-f]{64}', spec['sha256']):
                raise RuntimeError(f'Archive lacks SHA-256: {name}')
            if any(not re.fullmatch('[0-9a-f]{64}', value)
                   for value in spec.get('files', {}).values()):
                raise RuntimeError(f'Installed-file hash is malformed: {name}')
            if 'source_commit' in spec and not re.fullmatch('[0-9a-f]{40}', spec['source_commit']):
                raise RuntimeError(f'Archive source reference is malformed: {name}')
        else:
            raise RuntimeError(f'Unknown dependency type: {name}')
    coverage = json.loads((ROOT / 'config/startup_coverage.json').read_text(encoding='utf-8'))
    sections = [r['name'] for r in coverage['sections']]
    if len(sections) != len(set(sections)):
        raise RuntimeError('Duplicate generation coverage sections')
    print(f'ROM-free checks passed for {len(names)} tracked files; no game build or gameplay claim.')

if __name__ == '__main__':
    main()
