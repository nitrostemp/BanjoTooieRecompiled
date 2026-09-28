"""Actual original idle -> scheduled main entry, with bounded lifetime failures."""
import json
import os
from pathlib import Path
import subprocess
import sys

exe, rom, evidence = map(Path, sys.argv[1:])
evidence.mkdir(parents=True, exist_ok=True)


def run_case(name, failure=None, delayed=False):
    dest = evidence / name
    env = os.environ.copy()
    for key in ('TOOIE_TEST_THREAD_GATE_FAILURE', 'TOOIE_TEST_MAIN_GATE_FAILURE',
                'TOOIE_TEST_MAIN_GATE_DELAY'):
        env.pop(key, None)
    if failure:
        env['TOOIE_TEST_MAIN_GATE_FAILURE'] = failure
    if delayed:
        env['TOOIE_TEST_MAIN_GATE_DELAY'] = '1'
    result = subprocess.run([str(exe), '--rom', str(rom), '--trace-dir', str(dest),
                             '--stop-at', 'main-thread-entry'],
                            capture_output=True, text=True, timeout=45, env=env)
    (evidence / f'{name}.log').write_text(result.stdout + result.stderr)
    assert result.returncode == (3 if failure else 0), (name, result.returncode, result.stderr)
    rows = [json.loads(line) for line in (dest / 'events.jsonl').read_text().splitlines()]

    def one(event):
        matches = [r for r in rows if r['event'] == event]
        assert len(matches) == 1, (name, event, matches)
        return matches[0]

    ready = one('core1_ready')
    assert ready['bytes_match'] and ready['checksums_actual'] == ready['checksums_expected']
    assert ready['guards_preserved'] and ready['compressed_source_preserved']
    assert one('core1_bss_verified')['bss_zero']
    assert one('core1_initialization_verified')['guest_effects_validated']
    idle = one('idle_thread_entry')
    assert idle['runtime_dispatch_callback_observed'] and idle['guest_thread_id'] == 1
    assert one('launcher_guest_execution_finished')['sequence'] < idle['sequence']
    assert one('original_idle_body_entered')['sequence'] > idle['sequence']
    for number, address, buffer, count in ((1, '0x8007695C', '0x80076958', 1),
                                           (2, '0x800769B8', '0x80076978', 16)):
        q = one(f'main_queue_{number}_verified')
        assert q['queue'] == address and q['buffer'] == buffer and q['capacity'] == count
        assert q['wait_queues_null'] and q['valid_count'] == 0 and q['first'] == 0
    pi = one('main_pi_manager_boundary')
    assert pi['priority'] == 150 and pi['capacity'] == 16
    assert pi['native_import_empty'] and not pi['dma_completion_claimed']
    created = one('main_thread_created')
    assert created['guest_id'] == 6 and created['priority'] == 20
    assert created['initial_state_stopped'] and created['native_exit_armed_before_create_return']
    assert one('main_create_arguments')['stack_start'] == '0x80043388'
    assert one('main_create_arguments')['stack_top'] == '0x80045788'
    parked = one('main_gate_workers_parked')
    assert parked['rdram_retained'] and parked['creation_set_closed']
    exits = [r for r in rows if r['event'] == 'main_gate_native_thread_exited']
    assert len(exits) == 2 and {r['guest_id'] for r in exits} == {1, 6}
    assert all(r['cleanup_already_enqueued'] and r['rdram_retained'] for r in exits)
    assert [r['guest_id'] for r in exits] == ([6, 1] if failure == 'after-main-create' else [1, 6])
    producers = one('main_gate_producers_quiescent')
    assert producers['workers'] == 2 and producers['running_queue_empty']
    quit_row = one('main_gate_quit_after_native_exit')
    cleanup = one('main_gate_cleanup_complete')
    assert cleanup['context_deletion_count'] == 2 and cleanup['all_native_workers_joined']
    assert cleanup['per_worker_deletions'] == [1, 1] and not cleanup['salvage_used']
    restored = one('main_mapping_restored')
    assert restored['real_entry_mapping_verified']
    idle_restored = one('idle_mapping_restored')
    released = one('rdram_released_after_cleanup')
    assert parked['sequence'] < exits[0]['sequence'] < exits[1]['sequence'] < producers['sequence']
    assert producers['sequence'] < quit_row['sequence'] < cleanup['sequence'] < restored['sequence']
    assert restored['sequence'] < idle_restored['sequence'] < released['sequence']
    assert not any(r.get('original_main_body_executed', False) for r in rows)
    assert not any(r['event'] == 'main_start_returned_forbidden' for r in rows)
    if failure:
        assert one('main_gate_injected_failure')['site'] == failure
        assert not any(r['event'] == 'main_handoff_complete' for r in rows)
    else:
        assert not any(r['outcome'] == 'failure' for r in rows)
        assert one('main_handoff_complete')['bounded_priority_transfer']
    if failure != 'after-main-create':
        rare = one('main_rare_bookkeeping_verified')
        assert rare['native_context_preserved'] and rare['odd_storage_flags'] == [0, 0, 1]
        assert rare['rare_id_table'] == '0x80045788' and rare['stack_and_guards_preserved']
        start = one('main_start_from_original_idle')
        assert start['caller_id'] == 1 and start['caller_priority'] == 0
        entry = one('main_thread_entry')
        assert entry['guest_thread_id'] == 6 and entry['priority'] == 20
        assert entry['current_thread'] == '0x80045788' and entry['sp'] == '0x80045778'
        assert entry['ra'] == '0x800329A8' and entry['status_register'] == '0x0000FF01'
        assert entry['registers']['a0'] == '0x00000000' and entry['fr'] == 0 and entry['cu1'] == 0
        assert entry['fresh_context_valid'] and entry['idle_sole_running_queue_member']
        assert start['sequence'] < entry['sequence'] < parked['sequence']
    else:
        assert not any(r['event'] in ('main_thread_entry', 'main_start_from_original_idle') for r in rows)
    if delayed:
        retained = one('main_gate_delayed_release_deferred')
        read = one('main_gate_delayed_worker_read')
        assert retained['rdram_retained'] and retained['mapping_retained'] and retained['active_game_retained']
        assert retained['context_deletion_count'] == 0 and not retained['correctness_sleeps']
        assert retained['sequence'] < read['sequence'] < parked['sequence']


for index in range(1, 4):
    run_case(f'main-run-{index}')
run_case('failure-after-main-create', 'after-main-create')
run_case('failure-main-entry', 'main-entry')
run_case('delayed-worker', delayed=True)
print('PASS: 3 actual scheduled main handoffs, 2 synchronized failures, and delayed retention')
