#!/usr/bin/env python3
"""Materialize the pinned RT64 call-work deduplication overlay."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
PIN = "6f1c2d99a4ea571c139f449c326fd176ba8f3496"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def normalized_lines(path: pathlib.Path) -> list[str]:
    try:
        text = path.read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise RuntimeError(f"Cannot read UTF-8 input {path}: {error}") from error
    if "\x00" in text:
        raise RuntimeError(f"NUL byte in text input: {path}")
    return text.replace("\r\n", "\n").replace("\r", "\n").splitlines(keepends=True)


def encoded(lines: list[str]) -> bytes:
    return "".join(lines).encode("utf-8")


def dependency_head(dependency: pathlib.Path) -> str:
    completed = subprocess.run(
        ["git", "-C", str(dependency), "rev-parse", "HEAD"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if completed.returncode:
        raise RuntimeError(f"Cannot read RT64 revision: {completed.stderr.strip()}")
    return completed.stdout.strip()


def materialize(recipe_path: pathlib.Path, dependency: pathlib.Path,
                destination: pathlib.Path, verify_only: bool) -> dict[str, object]:
    recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
    if recipe.get("schema") != 1:
        raise RuntimeError(f"Unsupported recipe schema in {recipe_path}")
    upstream = recipe["upstream"]
    output = recipe["output"]
    if upstream.get("commit") != PIN:
        raise RuntimeError(f"Recipe pin drift in {recipe_path}: {upstream.get('commit')}")

    input_path = dependency / upstream["path"]
    original = normalized_lines(input_path)
    if sha256(encoded(original)) != upstream["sha256"] or len(original) != upstream["lines"]:
        raise RuntimeError(f"Pinned upstream input drift: {input_path}")

    rebuilt: list[str] = []
    cursor = 0
    for index, operation in enumerate(recipe["operations"]):
        start = operation["original_start"]
        end = operation["original_end"]
        if (not isinstance(start, int) or not isinstance(end, int) or
                start < cursor or end < start or end > len(original)):
            raise RuntimeError(f"Invalid operation {index} in {recipe_path}")
        if sha256(encoded(original[start:end])) != operation["original_sha256"]:
            raise RuntimeError(f"Original span drift at operation {index} in {recipe_path}")
        replacement = operation["replacement_lines"]
        if not isinstance(replacement, list) or not all(isinstance(line, str) for line in replacement):
            raise RuntimeError(f"Invalid replacement lines at operation {index} in {recipe_path}")
        if any("\r" in line for line in replacement):
            raise RuntimeError(f"Non-LF replacement at operation {index} in {recipe_path}")
        rebuilt.extend(original[cursor:start])
        rebuilt.extend(replacement)
        cursor = end
    rebuilt.extend(original[cursor:])

    result = encoded(rebuilt)
    if sha256(result) != output["sha256"] or len(rebuilt) != output["lines"]:
        raise RuntimeError("Reconstructed RT64 overlay drift")
    if verify_only:
        if not destination.is_file() or destination.read_bytes() != result:
            raise RuntimeError(f"Materialized output is missing or stale: {destination}")
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or destination.read_bytes() != result:
            destination.write_bytes(result)
    return {
        "input": upstream["path"],
        "input_sha256": upstream["sha256"],
        "output": str(destination),
        "output_sha256": output["sha256"],
        "operations": len(recipe["operations"]),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependency", type=pathlib.Path, default=ROOT / "deps" / "rt64")
    parser.add_argument("--recipe", type=pathlib.Path,
                        default=ROOT / "patches" / "rt64" / "game_frame_matching.delta.json")
    parser.add_argument("--output", type=pathlib.Path,
                        default=ROOT / "generated" / "rt64" / "rt64_game_frame.cpp")
    parser.add_argument("--verify", action="store_true", help="verify an existing output without writing")
    args = parser.parse_args()
    dependency = args.dependency.resolve()
    if dependency_head(dependency) != PIN:
        raise RuntimeError(f"RT64 must be exactly {PIN}: {dependency}")
    result = materialize(args.recipe.resolve(), dependency, args.output.resolve(), args.verify)
    print(json.dumps({"schema": 1, "dependency_commit": PIN, "file": result}, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"RT64 matching materialization error: {error}", file=sys.stderr)
        raise SystemExit(1)
