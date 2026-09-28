import json
from pathlib import Path
import subprocess
import sys
exe,rom,evidence=map(Path,sys.argv[1:])
coverage=json.loads((Path(__file__).resolve().parents[1]/'generated/coverage_metadata.json').read_text())
names=['__osSiGetAccess_recomp','__osSiRawStartDma_recomp','__osSiRelAccess_recomp','osPfsInit_recomp','osViGetCurrentLine_recomp','recomp_syscall_handler']
evidence.mkdir(parents=True,exist_ok=True)
for name in names+['watchdog']:
    trace=evidence/name
    result=subprocess.run([str(exe),name,str(trace),str(rom)],capture_output=True,text=True,timeout=15)
    (evidence/(name+'.log')).write_text(result.stdout+result.stderr)
    assert result.returncode==(124 if name=='watchdog' else 3),(name,result.returncode,result.stderr)
    packet=json.loads((trace/'fault.json').read_text())
    if name=='watchdog':
        assert packet['events'][-1]['event']=='watchdog_timeout'
        assert len(packet['guest_state']['sections'])==coverage['selected_sections']
        assert packet['guest_state']['sp']=='0x800064E0'
        assert len(packet['guest_state']['memory_words'])==7
    else:
        assert packet['events'][-1]['symbol']==name
print('PASS six remaining fatal imports return failure and emit packets; watchdog exits 124 with stable guest/registry snapshot')
