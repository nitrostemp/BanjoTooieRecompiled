# Windows source build

These instructions are for developers compiling the game. Players use the Windows package described in the [player guide](PLAYER_GUIDE.md). WSL is used below for source generation; the resulting game is a native Windows executable. Windows x64 is the priority; a single tested setup does not establish support for every Windows version, GPU, or driver.

## Tools

- Git for Windows with long-path support, Python 3.10+, CMake 3.25+, Ninja.
- Visual Studio 2022 with Desktop development with C++ and a Windows SDK. Open an **x64 Developer PowerShell** for the Windows build.
- WSL 2 with Ubuntu for ROM-derived source generation. The first generation run needs network access for pinned inputs.

In Ubuntu, install the generation prerequisites:

```bash
sudo apt-get update
sudo apt-get install -y build-essential binutils-mips-linux-gnu gcc-mips-linux-gnu cmake ninja-build git python3 python3-venv python3-pip libfmt-dev libtoml11-dev zlib1g-dev wget tar
```

## Source and dependencies

Clone the repository and fetch the pinned dependencies:

```powershell
git clone https://github.com/some-scurvy-dog/BanjoTooieRecompiled.git BanjoTooieRecomp
Set-Location .\BanjoTooieRecomp
python .\tools\bootstrap_dependencies.py --with-windows-toolchain
python .\tools\bootstrap_dependencies.py --with-windows-toolchain --verify
```

The locked inputs are in `dependencies.lock.json`; the bootstrap places them under ignored `deps/`. Do not edit those checkouts in place. If access to the source repository or a pinned input fails, the build cannot continue until that input is available. A candidate release must name its exact source revision and dependency pins.

The frontend uses Dear ImGui from the pinned RT64 tree and project-owned SDL
input/settings code. The RecompFrontend entry in the lock file is a
reference-only pin that the default bootstrap skips; the build does not use it.
See [architecture](ARCHITECTURE.md) for an overview.

## Verify your ROM and generate local source

Place your own NTSC-U big-endian ROM at the ignored `baserom.us.z64` path. Confirm its SHA-256 before generation:

```powershell
(Get-FileHash -Algorithm SHA256 .\baserom.us.z64).Hash.ToLowerInvariant()
```

Required hash: `9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583`. Stop if it differs. In Ubuntu/WSL, enter the same checkout through `/mnt/c` and run:

```bash
cd /mnt/c/path/to/BanjoTooieRecomp
python3 tools/generate_local.py --rom baserom.us.z64 --jobs "$(nproc)"
```

Generation validates the ROM and locked tools, rebuilds and checks the local game inputs, then writes CPU/audio source and metadata under ignored `generated/`. It also writes local receipts under `.local-evidence/`. [CODEGEN.md](CODEGEN.md) gives expanded commands and checks.

## Compile and launch

In x64 Developer PowerShell at the checkout root:

```powershell
cmake --preset windows-dev -DTOOIE_PERSISTENT_RUNTIME_EXPERIMENT=ON
cmake --build --preset windows-dev
.\build\windows-dev\TooieRecompiled.exe --build-metadata-info | Out-String
```

The compile option includes Private Practice checkpoints in the build. Players
must still opt into a separate Private Practice session; normal gameplay keeps
ordinary saves and does not enable checkpoint tracking. Omit the option for a
build without this capability.

Pipe the metadata command to `Out-String` in PowerShell: the bare command produced
no visible output on the tested host, while the piped command printed the JSON
build identity, executable SHA-256, and runtime metadata hashes.

The executable is `build\windows-dev\TooieRecompiled.exe`. CMake copies SDL2/DXC DLLs, `assets/`, and `recompcontrollerdb.txt` beside it, and prepares the required `runtime-data/<identity>/` metadata bundle. The executable verifies that bundle before using it. A normal launch opens the embedded launcher, where you select your local ROM. For an isolated smoke test, use `--profile-dir .\local\smoke-profile`; normal play uses the profile described in [PLAYER_GUIDE.md](PLAYER_GUIDE.md).

The executable depends on the Microsoft Visual C++ x64 runtime (`MSVCP140.dll`, `MSVCP140_ATOMIC_WAIT.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` were observed in import inspection). Install the current [Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170) on the machine running it. This is a machine prerequisite, not a file to copy from a developer installation into a release package.

## Local package candidate

`release/windows-files.json` lists every file in the Windows package, and
`tools/package_windows_candidate.py` assembles and verifies it from a clean,
committed build. The packager checks the executable's recorded source revision,
the runtime metadata identity and hashes, code-generation provenance and the
locked SDL2 binary, then writes `candidate-manifest.json` with the SHA-256 of
every file. It does not publish anything:

```powershell
python .	ools\package_windows_candidate.py
python .	ools\package_windows_candidate.py --verify-only
```

Use `--refresh` to replace a previous candidate in place. A candidate's
`reviewStatus` stays `NOT_FOR_DISTRIBUTION` unless it is deliberately prepared
with `--release` from a clean checkout whose build matches the current commit.
