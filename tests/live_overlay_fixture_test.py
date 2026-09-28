"""Isolated cold-start fixture: actual overlay loader/heap/defrag, never gameplay."""
import argparse,json,subprocess
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',required=True,type=Path);parser.add_argument('--rom',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path);parser.add_argument('--seconds',type=int,default=30)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    cmd=[str(args.exe.resolve()),'--rom',str(args.rom.resolve()),'--run-continuous','--live-overlay-fixture','--run-seconds',str(args.seconds),'--trace-dir',str(out/'trace'),'--save-dir',str(out/'save')]
    with (out/'process.log').open('w') as log:
        process=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=args.seconds+30)
    (out/'command.json').write_text(json.dumps(dict(argv=cmd,exit_code=process.returncode),indent=2)+'\n')
    assert process.returncode==0,f'Isolated fixture must finish successfully, got {process.returncode}'
    events=[json.loads(line) for line in (out/'trace/events.jsonl').read_text().splitlines()]
    def one(name):
        found=[e for e in events if e.get('event')==name];assert len(found)==1,(name,len(found));return found[0]
    one('overlay_live_fixture_begin');one('overlay_live_fixture_simultaneous')
    move=one('overlay_live_fixture_move');reload=one('overlay_live_fixture_reload')
    assert move['delta']!=0 and move['dirty_bss_bytes_checked']>=4 and move['packed_relocations_checked']==23
    assert move['stale_dispatch_rejected'] and move['initialized_bytes_checked']==1248
    assert len({reload[k] for k in ('first_header','moved_header','reloaded_header')})==3
    one('overlay_live_fixture_protected_unload')
    restored=one('overlay_live_fixture_resources_restored')
    assert restored['heap_before']==restored['heap_after'] and restored['loaded_count']==0
    complete=one('overlay_live_fixture_complete');shutdown=one('continuous_shutdown_complete')
    context=one('overlay_live_fixture_context_verified')
    assert context['stack_bytes_compared']==9216 and context['gpr_values_compared']==32 and context['fpr_bit_patterns_compared']==32
    assert context['hi_lo_compared'] and context['f_odd_pointer_compared'] and context['status_and_float_mode_compared'] and not context['padding_compared']
    assert complete['post_restore_comparison_passed']
    assert complete['original_context_and_stack_restored'] and complete['sequence']<shutdown['sequence']
    assert not any(e.get('event')=='overlay_live_fixture_failed' for e in events)
    lifecycle=[e for e in events if e.get('event')=='overlay_lifecycle']
    injected=one('overlay_live_fixture_conditional_request');conditional=one('overlay_live_fixture_conditional_unload')
    assert injected['injected_fixture_word'] and not injected['natural_game_callsite_claimed']
    assert int(injected['injected_delay_word'],16)^int(injected['original_delay_word'],16)==0x18000000
    assert conditional['stored_caller_ra_restored'] and not conditional['freed_descriptor_dereferenced']
    assert conditional['heap_before']==conditional['heap_after'] and conditional['loaded_count_before']==conditional['loaded_count_after']==0
    conditional_events=[e for e in lifecycle if injected['sequence']<e['sequence']<conditional['sequence'] and e.get('overlay_id')==350]
    assert [e['outcome'] for e in conditional_events]==['request','published','entry','return','unloaded']
    publication=next(e for e in conditional_events if e['outcome']=='published')
    assert int(conditional['returned_descriptor'],16)==int(publication['text'],16)+0x410
    assert conditional['f0_high_word']==publication['header'] and int(conditional['f0_low_word'],16)==1
    for id in (350,181,679):
        assert any(e.get('overlay_id')==id and e.get('outcome')=='published' for e in lifecycle),(id,'no publication')
        assert any(e.get('overlay_id')==id and e.get('outcome')=='entry' for e in lifecycle),(id,'no actual entry')
        assert any(e.get('overlay_id')==id and e.get('outcome')=='unloaded' for e in lifecycle),(id,'no actual unload')
    summary=dict(status='PASS',scope='Isolated original-loader fixture, not gameplay',stable_ids=[350,181,679],
        first_header=reload['first_header'],moved_header=reload['moved_header'],reloaded_header=reload['reloaded_header'],
        initialized_move_bytes=move['initialized_bytes_checked'],dirty_bss_move_bytes=move['dirty_bss_bytes_checked'],
        packed_move_relocations=move['packed_relocations_checked'],stale_dispatch_rejected=True,
        heap_accounting_restored=True,guest_context_stack_restored=True,continuous_shutdown_verified=True,
        original_conditional_unload_verified=True,injected_conditional_request_flag=True)
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
