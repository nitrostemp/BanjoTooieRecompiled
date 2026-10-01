# macOS source build

These instructions are for developers compiling the game on an Apple Silicon
Mac. Code generation and the game build both run natively; no Linux VM is
needed. Rendering uses RT64's Metal backend.

## Tools

- Xcode (not only the Command Line Tools) with its Metal Toolchain. Xcode 26
  and later download it separately: `xcodebuild -downloadComponent MetalToolchain`.
- Homebrew packages:

```bash
brew install cmake ninja make python mips-linux-gnu-binutils fmt sdl2-compat sdl3 openssl@3
```

`make` provides GNU make 4 as `gmake`, which the decompilation's Makefiles
need. `mips-linux-gnu-binutils` assembles and links the MIPS code. The
pipeline uses Apple's clang from `/usr/bin` even if another clang is first on
`PATH`.

## Source and dependencies

```bash
git clone https://github.com/nitrostemp/BanjoTooieRecompiled.git
cd BanjoTooieRecompiled
python3 tools/bootstrap_dependencies.py
```

On macOS the bootstrap also fetches the pinned macOS build of the IDO 5.3
static recompilation (`ido_53_macos` in `dependencies.lock.json`).

## Check the toolchain without a ROM

```bash
python3 tools/generate_local.py --toolchain-only
python3 tests/macos_assembler_shim_test.py
```

This prepares and builds every code generation tool that needs no ROM (IDO,
the decompilation's host tools, ultralib and the recompilers), then checks the
assembler driver against known instruction encodings. The `macos-toolchain`
job in `.github/workflows/source-checks.yml` runs the same steps on every push.

## Verify your ROM and generate local source

Place your own NTSC-U big-endian ROM at the ignored `baserom.us.z64` path and
check it:

```bash
shasum -a 256 baserom.us.z64
```

Required hash: `9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583`.
Then generate the local source:

```bash
python3 tools/generate_local.py --rom baserom.us.z64 --jobs "$(sysctl -n hw.ncpu)"
```

This is the pipeline described in [CODEGEN.md](CODEGEN.md), with macOS
substitutes for its Linux tools:

- IDO 5.3 is the decompals macOS build of the same release as the Linux pin.
- `tools/macos/mips-linux-gnu-gcc` stands in for the MIPS GCC driver, which
  the decompilation uses only to assemble: it preprocesses with Apple clang
  using GCC's MIPS predefines and assembles with `mips-linux-gnu-as`.
- ultralib is archived with `mips-linux-gnu-ar`, because macOS `ar` cannot
  index MIPS ELF objects.

The rebuilt decompressed ROM must still match byte for byte, so a host tool
difference stops generation rather than changing the output.

## Compile and launch

```bash
cmake --preset macos-arm64 -DTOOIE_PERSISTENT_RUNTIME_EXPERIMENT=ON
cmake --build --preset macos-arm64
./build/macos-dev/TooieRecompiled --profile-dir local/smoke-profile
```

As on Windows, the option compiles in Private Practice; omit it for a build
without that capability. A normal launch opens the launcher, where you select
your ROM. `--profile-dir` keeps a test profile apart from the normal one in
`~/Library/Application Support/BanjoTooieRecompiled`.

The player menu draws with ImGui's Metal backend (`src/imgui_backend_metal.cpp`),
because RT64's Inspector has only D3D12 and Vulkan renderers. Configure also
writes a checked copy of plume's `plume_apple.mm` that keeps window-update
callbacks from running after their window is destroyed. The RT64 checkout
itself is never modified.

## Package

```bash
python3 tools/package_macos.py
```

This writes `dist/macos/BanjoTooieRecompiled-<version>-macOS-arm64.zip`. The
zip holds `BanjoTooieRecompiled.app` with the Homebrew libraries it links,
signed ad hoc, beside the notes and notices listed in `release/macos-files.json`.
It also writes `package-manifest.json` with the SHA-256 of every file. The
packager checks the codegen provenance and the runtime metadata bundle. It
requires a clean, committed checkout unless you pass `--allow-dirty`. The
oldest macOS the package runs on is the newest that any bundled library was
built for. Release packages are therefore built on the oldest supported
GitHub runner by `.github/workflows/release-macos.yml`, which explains how to
give it access to a ROM.
