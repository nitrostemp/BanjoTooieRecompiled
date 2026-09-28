"""Black-box contract for the bounded S1-A executable, written before the host."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--exe', type=Path, required=True)
p.add_argument('--rom', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
a = p.parse_args()
assert a.exe.is_file(), 'S1-A executable has not been implemented/built'
a.evidence.mkdir(parents=True, exist_ok=True)
def run(rom, name, expected):
    trace = a.evidence / name
    cmd = [str(a.exe.resolve()), '--rom', str(rom.resolve()), '--trace-dir', str(trace.resolve()), '--stop-at', 'boot-entry']
    try:
        result = subprocess.run(cmd, text=True, capture_output=True, timeout=40)
    except subprocess.TimeoutExpired as error:
        (a.evidence / (name + '.timeout.txt')).write_text(str(error))
        raise AssertionError('Process exceeded its internal watchdog plus harness grace') from error
    (a.evidence / (name + '.log')).write_text(result.stdout + result.stderr)
    assert result.returncode == expected, (name, result.returncode, result.stderr)
    events = [json.loads(line) for line in (trace / 'events.jsonl').read_text().splitlines()]
    return events
events = run(a.rom, 'valid-rom', 0)
assert [e['checkpoint'] for e in events if e['event'] == 'checkpoint'] == ['A','B','C','D']
boundary = next(e for e in events if e['event'] == 'boot_boundary')
header = next(e for e in events if e['event'] == 'run_header')
assert all(len(header[k]) == 64 for k in ['executable_sha256','codegen_config_sha256','rom_sha256'])
assert boundary['sp'] == '0x800064E0' and boundary['bss_zero'] is True
assert boundary['bss_bytes'] == 0x3F90 and boundary['bss_guards_preserved'] is True
assert boundary['outcome'] == 'diagnostic_stop'
with tempfile.TemporaryDirectory(dir=a.evidence) as d:
    d = Path(d)
    short = d / 'short.z64'
    short.write_bytes(a.rom.read_bytes()[:4096])
    wrong = d / 'wrong.z64'
    image = bytearray(a.rom.read_bytes())
    image[0x100100] ^= 1
    wrong.write_bytes(image)
    for rom,name in [(short,'short-rom'),(wrong,'wrong-rom')]:
        rejected = run(rom,name,2)
        assert any(e['event']=='run_header' and len(e['executable_sha256'])==64 for e in rejected)
        assert not any(e['checkpoint'] in ['C','D'] for e in rejected)
        assert any(e['event'] == 'rom_rejected' for e in rejected)
print('PASS valid ROM executes generated entry; short/wrong ROMs rejected before C/D')
