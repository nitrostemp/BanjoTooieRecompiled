#!/usr/bin/env python3
"""Bootstrap pinned codegen dependencies and regenerate all local ROM-derived source."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Sequence


ROOT = Path(__file__).resolve().parents[1]
ROM_SHA256 = "9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583"
DECOMP_SHA256 = "8c9d316b2edca686ec8393ddf95d480a7dfd9879ef434dbcfc4ea3000f2ffe89"
MACOS = sys.platform == "darwin"
# GCC-only ultralib tools are never invoked for the IDO libultra_rom target.
MACOS_UNUSED_GCC_TOOLS = ("ar", "gcc", "strip-2.7")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_hash(path: Path, expected: str, label: str) -> str:
    if not path.is_file():
        raise FileNotFoundError(path)
    actual = sha256(path)
    if actual != expected:
        raise RuntimeError(f"{label} identity mismatch: {path} ({actual})")
    return actual


def validate_pinned_hashes() -> None:
    for label, value in (("ROM_SHA256", ROM_SHA256), ("DECOMP_SHA256", DECOMP_SHA256)):
        if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
            raise RuntimeError(f"invalid pinned SHA-256 literal {label}: {value!r}")


def run(command: Sequence[object], *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    printable = [str(value) for value in command]
    print("+", " ".join(printable), flush=True)
    subprocess.run(printable, cwd=cwd, check=True, env=env)


def macos_environment() -> dict[str, str]:
    """Host tools for the Linux-oriented decomp Makefiles on macOS."""
    missing = [tool for tool in ("gmake", "mips-linux-gnu-as", "mips-linux-gnu-ld",
                                 "mips-linux-gnu-objcopy", "mips-linux-gnu-ar")
               if shutil.which(tool) is None]
    if missing:
        raise RuntimeError(f"missing macOS codegen tools {missing}; "
                           "run: brew install make mips-linux-gnu-binutils fmt")
    env = dict(os.environ)
    # The assembler-driver shim stands in for mips-linux-gnu-gcc.
    env["PATH"] = os.pathsep.join([str(ROOT / "tools/macos"), env.get("PATH", "")])
    # Apple's clang finds the macOS SDK, whatever other clang is first on PATH.
    env["CC"] = "/usr/bin/clang"
    env["CXX"] = "/usr/bin/clang++"
    brew_prefix = subprocess.run(["brew", "--prefix"], text=True, capture_output=True)
    if brew_prefix.returncode == 0 and brew_prefix.stdout.strip():
        library = str(Path(brew_prefix.stdout.strip()) / "lib")
        env["LIBRARY_PATH"] = os.pathsep.join(
            path for path in (library, env.get("LIBRARY_PATH", "")) if path)
    return env


def executable(build: Path, name: str) -> Path:
    for candidate in (build / name, build / f"{name}.exe"):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"{name} was not produced under {build}")


def copy_tool_tree(source: Path, destination: Path) -> None:
    if not source.is_dir():
        raise FileNotFoundError(source)
    destination.mkdir(parents=True, exist_ok=True)
    for item in source.iterdir():
        if item.name == ".tooie-dependency.json":
            continue
        target = destination / item.name
        if item.is_dir():
            shutil.copytree(item, target, dirs_exist_ok=True)
        else:
            if not target.is_file() or sha256(target) != sha256(item):
                shutil.copy2(item, target)


def materialize_codegen_tools(deps: Path, decomp: Path) -> None:
    """Install verified archives where the pinned decomp Makefiles expect them."""
    tools = deps / "codegen-tools"
    ido = tools / "ido-5.3-recomp-linux"
    destinations = [decomp / "tools/ido", decomp / "lib/ultralib/tools/ido",
                    decomp / "lib/ultralib/tools/gcc"]
    # Refresh only pinned tool files; never recursively delete a supplied
    # dependency tree or unrelated files in its local tool directories.
    copy_tool_tree(ido, destinations[0])
    copy_tool_tree(ido, destinations[1])
    # Preserve the upstream extraction order: binutils 2.7 intentionally
    # replaces `ar` from 2.6 and adds `strip-2.7`.
    for source in (tools / "mips-binutils-2.6-linux",
                   tools / "mips-gcc-2.7.2-linux",
                   tools / "mips-binutils-2.7-linux"):
        copy_tool_tree(source, destinations[2])
    required = [destinations[0] / "cc", destinations[1] / "cc",
                destinations[2] / "ar", destinations[2] / "as",
                destinations[2] / "objcopy", destinations[2] / "gcc",
                destinations[2] / "strip", destinations[2] / "strip-2.7"]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError(f"materialized codegen tools are incomplete: {missing}")


def materialize_macos_codegen_tools(deps: Path, decomp: Path) -> None:
    """Install the verified macOS IDO; the decomp's IDO build needs no MIPS GCC."""
    ido = deps / "codegen-tools/ido-5.3-recomp-macos"
    copy_tool_tree(ido, decomp / "tools/ido")
    copy_tool_tree(ido, decomp / "lib/ultralib/tools/ido")
    # ultralib's tool setup requires these files to exist, but only libgultra
    # targets run them. Fail loudly if that ever changes.
    gcc_tools = decomp / "lib/ultralib/tools/gcc"
    gcc_tools.mkdir(parents=True, exist_ok=True)
    for name in MACOS_UNUSED_GCC_TOOLS:
        placeholder = gcc_tools / name
        placeholder.write_text(
            "#!/bin/sh\necho \"$0: GCC 2.7.2 tools are not installed on macOS "
            "(only libgultra targets use them)\" >&2\nexit 1\n", encoding="utf-8")
        placeholder.chmod(0o755)
    required = [decomp / "tools/ido/cc", decomp / "lib/ultralib/tools/ido/cc"]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError(f"materialized codegen tools are incomplete: {missing}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", required=True, type=Path,
                        help="user-owned NTSC-U big-endian Banjo-Tooie ROM")
    parser.add_argument("--deps-dir", type=Path, default=ROOT / "deps")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    parser.add_argument("--evidence", type=Path, default=ROOT / ".local-evidence")
    parser.add_argument("--reference-generated", type=Path,
                        help="optional retained generated/ tree for migration comparison")
    args = parser.parse_args()

    validate_pinned_hashes()

    if os.name == "nt":
        raise RuntimeError("Run this pipeline inside Linux/WSL; pass the ROM through a mounted path")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

    rom = args.rom.expanduser().resolve()
    deps = args.deps_dir.expanduser().resolve()
    evidence = args.evidence.expanduser().resolve()
    # Validate the user's source before creating or replacing any local ROM input.
    original_sha = require_hash(rom, ROM_SHA256, "NTSC-U ROM")

    env = macos_environment() if MACOS else None
    make = "gmake" if MACOS else "make"
    if MACOS:
        dependency_keys = ["banjo_tooie_decomp", "n64recomp_codegen", "ido_53_macos"]
    else:
        dependency_keys = ["banjo_tooie_decomp", "n64recomp_codegen", "ido_53_linux",
                           "mips_binutils_26_linux", "mips_gcc_272_linux",
                           "mips_binutils_27_linux"]
    bootstrap_command: list[object] = [sys.executable, ROOT / "tools/bootstrap_dependencies.py",
                                       "--deps-dir", deps]
    for key in dependency_keys:
        bootstrap_command.extend(["--only", key])
    run(bootstrap_command)
    decomp = deps / "banjo-tooie"
    n64recomp_source = deps / "N64Recomp-codegen"
    if MACOS:
        materialize_macos_codegen_tools(deps, decomp)
    else:
        materialize_codegen_tools(deps, decomp)
    local_rom = decomp / "baserom.us.z64"
    if local_rom.exists():
        require_hash(local_rom, ROM_SHA256, "existing decomp ROM")
    else:
        local_rom.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(rom, local_rom)
        require_hash(local_rom, ROM_SHA256, "copied decomp ROM")

    venv = ROOT / ".venv-codegen"
    venv_python = venv / "bin/python"
    if not venv_python.is_file():
        run([sys.executable, "-m", "venv", venv], env=env)
    requirements = [ROOT / "tools/requirements-codegen.txt",
                    decomp / "tools/requirements.txt",
                    decomp / "tools/splat/requirements.txt"]
    for requirement in requirements:
        if not requirement.is_file():
            raise FileNotFoundError(requirement)
    # Re-running pip is deliberate: the lock files, rather than mere venv presence,
    # determine whether this stage is complete.
    pip_command: list[object] = [venv_python, "-m", "pip", "install"]
    for requirement in requirements:
        pip_command.extend(["-r", requirement])
    run(pip_command, env=env)

    decompressed = decomp / "decompressed.us.z64"
    rebuilt = decomp / "build/us/banjotooie_decompressed.z64"
    elf = decomp / "build/us/banjotooie_decompressed.elf"
    decomp_complete = all(path.is_file() for path in (decompressed, rebuilt, elf))
    if decomp_complete:
        decomp_complete = (sha256(decompressed) == DECOMP_SHA256 and
                           sha256(rebuilt) == DECOMP_SHA256 and
                           decompressed.read_bytes() == rebuilt.read_bytes())
    if decomp_complete:
        print("verified existing decomp outputs; skipping decomp build", flush=True)
    else:
        make_var = f"PYTHON3_BIN={venv_python}"
        run([make, "setup", make_var], cwd=decomp, env=env)
        if MACOS:
            # Build ultralib first with GNU ar: macOS ar cannot index MIPS ELF
            # members, and AR must not reach the decomp's host-tool builds.
            run([make, "-C", "lib/ultralib", "VERSION=J", "TARGET=libultra_rom",
                 "NON_MATCHING=1", "AR=mips-linux-gnu-ar", f"-j{args.jobs}"],
                cwd=decomp, env=env)
        run([make, f"-j{args.jobs}", make_var], cwd=decomp, env=env)
    require_hash(decompressed, DECOMP_SHA256, "canonical decompressed ROM")
    require_hash(rebuilt, DECOMP_SHA256, "rebuilt decompressed ROM")
    if decompressed.read_bytes() != rebuilt.read_bytes():
        raise RuntimeError("decompressed.us.z64 and linked build ROM differ")
    if not elf.is_file():
        raise FileNotFoundError(elf)

    codegen_build = n64recomp_source / "build"
    # CMake's incremental configure/build safely verifies or refreshes this stage.
    run(["cmake", "-S", n64recomp_source, "-B", codegen_build,
         "-DCMAKE_BUILD_TYPE=Release"], env=env)
    run(["cmake", "--build", codegen_build, "--parallel", str(args.jobs)], env=env)
    n64recomp = executable(codegen_build, "N64Recomp")
    rsp_recomp = executable(codegen_build, "RSPRecomp")

    evidence.mkdir(parents=True, exist_ok=True)
    cpu_command: list[object] = [venv_python, ROOT / "tools/prepare_codegen.py",
        "--rom", local_rom, "--decomp-root", decomp, "--n64recomp", n64recomp,
        "--evidence", evidence / "codegen"]
    if args.reference_generated:
        cpu_command.extend(["--reference-generated", args.reference_generated.resolve()])
    run(cpu_command)
    audio_command: list[object] = [venv_python, ROOT / "tools/prepare_audio_rsp.py",
        "--rom", local_rom, "--decompressed-rom", decompressed,
        "--rsp-recomp", rsp_recomp, "--n64recomp-source", n64recomp_source,
        "--evidence", evidence / "audio"]
    if args.reference_generated:
        audio_command.extend(["--reference-generated",
                              args.reference_generated.resolve() / "audio_mission01"])
    run(audio_command)

    receipt = {
        "source_rom": str(rom),
        "source_rom_sha256": original_sha,
        "decompressed_rom_sha256": sha256(decompressed),
        "rebuilt_rom_sha256": sha256(rebuilt),
        "decompressed_matches_rebuilt": decompressed.read_bytes() == rebuilt.read_bytes(),
        "decomp_root": str(decomp),
        "n64recomp_root": str(n64recomp_source),
        "n64recomp_sha256": sha256(n64recomp),
        "rsp_recomp_sha256": sha256(rsp_recomp),
    }
    (evidence / "generate-local.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"generated local source under {ROOT / 'generated'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
