#!/usr/bin/env python3
"""Assemble/verify an allowlisted Windows package; never publishes or uploads it.

By default the package is a local candidate whose receipt says
NOT_FOR_DISTRIBUTION. --release prepares a separate package whose receipt says
RELEASE_PREPARED; it requires a clean, matching build and an explicit
releaseApproval for this version in release/windows-files.json. Preparing a
release package does not publish it or settle any distribution question.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from codegen_provenance import validate_build, validate_sdl


ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = ROOT / "release" / "windows-files.json"
RECEIPT = "candidate-manifest.json"
IDENTITY_TOKEN = "{identity}"
PLAYER_README = "release/README.player.md"
LOCAL_STATUS = "NOT_FOR_DISTRIBUTION"
RELEASE_STATUS = "RELEASE_PREPARED"
DEFAULT_CANDIDATE = ROOT / "dist" / "windows-alpha-candidate"


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def safe_relative(text: str) -> PurePosixPath:
    path = PurePosixPath(text)
    if (not text or "\\" in text or ":" in text or "//" in text or str(path) != text or path.is_absolute()
            or any(part in ("", ".", "..") for part in path.parts)):
        raise ValueError(f"Unsafe package path: {text!r}")
    return path


def is_reparse(path: Path) -> bool:
    if not os.path.lexists(path):
        return False
    details = os.lstat(path)
    return path.is_symlink() or getattr(path, "is_junction", lambda: False)() or bool(getattr(details, "st_file_attributes", 0)
                                     & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def contained(output: Path, relative: PurePosixPath) -> Path:
    destination = output.joinpath(*relative.parts)
    if not destination.resolve().is_relative_to(output.resolve()):
        raise ValueError(f"Candidate path escapes output: {relative}")
    current = destination
    while current != output:
        if is_reparse(current):
            raise ValueError(f"Reparse point in candidate path: {current}")
        current = current.parent
    return destination


def run(*command: str, cwd: Path = ROOT, timeout: int = 30) -> str:
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=timeout, check=True)
    return result.stdout.strip()


def metadata_identity(build: Path, expected_names: set[str], version: str,
                      revision: str, state: str, allow_dirty: bool) -> str:
    executable = build / "TooieRecompiled.exe"
    report = json.loads(run(str(executable), "--build-metadata-info", timeout=30))
    if report.get("version") != version:
        raise ValueError("Executable version differs from candidate allowlist")
    if report.get("revision_at_configure") != revision[:12]:
        raise ValueError("Executable configure revision differs from current source HEAD")
    configured_state = report.get("git_state_at_configure")
    if configured_state != state or state not in {"clean", "dirty"}:
        raise ValueError("Executable configure Git state differs from current source state")
    if state != "clean" and not allow_dirty:
        raise ValueError("Source checkout is dirty; commit/reconfigure/rebuild, or use --allow-dirty for local engineering")
    selection = read_json(build / "build-metadata" / "selection.json")
    identity = report["identity"]
    if not re.fullmatch(r"[0-9a-f]{64}", identity) or identity != selection["identity"]:
        raise ValueError("Executable and build selection metadata identities differ")
    bundle = build / "runtime-data" / identity
    if Path(selection["bundle"]).resolve() != bundle.resolve():
        raise ValueError("Build selection points outside the executable's runtime bundle")
    if Path(report["directory"]).resolve() != bundle.resolve():
        raise ValueError("Executable loaded a different runtime metadata bundle")
    manifest_path = bundle / "manifest.json"
    if digest(manifest_path) != identity:
        raise ValueError("Runtime metadata manifest does not hash to the compiled identity")
    manifest = read_json(manifest_path)
    rows = manifest["files"]
    if rows != selection["files"] or rows != report["files"]:
        raise ValueError("Runtime metadata rows differ between manifest, selection, and executable")
    if {row["path"] for row in rows} != expected_names:
        raise ValueError("Runtime metadata payload differs from reviewed allowlist")
    for row in rows:
        source = bundle / safe_relative(row["path"])
        if source.stat().st_size != row["bytes"] or digest(source) != row["sha256"]:
            raise ValueError(f"Runtime metadata size or hash mismatch: {row['path']}")
    if digest(executable) != report["executable_sha256"]:
        raise ValueError("Executable changed since its metadata report")
    return identity


def source_for(path: PurePosixPath, build: Path) -> Path:
    if path.parts[0] == "runtime-data" or path.name in {
        "TooieRecompiled.exe", "SDL2.dll", "dxcompiler.dll", "recompcontrollerdb.txt"
    } and len(path.parts) == 1:
        base = build
    elif str(path) == "README.md":
        return source_for(safe_relative(PLAYER_README), build)
    else:
        base = ROOT
    source = base.joinpath(*path.parts)
    if not source.is_file() or source.is_symlink() or not source.resolve().is_relative_to(base.resolve()):
        raise ValueError(f"Missing or unsafe allowlisted source: {path}")
    return source


def expected_paths(allowlist: dict, identity: str) -> list[PurePosixPath]:
    if allowlist.get("schema") != 2 or "releaseApproval" not in allowlist:
        raise ValueError("Unrecognized package allowlist")
    raw = allowlist["files"]
    if len(raw) != len(set(raw)):
        raise ValueError("Duplicate allowlist paths")
    paths = [safe_relative(name.replace(IDENTITY_TOKEN, identity)) for name in raw]
    if len(paths) != len(set(paths)) or RECEIPT in {str(path) for path in paths}:
        raise ValueError("Duplicate or reserved package path")
    return paths


def all_files(folder: Path) -> set[str]:
    found: set[str] = set()
    for parent, directories, files in os.walk(folder, followlinks=False):
        for name in directories + files:
            entry = Path(parent) / name
            if is_reparse(entry):
                raise ValueError(f"Reparse point in candidate: {entry}")
            if not entry.resolve().is_relative_to(folder.resolve()):
                raise ValueError(f"Candidate entry escapes output: {entry}")
        for name in files:
            path = Path(parent) / name
            found.add(path.relative_to(folder).as_posix())
    return found


def verify(output: Path, paths: list[PurePosixPath], expected_hashes: dict[str, str]) -> None:
    expected = {str(path) for path in paths} | {RECEIPT}
    if set(expected_hashes) != expected - {RECEIPT}:
        raise ValueError("Candidate receipt hash list differs from allowlist")
    actual = all_files(output)
    if actual != expected:
        raise ValueError(f"Candidate file set differs: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}")
    for name, wanted in expected_hashes.items():
        if digest(output / safe_relative(name)) != wanted:
            raise ValueError(f"Candidate file hash mismatch: {name}")


def is_documentation(name: str) -> bool:
    path = safe_relative(name)
    return (name in {"README.md", "THIRD_PARTY_NOTICES.md", "CONTRIBUTING.md", PLAYER_README}
            or path.parts[0] == "docs" and path.suffix in {".md", ".txt"}
            or path.parts[:2] == ("third_party", "notices") and path.suffix in {".md", ".txt"}
            or path.parts[:2] == (".github", "ISSUE_TEMPLATE") and path.suffix == ".md")


def documentation_source_revision(receipt: dict, revision: str, state: str) -> str:
    source = receipt.get("sourceCommit", "")
    if (state != "clean" or receipt.get("sourceDirty") is not False
            or receipt.get("gitStateAtConfigure") != "clean"
            or not re.fullmatch(r"[0-9a-f]{40}", source)
            or receipt.get("revisionAtConfigure") != source[:12]):
        raise ValueError("Documentation refresh requires a clean checkout and clean identified binary baseline")
    if run("git", "rev-parse", source + "^{commit}") != source:
        raise ValueError("Binary source commit is unavailable")
    run("git", "merge-base", "--is-ancestor", source, revision)
    changed = run("git", "diff", "--name-only", "--no-renames", "-z", source, revision).split("\0")
    tooling = {"tools/package_windows_candidate.py", "tests/package_windows_candidate_test.py"}
    forbidden = [name for name in changed if name and name not in tooling and not is_documentation(name)]
    if forbidden:
        raise ValueError(f"Non-documentation source changes require a rebuilt candidate: {forbidden}")
    return source


def verify_documentation_payload(before: dict[str, str], after: dict[str, str]) -> None:
    if set(before) != set(after):
        raise ValueError("Documentation refresh cannot change the candidate file set")
    changed = [name for name in after if not is_documentation(name) and before[name] != after[name]]
    if changed:
        raise ValueError(f"Documentation refresh would change non-documentation payload: {changed}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "windows-dev")
    parser.add_argument("--output-dir", type=Path, default=None,
                        help="Default: dist/windows-alpha-candidate (local) or dist/windows-release (--release)")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--refresh", action="store_true", help="Replace only files from a verified prior local candidate")
    parser.add_argument("--docs-only", action="store_true", help="Refresh/verify documentation while preserving the existing binary source revision and payload")
    parser.add_argument("--allow-dirty", action="store_true", help="Local engineering only; candidate remains NOT_FOR_DISTRIBUTION")
    parser.add_argument("--release", action="store_true",
                        help="Prepare/verify a separate release package; requires releaseApproval for this version")
    parser.add_argument("--sdl-observer-sha256", default="",
                        help="Explicit approved private observer DLL hash; recorded as an exception to the lock")
    args = parser.parse_args()
    if args.verify_only and args.refresh:
        raise ValueError("Choose --verify-only or --refresh")
    if args.docs_only and (args.allow_dirty or not (args.refresh or args.verify_only)):
        raise ValueError("--docs-only requires --refresh or --verify-only, without --allow-dirty")
    if args.output_dir is None:
        args.output_dir = ROOT / "dist" / ("windows-release" if args.release else "windows-alpha-candidate")
    status = RELEASE_STATUS if args.release else LOCAL_STATUS
    if args.release:
        # A release package is always assembled fresh from a clean, matching
        # build; it never relabels or refreshes a local candidate.
        if args.refresh or args.docs_only or args.allow_dirty or args.sdl_observer_sha256:
            raise ValueError("--release cannot be combined with --refresh, --docs-only, --allow-dirty or an SDL override")
        if args.output_dir.resolve() == DEFAULT_CANDIDATE.resolve():
            raise ValueError("--release must not write to or verify the local candidate directory")
    build = args.build_dir.resolve()
    if any(is_reparse(path) for path in (args.output_dir, *args.output_dir.parents)):
        raise ValueError("Candidate output path crosses a reparse point")
    output = args.output_dir.resolve()
    if output == ROOT or output == build or output.is_relative_to(build):
        raise ValueError("Candidate output must be separate from source and build directories")
    if output.is_relative_to(ROOT) and not output.is_relative_to(ROOT / "dist"):
        raise ValueError("A repository-local candidate must be under dist/")
    allowlist = read_json(ALLOWLIST)
    if args.release and allowlist.get("releaseApproval") != allowlist.get("candidateVersion"):
        raise ValueError("--release requires release/windows-files.json releaseApproval to equal "
                         "candidateVersion; that is an explicit owner decision committed to source")
    revision = run("git", "rev-parse", "HEAD")
    state = "dirty" if run("git", "status", "--porcelain", "--untracked-files=normal") else "clean"
    old_hashes: dict[str, str] | None = None
    old: dict = {}
    if args.refresh or args.docs_only:
        if not output.is_dir():
            raise ValueError("No existing candidate to refresh/verify")
        old = read_json(output / RECEIPT)
        if old.get("reviewStatus") != LOCAL_STATUS or not isinstance(old.get("sha256"), dict):
            raise ValueError("Existing candidate receipt is missing or not local-only")
        old_hashes = old["sha256"]
        verify(output, [safe_relative(name) for name in old_hashes], old_hashes)
    source_revision = documentation_source_revision(old, revision, state) if args.docs_only else revision
    documentation_fields = ({"documentationCommit": revision,
                             "packagingToolSha256": digest(Path(__file__)),
                             "refreshMode": "documentation-only"} if args.docs_only else {})
    metadata_names = {
        name.split(IDENTITY_TOKEN + "/", 1)[1]
        for name in allowlist["files"] if name.startswith("runtime-data/" + IDENTITY_TOKEN + "/")
    } - {"manifest.json"}
    if not metadata_names or any(IDENTITY_TOKEN in name for name in metadata_names):
        raise ValueError("Runtime metadata allowlist is missing or malformed")
    identity = metadata_identity(build, metadata_names, allowlist["candidateVersion"],
                                 source_revision, state, args.allow_dirty)
    paths = expected_paths(allowlist, identity)
    sources = {str(path): source_for(path, build) for path in paths}
    hashes = {name: digest(source) for name, source in sources.items()}
    if args.docs_only:
        assert old_hashes is not None
        verify_documentation_payload(old_hashes, hashes)
        if old.get("metadataIdentity") != identity:
            raise ValueError("Documentation refresh cannot change the metadata identity")
    lock = read_json(ROOT / "dependencies.lock.json")
    codegen_build = validate_build(ROOT, build / 'TooieRecompiled.exe')
    validate_sdl(build / 'SDL2.dll',
                 lock['dependencies']['sdl2_windows']['files']['lib/x64/SDL2.dll'],
                 args.sdl_observer_sha256)
    provenance_fields = {'codegenBuildReceipt': codegen_build,
                         'sdlObserverSha256Override': args.sdl_observer_sha256.lower()}
    pins = {name: spec["commit"] for name, spec in lock["dependencies"].items() if "commit" in spec}
    submodules = {name: spec["submodules"] for name, spec in lock["dependencies"].items()
                  if "submodules" in spec}
    archives = {name: spec["sha256"] for name, spec in lock["dependencies"].items() if "sha256" in spec}
    archive_files = {name: spec["files"] for name, spec in lock["dependencies"].items()
                     if spec.get("kind") == "archive" and "files" in spec}
    if args.verify_only:
        receipt = read_json(output / RECEIPT)
        expected_receipt = {
            "reviewStatus": status,
            "version": allowlist["candidateVersion"],
            "sourceCommit": source_revision,
            "sourceDirty": state == "dirty",
            "revisionAtConfigure": source_revision[:12],
            "gitStateAtConfigure": state,
            "metadataIdentity": identity,
            "dependencyLockSha256": digest(ROOT / "dependencies.lock.json"),
            "dependencyCommits": pins,
            "dependencySubmoduleCommits": submodules,
            "dependencyArchivesSha256": archives,
            "dependencyArchiveFilesSha256": archive_files,
            "buildCommand": ["cmake --preset windows-dev", "cmake --build --preset windows-dev"],
            "sha256": hashes,
            **documentation_fields,
            **provenance_fields,
        }
        for name, wanted in expected_receipt.items():
            if receipt.get(name) != wanted:
                raise ValueError(f"Candidate receipt differs from current reviewed source/build: {name}")
        verify(output, paths, hashes)
        print(f"Verified package: {output} ({len(paths)} allowlisted files; {status})")
        return
    if not args.refresh and output.exists() and any(output.iterdir()):
        raise ValueError("Output directory is not empty; use --verify-only or --refresh")
    receipt = {
        "schema": 1,
        "reviewStatus": status,
        "version": allowlist["candidateVersion"],
        "sourceCommit": source_revision,
        "sourceDirty": state == "dirty",
        "revisionAtConfigure": source_revision[:12],
        "gitStateAtConfigure": state,
        "dependencyLockSha256": digest(ROOT / "dependencies.lock.json"),
        "dependencyCommits": pins,
        "dependencySubmoduleCommits": submodules,
        "dependencyArchivesSha256": archives,
        "dependencyArchiveFilesSha256": archive_files,
        "buildCommand": ["cmake --preset windows-dev", "cmake --build --preset windows-dev"],
        "metadataIdentity": identity,
        "sha256": hashes,
        **documentation_fields,
        **provenance_fields,
    }
    # A documentation refresh never replaces executable, DLL, asset or metadata files.
    writes = ({name: source for name, source in sources.items()
               if is_documentation(name) and old_hashes[name] != hashes[name]}
              if args.docs_only else sources)
    output.mkdir(parents=True, exist_ok=True)
    suffix = ".candidate-pending"
    if args.refresh:
        staged = [contained(output, safe_relative(name + suffix)) for name in writes]
        staged.append(contained(output, safe_relative(RECEIPT + suffix)))
        if any(path.exists() or path.is_symlink() for path in staged):
            raise ValueError("Unexplained pending file in candidate output")
    for name, source in writes.items():
        destination = contained(output, safe_relative(name))
        destination.parent.mkdir(parents=True, exist_ok=True)
        contained(output, safe_relative(name))
        staged_path = destination.with_name(destination.name + suffix) if args.refresh else destination
        with source.open("rb") as input_file, staged_path.open("xb") as output_file:
            shutil.copyfileobj(input_file, output_file)
    receipt_path = output / (RECEIPT + suffix if args.refresh else RECEIPT)
    with receipt_path.open("x", encoding="utf-8") as result:
        json.dump(receipt, result, indent=2, sort_keys=True)
        result.write("\n")
    if args.refresh:
        for name in writes:
            staged_path = output / safe_relative(name + suffix)
            if digest(staged_path) != hashes[name]:
                raise ValueError(f"Staged candidate hash mismatch: {name}")
        for name in writes:
            destination = contained(output, safe_relative(name))
            os.replace(destination.with_name(destination.name + suffix), destination)
        assert old_hashes is not None
        for name in old_hashes.keys() - sources.keys():
            removed = contained(output, safe_relative(name))
            removed.unlink()
            # Drop folders emptied by an inventory change, never the output root.
            parent = removed.parent
            while parent != output and not any(parent.iterdir()):
                parent.rmdir()
                parent = parent.parent
        os.replace(receipt_path, output / RECEIPT)
    verify(output, paths, hashes)
    print(f"Prepared package: {output} ({len(paths)} allowlisted files; {status})")


if __name__ == "__main__":
    main()
