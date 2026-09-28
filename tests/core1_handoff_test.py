"""Black-box acceptance: original boot/decompressor and actual registry handoff."""
import json
from pathlib import Path
import subprocess
import sys

exe, rom, evidence = map(Path, sys.argv[1:])
evidence.mkdir(parents=True, exist_ok=True)
for run in range(1, 4):
    dest = evidence / f'core1-run-{run}'
    result = subprocess.run([str(exe), '--rom', str(rom), '--trace-dir', str(dest),
                             '--stop-at', 'core1-entry'], capture_output=True, text=True, timeout=45)
    (evidence / f'core1-run-{run}.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, (run, result.returncode, result.stderr)
    events = [json.loads(line) for line in (dest / 'events.jsonl').read_text().splitlines()]
    one = lambda name: next(e for e in events if e['event'] == name)
    assert one('boot_procedure')['original_func_80000450_executed']
    dma = one('boot_dma_complete')
    assert dma['copy_validated'] and dma['status'] == 0 and dma['bytes'] == 100848
    spans = [e for e in events if e['event'] == 'decompression_after']
    assert len(spans) == 2 and all(e['outcome'] == 'pass' for e in spans)
    ready = one('core1_ready')
    assert ready['bytes_match'] and ready['first_mismatch'] is None
    assert ready['checksums_actual'] == ready['checksums_expected']
    assert ready['core2_actual'] == ready['core2_expected']
    assert ready['guards_preserved'] and ready['compressed_source_preserved']
    registered = one('core1_registered')
    assert registered['real_entry_mapping_verified']
    stop = one('core1_boundary')
    assert stop['outcome'] == 'diagnostic_stop' and stop['guest_pc'] == '0x80012030'
    assert stop['original_func_80000450_executed'] and not stop['original_func_80012030_executed']
    assert stop['sp'] == '0x800064C0'
    assert one('boundary_restored')['real_entry_mapping_verified']
    assert not any(e['outcome'] == 'failure' for e in events)
print('PASS three fresh processes: generated decompressor, bytes, CRCs, ranges, guards, real registry and restored boundary')
