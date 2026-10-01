# Local ROM code generation

Generated guest code and ROM-derived metadata are intentionally absent from the repository. Supply a user-owned NTSC-U big-endian ROM with SHA-256 `9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583`.

On Ubuntu/WSL, install the host tools used by the pinned decompilation and generators:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential binutils-mips-linux-gnu gcc-mips-linux-gnu \
  cmake ninja-build git python3 python3-venv python3-pip \
  libfmt-dev libtoml11-dev zlib1g-dev wget tar
```

`zlib1g-dev` supplies the link-time `-lz` dependency for the Tooie ROM tools. Python packages stay in the repository-local environment created by the orchestrator.

Run the complete pipeline from Linux/WSL with one command. It validates the source ROM before copying it, materializes the two pinned dependency checkouts, creates a pinned Python environment, rebuilds or verifies the decompilation outputs, builds the pinned generators, and creates the local CPU and RSP outputs:

```bash
python3 tools/generate_local.py --rom /path/to/banjotooie-ntsc-u.z64 --jobs 8
```

To compare local output against an existing reference corpus before compiling the application, add `--reference-generated /path/to/reference/generated`. Generated text must match after CRLF-to-LF normalization; binary references must match byte-for-byte. The receipt identifies files needing line-ending normalization. The comparison also permits the recorded ELF-container SHA fields in `core2-reference.json` and `overlay-validation.json` to differ, because checkout paths embedded in DWARF can change the whole ELF hash. The scripts still prove the decompressed and rebuilt ROMs are identical and validate ELF section bytes, overlay relocation metadata, and generated code against the ROM-backed reference.

The orchestrator is equivalent to this expanded sequence:

```bash
python3 tools/bootstrap_dependencies.py --deps-dir deps \
  --only banjo_tooie_decomp --only n64recomp_codegen \
  --only ido_53_linux --only mips_binutils_26_linux \
  --only mips_gcc_272_linux --only mips_binutils_27_linux
python3 -c 'from pathlib import Path; from tools.generate_local import materialize_codegen_tools; materialize_codegen_tools(Path("deps").resolve(), Path("deps/banjo-tooie").resolve())'
python3 -m venv .venv-codegen
.venv-codegen/bin/python -m pip install -r tools/requirements-codegen.txt \
  -r deps/banjo-tooie/tools/requirements.txt \
  -r deps/banjo-tooie/tools/splat/requirements.txt
# generate_local.py validates before copying the ROM to this ignored path
# cp /path/to/banjotooie-ntsc-u.z64 deps/banjo-tooie/baserom.us.z64
make -C deps/banjo-tooie setup PYTHON3_BIN="$PWD/.venv-codegen/bin/python"
make -C deps/banjo-tooie -j8 PYTHON3_BIN="$PWD/.venv-codegen/bin/python"
cmp deps/banjo-tooie/decompressed.us.z64 deps/banjo-tooie/build/us/banjotooie_decompressed.z64

cmake -S deps/N64Recomp-codegen -B deps/N64Recomp-codegen/build -DCMAKE_BUILD_TYPE=Release
cmake --build deps/N64Recomp-codegen/build --parallel 8

.venv-codegen/bin/python tools/prepare_codegen.py \
  --rom deps/banjo-tooie/baserom.us.z64 \
  --decomp-root deps/banjo-tooie \
  --n64recomp deps/N64Recomp-codegen/build/N64Recomp \
  --evidence .local-evidence/codegen

.venv-codegen/bin/python tools/prepare_audio_rsp.py \
  --rom deps/banjo-tooie/baserom.us.z64 \
  --decompressed-rom deps/banjo-tooie/decompressed.us.z64 \
  --rsp-recomp deps/N64Recomp-codegen/build/RSPRecomp \
  --n64recomp-source deps/N64Recomp-codegen \
  --evidence .local-evidence/audio
```

Both scripts fail closed on source/ROM identity drift. `prepare_codegen.py` derives the symbol dump, applies the reviewed boundary policy and every generated-C transform, and writes the complete local `generated/` tree. `config/boundary_adjustments.json`, `config/runtime_exclusions.json`, `config/startup_coverage.json`, and the scripts under `tools/` are authored inputs and belong in source control. `config/selected.syms.toml`, `config/overlay-ids.txt`, `config/symbol_renames.json`, `config/tooie.us.toml`, `.codegen/`, `.venv-codegen/`, `.local-evidence/`, and `generated/` are local derived outputs.

The supported clean-checkout path regenerates `native_fixture.h`, the boot references, and the IPL3 shape header directly from the validated local inputs; it does not read any other workspace.

These commands describe the Linux/WSL generation workflow used for the Windows
build. The same orchestrator runs natively on Apple Silicon macOS with macOS
substitutes for the Linux tools; see [MACOS.md](MACOS.md). ROMs,
generated code/data and generation receipts remain local and excluded from Git.
