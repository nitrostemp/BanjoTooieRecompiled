"""Actual original-core1 handoffs, plus failure-path lifetime checks."""
import json
import os
from pathlib import Path
import subprocess
import sys

exe, rom, evidence = map(Path, sys.argv[1:])
evidence.mkdir(parents=True, exist_ok=True)


def run_case(name, failure=None):
    dest = evidence / name
    env = os.environ.copy()
    env.pop('TOOIE_TEST_THREAD_GATE_FAILURE', None)
    if failure:
        env['TOOIE_TEST_THREAD_GATE_FAILURE'] = failure
    result = subprocess.run(
        [str(exe), '--rom', str(rom), '--trace-dir', str(dest),
         '--stop-at', 'idle-thread-entry'],
        capture_output=True, text=True, timeout=45, env=env)
    (evidence / f'{name}.log').write_text(result.stdout + result.stderr)
    assert result.returncode == (3 if failure else 0), (name, result.returncode, result.stderr)
    rows = [json.loads(line) for line in (dest / 'events.jsonl').read_text().splitlines()]

    def one(event):
        matches = [row for row in rows if row['event'] == event]
        assert len(matches) == 1, (name, event, matches)
        return matches[0]

    ready = one('core1_ready')
    assert ready['bytes_match'] and ready['checksums_actual'] == ready['checksums_expected']
    assert ready['guards_preserved'] and ready['compressed_source_preserved']
    bss = one('core1_bss_verified')
    assert bss['bss_zero'] and bss['bss_bytes'] == 0x3E240
    assert bss['guards_preserved'] and bss['core1_image_preserved']
    assert one('core1_initialization_verified')['guest_effects_validated']
    exit_row = one('idle_native_thread_exited')
    assert exit_row['cleanup_already_enqueued'] and exit_row['no_future_producers']
    parked = one('idle_worker_parked')
    assert parked['rdram_retained'] and not parked['worker_released'] and not parked['native_exit']
    destroyed = one('idle_destroyed_while_parked')
    assert destroyed['native_context_cleared'] and destroyed['external_public_osDestroyThread']
    producers = one('idle_cleanup_producers_quiescent')
    assert producers['workers'] == 1 and producers['no_future_producers']
    quit_row = one('idle_quit_after_native_exit')
    assert quit_row['active_game_retained_through_native_exit']
    cleanup = one('idle_cleanup_complete')
    assert cleanup['context_deletion_count'] == 1 and cleanup['all_native_workers_joined']
    mapping = one('idle_mapping_restored')
    assert mapping['real_entry_mapping_verified']
    released = one('rdram_released_after_cleanup')
    assert (parked['sequence'] < destroyed['sequence'] < exit_row['sequence'] <
            producers['sequence'] < quit_row['sequence'] < cleanup['sequence'] <
            mapping['sequence'] < released['sequence'])
    assert all(not row.get('original_idle_body_executed', False) for row in rows)
    if failure:
        assert one('thread_gate_injected_failure')['site'] == failure
        assert not any(row['event'] == 'idle_handoff_complete' for row in rows)
        if failure == 'after-create':
            one('idle_cleanup_start')
            assert not any(row['event'] == 'idle_thread_started' for row in rows)
        else:
            one('idle_thread_started')
            assert not any(row['event'] == 'idle_thread_entry' for row in rows)
        return
    assert not any(row['outcome'] == 'failure' for row in rows)
    original = one('original_core1_entered')
    assert original['original_func_80012030_executed']
    created = one('idle_thread_created')
    assert created['guest_id'] == 1 and created['priority'] == 0
    assert created['runtime_context_created'] and created['initial_state_stopped']
    rare = one('idle_rare_bookkeeping_verified')
    assert rare['native_context_preserved'] and rare['odd_storage_flags'] == [0, 0, 0]
    assert rare['odd_storage_header'] == 0 and rare['rare_id_table'] == '0x800775F0'
    start = one('idle_thread_started')
    entry = one('idle_thread_entry')
    assert start['sequence'] < entry['sequence']
    assert entry['runtime_dispatch_callback_observed'] and entry['guest_thread_id'] == 1
    assert entry['current_thread'] == '0x800775F0' and entry['sp'] == '0x800775E0'
    assert entry['ra'] == '0x800329A8' and entry['status_register'] == '0x0000FF01'
    assert entry['registers']['a0'] == '0x00000000'
    assert entry['fr'] == 0 and entry['cu1'] == 0 and entry['odd_register_mapping_valid']
    assert one('launcher_guest_execution_finished')['sequence'] < entry['sequence']
    complete = one('idle_handoff_complete')
    assert complete['bounded_first_scheduled_tooie_thread']


for index in range(1, 4):
    run_case(f'idle-run-{index}')
run_case('failure-after-create', 'after-create')
run_case('failure-worker-entry', 'worker-entry')
print('PASS: 3 actual core1/idle handoffs and 2 synchronized failure-cleanup cases')
