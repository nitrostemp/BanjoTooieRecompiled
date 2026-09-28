"""Fresh-process two-worker runtime prerequisite, separate from Tooie progress."""
from pathlib import Path
import hashlib,json,subprocess,sys
exe,rom,out=map(Path,sys.argv[1:])
out.mkdir(parents=True,exist_ok=True)
results=[]
for scenario in ['ordinary','never-started','entry-failure','delayed-worker']:
    dest=out/scenario
    command=[str(exe),str(rom),str(dest),scenario]
    process=subprocess.run(command,capture_output=True,text=True,timeout=30)
    (out/(scenario+'.log')).write_text(process.stdout+process.stderr)
    assert process.returncode==0,(scenario,process.returncode,process.stderr)
    trace=dest/'events.jsonl'
    results.append(dict(scenario=scenario,command=command,exit_code=0,trace=str(trace),trace_sha256=hashlib.sha256(trace.read_bytes()).hexdigest()))
    (out/'results.json').write_text(json.dumps(dict(status='PASS' if len(results)==4 else 'PARTIAL',runtime_only=True,results=results),indent=2)+'\n')
print('PASS four fresh-process runtime priority/cleanup prerequisite cases; no Tooie handoff claim')
