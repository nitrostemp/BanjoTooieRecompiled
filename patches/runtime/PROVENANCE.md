# Producer-quiescent cleanup drain

`producer-quiescent-cleanup.patch` is a project change to the pinned
N64ModernRuntime `threads.cpp` (GPL-3.0). `tools/prepare_runtime_cleanup.py`
verifies the hashes below before applying it.

- Runtime revision: `ca568b6ad79b9029d14077f0c3ffa757727c5559`.
- Reference threads.cpp normalized-LF SHA256: `e3ba32a1e06ba1643adc10ccc29ef5e3429e8809270f787a181314fc5704e653` (the exact pinned content; checkout line endings may be LF or CRLF).
- Patch SHA256: `9b4261af055d9677c061065c1d7588e79fcec74f679dac6cf12d3c069e2319fb`.
- Generated threads.cpp SHA256: `70582b56f42f0ad6935cb9821f7582a788a12da9f3dff5aa48a67109a9501f74` (LF).

The generic runtime repair joins the cleaner and drains queued native contexts.
Before quit/drain, callers must forbid new producers, destroy parked workers while
synchronized, release their boundaries, and await native thread-exit notification
from every cleanup producer. Keep active-game state, mappings and RDRAM alive until
native worker exit and cleanup are complete. Boundary return alone is insufficient.

This patch does not stop workers or prevent later enqueues. The normal `recomp::start`
shutdown path has not established this precondition and is **not certified** by this
bounded diagnostic. This repair is separate from Tooie initialization adapters.

CMake substitutes a generated build-directory source in the existing ultramodern
target, preserving its settings. Generation checks the revision, exact input and
patch hashes, exact hunk location and output hash. References are never patched.
The build-directory `runtime-cleanup/threads.provenance.json` records actual paths
and hashes; `source-substitution.txt` records the single target source replacement.
