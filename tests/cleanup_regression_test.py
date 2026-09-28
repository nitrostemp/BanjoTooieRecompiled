"""Positive native cleanup gate. Every process must exit zero without salvage."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

exe, rom, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
cases = ['late-cleaner'] * 3 + ['ordinary', 'multiple-queued', 'empty', 'already-consumed', 'delayed-worker']
results = []
for index, case in enumerate(cases, 1):
    name = f'{index:02d}-{case}'
    trace = out / name
    command = [str(exe), str(rom), str(trace), case]
    process = subprocess.run(command, capture_output=True, text=True, timeout=25)
    (out / f'{name}.log').write_text(process.stdout + process.stderr)
    assert process.returncode == 0, (case, process.returncode, process.stderr)
    payload = (trace / 'events.jsonl').read_bytes()
    rows = [json.loads(line) for line in payload.splitlines()]
    names = [row['event'] for row in rows]
    assert names[-1] == 'cleanup_regression_pass'
    assert not rows[-1]['salvage_used'] and not rows[-1]['idle_handoff_acceptance']
    count = 0 if case == 'empty' else 3 if case == 'multiple-queued' else 1
    for event in ['worker_dispatched', 'worker_destroyed_while_parked', 'native_worker_exited', 'context_deleted_exactly_once']:
        assert names.count(event) == count, (case, event)
    for row in rows:
        assert not row['original_core1_executed'] and not row['idle_handoff_acceptance']
        if row['event'] == 'context_deleted_exactly_once':
            assert row['deletion_count'] == 1
    assert names.index('all_cleanup_producers_quiescent') < names.index('quit_after_native_exit')
    assert names.index('cleaner_join_and_drain_returned') < names.index('native_mapping_restored') < names.index('rdram_released_after_cleanup')
    if case in ('ordinary', 'already-consumed'):
        assert names.index('cleaner_started_before_workers') < names.index('worker_dispatched')
        assert names.index('queue_consumed_before_quit') < names.index('quit_after_native_exit')
    else:
        assert names.index('completed_contexts_queued') < names.index('quit_after_native_exit') < names.index('cleaner_started_after_quit')
    if case == 'delayed-worker':
        assert names.index('release_deferred_for_delayed_worker') < names.index('delayed_worker_rdram_read') < names.index('native_worker_exited')
    results.append(dict(scenario=case, command=command, exit_code=process.returncode, workers=count,
                        trace=str(trace), trace_sha256=hashlib.sha256(payload).hexdigest()))
    (out / 'results.json').write_text(json.dumps(dict(status='PARTIAL' if len(results) < len(cases) else 'PASS',
        runtime_cleanup_only=True, successful_tooie_handoffs=0, results=results), indent=2) + '\n')
print('PASS: 8/8 fresh cleanup processes; late-cleaner 3/3; zero Tooie handoffs claimed')
