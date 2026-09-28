"""Continuous mode must execute main and dispose owned runtime resources on failure."""
import json, subprocess, sys
from pathlib import Path
exe,rom,out=Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3])
out.mkdir(parents=True,exist_ok=False)
cmd=[str(exe),'--rom',str(rom),'--trace-dir',str(out/'trace'),'--run-continuous','--run-seconds','2','--save-dir',str(out/'save')]
with (out/'process.log').open('w') as log:
    p=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=20)
(out/'command.json').write_text(json.dumps(dict(argv=cmd,exit_code=p.returncode),indent=2)+'\n')
assert p.returncode in (0,3), f'Continuous CLI/runtime must return handled outcome, got {p.returncode}'
events=[json.loads(line) for line in (out/'trace/events.jsonl').read_text().splitlines()]
assert any(e.get('event')=='continuous_main_entered' for e in events), 'Original main never began'
assert any(e.get('event')=='continuous_shutdown_complete' for e in events), 'No verified shutdown'
print('PASS continuous original main and owned shutdown')
