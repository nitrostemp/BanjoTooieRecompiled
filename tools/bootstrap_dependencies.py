#!/usr/bin/env python3
"""Materialize the exact dependency graph recorded in dependencies.lock.json."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
LOCK = ROOT / "dependencies.lock.json"


def run(command: list[str], cwd: pathlib.Path | None = None) -> str:
    completed = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    if completed.returncode:
        raise RuntimeError(f"Command failed ({completed.returncode}): {' '.join(command)}\n{completed.stdout}")
    return completed.stdout.rstrip()


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_head(path: pathlib.Path) -> str:
    return run(["git", "rev-parse", "HEAD"], path)


def git_command(*arguments: str) -> list[str]:
    return ["git", "-c", "core.autocrlf=false", "-c", "core.longpaths=true", *arguments]


def verify_git(name: str, spec: dict[str, object], target: pathlib.Path) -> None:
    if not (target / ".git").exists():
        raise RuntimeError(f"{name}: no Git checkout at {target}")
    actual = git_head(target)
    expected = str(spec["commit"])
    if actual != expected:
        raise RuntimeError(f"{name}: expected {expected}, found {actual}")
    dirty = run(["git", "status", "--porcelain=v1", "--untracked-files=no"], target)
    if dirty:
        raise RuntimeError(f"{name}: dependency checkout is modified:\n{dirty}")
    expected_submodules = dict(spec.get("submodules", {}))
    if expected_submodules:
        output = run(["git", "submodule", "status", "--recursive"], target)
        actual_submodules: dict[str, str] = {}
        for line in output.splitlines():
            if not line:
                continue
            state = line[0]
            fields = line[1:].strip().split()
            if len(fields) < 2 or state != " ":
                raise RuntimeError(f"{name}: uninitialized or divergent submodule: {line}")
            actual_submodules[fields[1]] = fields[0]
        missing = sorted(set(expected_submodules) - set(actual_submodules))
        extra = sorted(set(actual_submodules) - set(expected_submodules))
        wrong = sorted(path for path, commit in expected_submodules.items()
                       if actual_submodules.get(path) != commit)
        if missing or extra or wrong:
            raise RuntimeError(
                f"{name}: submodule lock mismatch; missing={missing}, extra={extra}, wrong={wrong}")


def install_git(name: str, spec: dict[str, object], target: pathlib.Path) -> None:
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        run(git_command("init", str(target)))
    if not (target / ".git").exists():
        raise RuntimeError(f"{name}: existing target is not a Git checkout: {target}")
    run(["git", "config", "core.autocrlf", "false"], target)
    run(["git", "config", "core.longpaths", "true"], target)
    remotes = run(["git", "remote"], target).splitlines()
    if "origin" not in remotes:
        run(["git", "remote", "add", "origin", str(spec["url"])], target)
    elif run(["git", "remote", "get-url", "origin"], target) != spec["url"]:
        raise RuntimeError(f"{name}: existing origin does not match {spec['url']}")
    head = subprocess.run(["git", "rev-parse", "--verify", "HEAD"], cwd=target,
                          text=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if head.returncode or git_head(target) != spec["commit"]:
        dirty = run(["git", "status", "--porcelain=v1", "--untracked-files=all"], target)
        if dirty:
            raise RuntimeError(f"{name}: refusing to replace files in a partial/other checkout:\n{dirty}")
        run(git_command("-c", "protocol.version=2", "fetch", "--depth", "1",
                        "--filter=blob:none", "origin", str(spec["commit"])), target)
        run(git_command("checkout", "--detach", "FETCH_HEAD"), target)
    if spec.get("recursive_submodules"):
        run(git_command("-c", "protocol.version=2", "submodule", "update", "--init",
                        "--recursive", "--depth", "1", "--filter=blob:none"), target)
        run(git_command("submodule", "foreach", "--recursive",
                        "git config core.autocrlf false && git config core.longpaths true"), target)
    verify_git(name, spec, target)
    print(f"installed {name}: {spec['commit']}")


def download(url: str, destination: pathlib.Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "BanjoTooieRecompiled-bootstrap/1"})
    with urllib.request.urlopen(request) as response, temporary.open("wb") as output:
        shutil.copyfileobj(response, output, length=1024 * 1024)
    temporary.replace(destination)


def safe_archive_members(names: list[str]) -> None:
    for name in names:
        normalized = pathlib.PurePosixPath(name.replace("\\", "/"))
        if normalized.is_absolute() or ".." in normalized.parts:
            raise RuntimeError(f"Unsafe archive member: {name}")


def unpack_archive(archive: pathlib.Path, destination: pathlib.Path, strip_components: int) -> None:
    with tempfile.TemporaryDirectory(prefix="tooie-dependency-", dir=destination.parent) as temp_name:
        temp = pathlib.Path(temp_name)
        if zipfile.is_zipfile(archive):
            with zipfile.ZipFile(archive) as bundle:
                safe_archive_members(bundle.namelist())
                bundle.extractall(temp)
        else:
            with tarfile.open(archive, "r:*") as bundle:
                members = bundle.getmembers()
                safe_archive_members([member.name for member in members])
                if any(member.issym() or member.islnk() for member in members):
                    raise RuntimeError(f"Archive contains links and is not accepted: {archive}")
                bundle.extractall(temp)
        source = temp
        for _ in range(strip_components):
            entries = [entry for entry in source.iterdir()]
            if len(entries) != 1 or not entries[0].is_dir():
                raise RuntimeError(f"Cannot strip archive component from {archive}")
            source = entries[0]
        source.replace(destination)


def archive_marker(target: pathlib.Path) -> pathlib.Path:
    return target / ".tooie-dependency.json"


def verify_archive(name: str, spec: dict[str, object], target: pathlib.Path) -> None:
    marker = archive_marker(target)
    if not marker.is_file():
        raise RuntimeError(f"{name}: missing bootstrap marker at {marker}")
    state = json.loads(marker.read_text(encoding="utf-8"))
    if state.get("sha256") != spec["sha256"] or state.get("url") != spec["url"]:
        raise RuntimeError(f"{name}: bootstrap marker does not match dependencies.lock.json")
    for relative, expected in dict(spec.get("files", {})).items():
        installed = target / relative
        if not installed.is_file():
            raise RuntimeError(f"{name}: required installed file is missing: {relative}")
        actual = sha256(installed)
        if actual != expected:
            raise RuntimeError(
                f"{name}: installed file {relative} expected SHA-256 {expected}, found {actual}")


def install_archive(name: str, spec: dict[str, object], deps: pathlib.Path,
                    downloads: pathlib.Path) -> None:
    target = deps / str(spec["directory"])
    if target.exists():
        verify_archive(name, spec, target)
        print(f"verified {name}: {spec['sha256']}")
        return
    filename = pathlib.PurePosixPath(str(spec["url"])).name
    archive = downloads / filename
    if not archive.exists() or sha256(archive) != spec["sha256"]:
        if archive.exists():
            archive.unlink()
        print(f"downloading {name}: {spec['url']}")
        download(str(spec["url"]), archive)
    actual = sha256(archive)
    if actual != spec["sha256"]:
        raise RuntimeError(f"{name}: expected archive SHA-256 {spec['sha256']}, found {actual}")
    target.parent.mkdir(parents=True, exist_ok=True)
    unpack_archive(archive, target, int(spec.get("strip_components", 0)))
    archive_marker(target).write_text(json.dumps({
        "name": name, "url": spec["url"], "sha256": spec["sha256"]
    }, indent=2) + "\n", encoding="utf-8")
    verify_archive(name, spec, target)
    print(f"installed {name}: {spec['sha256']}")


def platform_matches(spec: dict[str, object]) -> bool:
    wanted = spec.get("platform")
    return wanted is None or (wanted == "windows" and os.name == "nt") or (
        wanted == "linux" and sys.platform.startswith("linux"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps-dir", type=pathlib.Path, default=ROOT / "deps")
    parser.add_argument("--downloads-dir", type=pathlib.Path, default=ROOT / "downloads")
    parser.add_argument("--only", action="append", default=[], metavar="NAME")
    parser.add_argument("--with-windows-toolchain", action="store_true",
                        help="also download the optional pinned LLVM 19.1.3 Windows archive")
    parser.add_argument("--verify", action="store_true", help="perform no network or filesystem writes")
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    dependencies: dict[str, dict[str, object]] = lock["dependencies"]
    selected = set(args.only) if args.only else set(dependencies)
    unknown = selected - set(dependencies)
    if unknown:
        parser.error(f"unknown dependencies: {', '.join(sorted(unknown))}")
    for name, spec in dependencies.items():
        if spec.get("reference_only") and not args.only:
            continue
        if name not in selected or not platform_matches(spec):
            continue
        if spec.get("optional_group") == "windows-toolchain" and not args.with_windows_toolchain:
            continue
        target = args.deps_dir.resolve() / str(spec["directory"])
        if spec["kind"] == "git":
            if args.verify:
                verify_git(name, spec, target)
                print(f"verified {name}: {spec['commit']}")
            else:
                install_git(name, spec, target)
        elif spec["kind"] == "archive":
            if args.verify:
                verify_archive(name, spec, target)
                print(f"verified {name}: {spec['sha256']}")
            else:
                install_archive(name, spec, args.deps_dir.resolve(), args.downloads_dir.resolve())
        else:
            raise RuntimeError(f"{name}: unsupported dependency kind {spec['kind']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"bootstrap error: {error}", file=sys.stderr)
        raise SystemExit(1)
