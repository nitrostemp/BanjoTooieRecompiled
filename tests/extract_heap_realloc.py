#!/usr/bin/env python3
"""Extract the one generated heap_realloc definition for the focused fixture."""

import re
import sys
from pathlib import Path


def extract(source: str) -> str:
    pattern = re.compile(
        r"(?m)^RECOMP_FUNC void heap_realloc\(uint8_t\* rdram, recomp_context\* ctx\) \{"
    )
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise RuntimeError(f"expected one heap_realloc definition, found {len(matches)}")

    start = matches[0].start()
    cursor = matches[0].end() - 1
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start : cursor + 1] + "\n"
        cursor += 1
    raise RuntimeError("unterminated heap_realloc definition")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: extract_heap_realloc.py SOURCE OUTPUT")
    source = Path(sys.argv[1]).read_text(encoding="utf-8").replace("\r\n", "\n")
    output = Path(sys.argv[2])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(extract(source), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
