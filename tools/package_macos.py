#!/usr/bin/env python3
"""Assemble a macOS app package from a built tree; never publishes or uploads it.

The package folder holds BanjoTooieRecompiled.app beside the notes and notices
allowlisted in release/macos-files.json. The app carries the executable, its
verified runtime metadata, assets and controller database in Resources, and
the Homebrew libraries it links (SDL2 through sdl2-compat, the SDL3 that
sdl2-compat loads at run time, and OpenSSL's libcrypto) in Frameworks. It is
signed ad hoc. No ROM is included; a final scan rejects any N64 ROM image.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from codegen_provenance import validate_build
from package_windows_candidate import digest, metadata_identity, read_json, run, safe_relative


ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = ROOT / "release" / "macos-files.json"
PLAYER_README = ROOT / "release" / "README.macos.md"
EXECUTABLE = "TooieRecompiled"
APP_NAME = "BanjoTooieRecompiled.app"
PACKAGE_NAME = "BanjoTooieRecompiled"
HOMEBREW_PREFIXES = ("/opt/homebrew/", "/usr/local/")
FRAMEWORKS_REFERENCE = "@executable_path/../Frameworks/"
N64_ROM_MAGIC = bytes.fromhex("80371240")


def tool(*command: str) -> str:
    return subprocess.run(command, capture_output=True, text=True, check=True).stdout


def homebrew_dependencies(binary: Path) -> list[str]:
    own_id = tool("otool", "-D", str(binary)).splitlines()[1:]
    references = []
    for line in tool("otool", "-L", str(binary)).splitlines()[1:]:
        reference = line.strip().split(" (", 1)[0]
        if reference.startswith(HOMEBREW_PREFIXES) and reference not in own_id:
            references.append(reference)
    return references


def bundle_library(source: Path, name: str, frameworks: Path) -> Path:
    target = frameworks / name
    shutil.copyfile(source.resolve(), target)
    target.chmod(0o644)
    tool("install_name_tool", "-id", FRAMEWORKS_REFERENCE + name, str(target))
    return target


def bundle_homebrew_libraries(executable: Path, frameworks: Path) -> list[str]:
    frameworks.mkdir(parents=True)
    bundled: dict[str, Path] = {}
    pending = [executable]
    # sdl2-compat dlopens SDL3 from its own folder, so otool cannot see it.
    sdl3 = Path(tool("brew", "--prefix", "sdl3").strip()) / "lib" / "libSDL3.0.dylib"
    if not sdl3.is_file():
        raise ValueError(f"No SDL3 library at {sdl3}; run: brew install sdl3")
    bundled["libSDL3.dylib"] = bundle_library(sdl3, "libSDL3.dylib", frameworks)
    pending.append(bundled["libSDL3.dylib"])
    while pending:
        binary = pending.pop()
        for reference in homebrew_dependencies(binary):
            name = PurePosixPath(reference).name
            if name not in bundled:
                bundled[name] = bundle_library(Path(reference), name, frameworks)
                pending.append(bundled[name])
            tool("install_name_tool", "-change", reference, FRAMEWORKS_REFERENCE + name, str(binary))
    if not any(name.startswith("libSDL2") for name in bundled):
        raise ValueError("The executable does not link SDL2 from Homebrew; nothing to bundle")
    for binary in [executable, *bundled.values()]:
        remaining = homebrew_dependencies(binary)
        if remaining:
            raise ValueError(f"{binary.name} still links Homebrew: {remaining}")
    return sorted(bundled)


def minimum_macos(binaries: list[Path]) -> str:
    versions = []
    for binary in binaries:
        match = re.search(r"LC_BUILD_VERSION.*?\n\s*minos (\S+)", tool("otool", "-l", str(binary)), re.S)
        if not match:
            raise ValueError(f"No minimum macOS version recorded in {binary.name}")
        versions.append(tuple(int(part) for part in match.group(1).split(".")))
    return ".".join(str(part) for part in max(versions))


def write_info_plist(app: Path, version: str, minimum: str, identifier: str) -> None:
    numeric = re.match(r"\d+(\.\d+)*", version)
    if not numeric:
        raise ValueError(f"Version has no numeric prefix: {version}")
    info = {
        "CFBundleExecutable": EXECUTABLE,
        "CFBundleIdentifier": identifier,
        "CFBundleName": "BanjoTooieRecompiled",
        "CFBundleDisplayName": "Banjo-Tooie: Recompiled",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": numeric.group(0),
        "CFBundleVersion": numeric.group(0),
        "LSMinimumSystemVersion": minimum,
        "LSApplicationCategoryType": "public.app-category.games",
        "NSHighResolutionCapable": True,
    }
    with (app / "Contents" / "Info.plist").open("wb") as output:
        plistlib.dump(info, output)


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file() or source.is_symlink():
        raise ValueError(f"Package input is missing or not a regular file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def reject_roms(folder: Path) -> None:
    for path in folder.rglob("*"):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as source:
                if source.read(4) == N64_ROM_MAGIC:
                    raise ValueError(f"Package contains an N64 ROM image: {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build" / "macos-dev")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist" / "macos")
    parser.add_argument("--label", help="Name suffix for the ZIP, such as a release tag (default: the version)")
    parser.add_argument("--bundle-identifier", default="io.github.nitrostemp.BanjoTooieRecompiled")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="Package a build configured from uncommitted source (local engineering only)")
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise ValueError("Package the macOS build on macOS")
    build = args.build.resolve()
    output = args.output_dir.resolve()
    if not output.is_relative_to(ROOT / "dist"):
        raise ValueError("A repository-local package must be under dist/")
    allowlist = read_json(ALLOWLIST)
    version = allowlist["candidateVersion"]
    executable = build / EXECUTABLE
    validate_build(ROOT, executable)
    revision = run("git", "rev-parse", "HEAD")
    state = "dirty" if run("git", "status", "--porcelain", "--untracked-files=normal") else "clean"
    identity = metadata_identity(build, set(allowlist["runtimeData"]), version, revision, state,
                                 args.allow_dirty, executable_name=EXECUTABLE)

    if output.exists():
        shutil.rmtree(output)
    package = output / PACKAGE_NAME
    app = package / APP_NAME
    resources = app / "Contents" / "Resources"
    (app / "Contents" / "MacOS").mkdir(parents=True)
    packaged_executable = app / "Contents" / "MacOS" / EXECUTABLE
    copy_file(executable, packaged_executable)
    packaged_executable.chmod(0o755)
    # Drop the linker's debug map, which records local object-file paths.
    tool("strip", "-S", str(packaged_executable))
    for name in ["recompcontrollerdb.txt", "assets/InterVariable.ttf", "assets/SIL-OFL-1.1.txt",
                 "assets/FONT_COPYRIGHTS.txt", f"runtime-data/{identity}/manifest.json",
                 *(f"runtime-data/{identity}/{row}" for row in allowlist["runtimeData"])]:
        copy_file(build / safe_relative(name), resources / safe_relative(name))
    libraries = bundle_homebrew_libraries(packaged_executable, app / "Contents" / "Frameworks")
    frameworks = sorted((app / "Contents" / "Frameworks").iterdir())
    minimum = minimum_macos([packaged_executable, *frameworks])
    write_info_plist(app, version, minimum, args.bundle_identifier)

    for name in allowlist["files"]:
        relative = safe_relative(name)
        source = PLAYER_README if name == "README.md" else ROOT / relative
        copy_file(source, package / relative)
    for formula, license_name in allowlist["bundledLibraryLicenses"].items():
        prefix = Path(tool("brew", "--prefix", formula).strip())
        copy_file(prefix / license_name, package / "third_party" / "notices" / "bundled" / f"{formula}-{license_name}")
    for name, source in allowlist["sourceLicenses"].items():
        copy_file(ROOT / safe_relative(source), package / safe_relative(name))

    reject_roms(package)
    for library in frameworks:
        tool("codesign", "--force", "--sign", "-", str(library))
    tool("codesign", "--force", "--sign", "-", str(app))
    tool("codesign", "--verify", "--strict", str(app))
    label = args.label or version
    archive = output / f"{PACKAGE_NAME}-{label}-macOS-arm64.zip"
    tool("ditto", "-c", "-k", "--norsrc", "--noextattr", "--noacl", "--keepParent", str(package), str(archive))
    manifest = {
        "schema": 1, "version": version, "label": label, "sourceRevision": revision,
        "sourceState": state, "runtimeIdentity": identity, "minimumMacOS": minimum,
        "buildExecutableSha256": digest(executable), "bundledLibraries": libraries,
        "archive": archive.name, "archiveSha256": digest(archive),
        "sha256": {path.relative_to(package).as_posix(): digest(path)
                   for path in sorted(package.rglob("*")) if path.is_file()},
    }
    (output / "package-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Packaged {archive} (macOS {minimum} or later, source {state} at {revision[:12]})")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", None)
        print(f"package_macos: {error}{': ' + detail.strip() if detail else ''}", file=sys.stderr)
        raise SystemExit(1)
