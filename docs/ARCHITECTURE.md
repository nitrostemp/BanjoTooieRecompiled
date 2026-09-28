# Architecture

The project translates the original game's MIPS code locally with N64Recomp,
then runs it natively with N64ModernRuntime and renders with RT64. The launcher
and in-game menus are project code built on the Dear ImGui copy in the pinned
RT64 tree, with SDL input and project-owned settings. Users supply the
supported original ROM. The repository does not contain translated game code or
extracted game assets.

- `src/`: authored host integration, game-specific adaptations, controls, UI,
  save coordination, diagnostics, graphics and runtime adapters.
- `tools/`: local generation and canonical transformations. Generated guest
  hooks are maintained in `instrument_continuous.py`; runtime adaptations in
  `prepare_runtime_continuous.py` and the other explicitly applied patches.
- `cmake/`, `patches/`: pinned shared infrastructure integration. Project-owned
  overrides replace selected compilation units without editing upstream trees.
  Each patch directory records its upstream pin and hashes in `PROVENANCE.md`.
- `tests/`: focused behavior and transformation checks. Some require local
  licensed generation inputs; ROM-free checks are a separate narrow CI scope.
- `generated/`, `deps/`, `build/`, `inputs/`, `local/`: ignored local material.
  Build caches are reusable; they are not source-history snapshots.

Generated guest files compile as C. Host code is C++20. C-linkage wrappers expose
host helpers without making generated guest files C++. Preserve pinned header
ABI consistency across generated code, runtime and frontend.

The launcher validates the compressed USA ROM and owns config/input/menu flow.
The continuous host provides native SDL audio/input/window and RT64 rendering.
Task-associated metadata coordinates replay input rate and interpolation state.
Ordinary saves use the original game's writer acknowledgement, not host save
states.

## Profile

`src/profile_location.cpp` selects the profile folder once at startup, in this
order: an explicit `--profile-dir`, a `portable.txt` beside the executable (the
executable's folder becomes the profile), then the per-user default
(`%LOCALAPPDATA%\BanjoTooieRecompiled` on Windows). Each profile is locked for
the process lifetime. Restart, Return to Launcher and Private Practice relaunch
the executable with the same absolute `--profile-dir`; the child waits for the
parent to exit before it takes the lock.

The first default launch after the product rename copies an existing
`%LOCALAPPDATA%\TooieRecomp-frontend` profile into a staging folder, verifies
every file by size and SHA-256, confirms the source did not change, and then
renames the staging folder into place. The legacy folder is never modified, and
two existing profiles are never merged. Anything unexpected (links, a folder
that cannot be fully listed, a busy or changing source, a conflicting
destination, a failed copy) stops startup with an explanation instead of
opening an empty or partial profile.

Profile contents:

| Path | Contents |
| --- | --- |
| `config/` | `tooie_imgui.json` settings and controls, the validated stored ROM copy, mod folders |
| `saves/` | ordinary EEPROM save, its `.bak`, and cheat-reset backups |
| `logs/` | `session.jsonl` and `graphics-captures.jsonl` (size-limited, rotated) |
| `devices/rt64/` | RT64 data |
| `private-practice.tooie-state` | the single Private Practice state slot |

Stored settings do not contain absolute paths, so a profile can be copied as a
whole.

## Private Practice

Private Practice is compiled in with `TOOIE_PERSISTENT_RUNTIME_EXPERIMENT=ON`
and must be chosen per launch (`--practice-game`). The session seeds its own
in-memory EEPROM from a read-only copy of the profile's ordinary save and never
writes it back. Save State serializes the guest machine together with explicit,
versioned continuations for generated native frames; no host stack or pointer
is stored. The state file carries SHA-256 integrity and a compatibility
identity (generated program, executable, ROM, runtime revision and gameplay/
render settings). A state is loaded only when all of these match, so a rebuilt
executable rejects states saved by an earlier build.

## Build metadata

Build metadata is hashed and packaged beside the executable under
`runtime-data/<identity>/`; the executable verifies it before use and reports it
with `--build-metadata-info`. These files are generated locally and excluded
from the source repository. The Windows candidate deliberately includes only
the runtime configuration and core/overlay metadata listed in
`release/windows-files.json`, alongside the translated executable. The metadata
describes symbols and memory layouts; raw comparison bytes and extracted game
images are excluded by `tools/prepare_build_metadata.py`. Players still supply
their own ROM.

The source/contribution exclusion and the package allowlist serve different
purposes. Neither authorizes a public release: a candidate stays
`NOT_FOR_DISTRIBUTION` until the exact package is approved. Other generated
files, ROMs, saves and local build evidence must not be added to it. Native game
execution must not depend on historical engineering directories.
