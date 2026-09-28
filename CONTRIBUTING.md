# Contributing

Contributions that improve Banjo-Tooie Recompiled are welcome. You can help by
reporting a reproducible problem, discussing a proposed change in an issue, or
submitting a pull request. Please keep changes focused and explain the player
problem they address.

## Report a problem

Use the bug report template in the
[issue tracker](https://github.com/some-scurvy-dog/BanjoTooieRecompiled/issues).
Include the build version or executable hash, Windows version, GPU and driver,
controller if relevant, settings, game location, and concise steps to reproduce
the issue. Describe
what you expected and what happened. Say whether you repeated the issue and
what you tested.

F4 adds a marker to the profile logs; it does not make a screenshot. Review all
attachments and redact usernames, personal paths and other private details.
Never attach a ROM, save file, generated game data, credentials, or an entire
profile. Do not include unreviewed logs or captures. If a private artifact is
needed, discuss a safe way to share it with a maintainer first.

## Propose and submit a change

For a larger change, open an issue first so its goal and scope can be discussed.
Pull requests should explain the user-facing result, list the files or areas
changed, and report the checks actually performed. Link related issues and
include screenshots only when they help explain a visual change and contain no
private or game-derived material that should not be shared.

Do not describe a build, test, or gameplay check as successful unless it was
actually performed. Keep implementation evidence, focused checks, observed
runtime behavior and player acceptance distinct. A passing build does not
establish that every map, controller, setting or hardware configuration works.

## Build and verify

Read [the Windows build guide](docs/WINDOWS.md) before setting up a development
environment. [Code generation](docs/CODEGEN.md) describes the local generation
inputs and [architecture](docs/ARCHITECTURE.md) gives an overview of the source
tree. The project uses the user's own supported NTSC-U 1.0 ROM for local
generation. Do not commit the ROM, extracted game assets or generated game
source/data, or attach them to issues or pull requests. Do not include build
output, private profiles, saves, logs, captures or credentials in a contribution.

Maintainer-prepared Windows packages are a separate process: the executable
and the specific runtime metadata listed in `release/windows-files.json` are
intentional package inputs. This is not permission to upload other generated
files or a developer's build directory. A local candidate remains
`NOT_FOR_DISTRIBUTION` until its exact contents are reviewed and approved for
release. See [build metadata](docs/ARCHITECTURE.md#build-metadata).

Run checks appropriate to the change. For code changes, build the affected
target and run focused checks when available. For documentation changes, check
links, commands and labels against the current interface. In your pull request,
state the exact commands and observations; do not claim broader coverage than
you performed. `python tools/check_repository.py` runs the ROM-free source
checks that also run in CI.

Keep contributions to player, contributor, build and licensing material. Working
notes, plans, audits, investigations and status logs belong in your own local
files, not in the repository.

## Code, assets and attribution

Contribute only material you have the right to submit. Preserve copyright
notices and attribution, and explain the source and license of copied or adapted
code and assets. Do not submit extracted game assets, translated game content,
ROM data or other copyrighted game material. Preserve all applicable project
and third-party notices. See [LICENSE](LICENSE) and
[third-party notices](THIRD_PARTY_NOTICES.md).

This project uses N64Recomp, N64ModernRuntime, RT64, Dear ImGui and other
third-party projects. Their licenses and contribution rules remain their own.
This project is independently maintained and has no upstream endorsement.
Respect each upstream project's policy when contributing to that project.

AI tools generated substantial portions of this project's code and assisted
with engineering and review. The project owner has directed development,
tested the game and verified its behavior; generated code was not manually
written by the owner. Contributors are responsible for the provenance,
correctness, licensing and testing of their own submissions. Disclose material
AI assistance in a pull request and respect the contribution policies of any
upstream project; do not submit AI-generated work where that project prohibits
it.
