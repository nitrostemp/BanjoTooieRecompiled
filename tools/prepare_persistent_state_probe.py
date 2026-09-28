#!/usr/bin/env python3
"""Lift a bounded real generated call chain into persistent continuations.

Only writes derived probe code into the supplied existing build directory. Does
not change canonical generated game code or package extracted game functions.
Rejects unknown calls/locals: broad runtime coverage is NOT inferred from this
two-function proof. Original and lifted variants retain instruction bodies.
"""
import argparse
import hashlib
from pathlib import Path
import re
from lift_persistent_continuations import lift_body

NAMES = ("func_800137D4", "func_80013818", "func_8001A0F0")
CALLS = NAMES + ("osRecvMesg_recomp",)
YIELD_PC = 0x800138A8
PREAMBLE = "    uint64_t hi = 0, lo = 0, result = 0;\n    int c1cs = 0;\n"


def extract(text, name):
    pattern = rf"^RECOMP_FUNC void {name}\([^\n]*\n.*?^;}}\n"
    found = re.findall(pattern, text, re.M | re.S)
    if len(found) != 1:
        raise ValueError(f"Expected exactly one generated body for {name}")
    body = found[0]
    if body.count(PREAMBLE) != 1:
        raise ValueError(f"Generated local schema changed for {name}")
    body = re.sub(r"^    tooie_continuous_poll\([^\n]*\n", "", body, flags=re.M)
    unknown_locals = re.findall(
        r"^\s+(?:uint\d+_t|int\d+_t|int|float|double|gpr|fpr)\s+[^\n]+",
        body.replace(PREAMBLE, ""), re.M)
    if unknown_locals:
        raise ValueError(f"Unrepresented native locals in {name}: {unknown_locals}")
    if "LOOKUP_FUNC" in body or "recomp_syscall_handler" in body or "section_addresses" in body:
        raise ValueError(f"Dynamic/overlay dispatch needs a separate continuation schema: {name}")
    # Every standalone native/generated call needs an explicit classification.
    calls = re.findall(r"^\s*(\w+)\(rdram, ctx(?:,[^\n]*)?\);", body, re.M)
    if any(call not in CALLS for call in calls):
        raise ValueError(f"Unlifted native/guest call in {name}: {calls}")
    return body


def rename(body, prefix):
    for name in CALLS:
        body = re.sub(rf"\b{name}\b", prefix + name, body)
    return body


def lift(body, name, function_id):
    body, metadata = lift_body(body, function_id, set(NAMES),
        adapters={"osRecvMesg_recomp"}, checkpoint=YIELD_PC if name == NAMES[1] else None)
    if name == NAMES[0] and metadata["direct_calls"] != 1:
        raise ValueError("Caller no longer has exactly one child")
    if name == NAMES[2] and metadata["direct_calls"] != 1:
        raise ValueError("Receive wrapper no longer has exactly one HLE child")
    if name == NAMES[1] and metadata["direct_calls"]:
        raise ValueError("Leaf acquired a nested call")
    return rename(body, "lifted_"), [point["pc"] for point in metadata["points"]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if not args.output_dir.is_dir():
        raise ValueError("Use an existing build directory")
    text = args.source.read_text(encoding="utf-8")
    receive_text = (args.source.parent / "funcs_5.c").read_text(encoding="utf-8")
    bodies = [extract(text if name != NAMES[2] else receive_text, name) for name in NAMES]
    identity = hashlib.sha256(
        Path(__file__).read_bytes() + Path(__file__).with_name("lift_persistent_continuations.py").read_bytes()
        + b"\0" + "".join(bodies).encode()).hexdigest()
    header = ['#pragma once\n#include "persistent_state_continuation.hpp"\n',
              f'inline constexpr const char* continuation_program_sha256="{identity}";\n',
              f"inline constexpr std::uint32_t continuation_yield_pc=0x{YIELD_PC:X}u;\n"]
    source = ['#include "persistent_state_probe_generated.hpp"\n#include <stdexcept>\n',
              '#include "persistent_state_hooks.h"\n',
              "#undef RECOMP_FUNC\n#define RECOMP_FUNC\n"]
    for prefix in ("original_", "lifted_"):
        for name in CALLS:
            header.append(f"void {prefix}{name}(uint8_t*,recomp_context*);\n")
    registry = []
    for number, (name, body) in enumerate(zip(NAMES, bodies), 1):
        source.append(rename(body, "original_"))
        transformed, pcs = lift(body, name, number)
        source.append(transformed)
        header.append(f"inline constexpr std::uint32_t continuation_pcs_{number}[]={{" +
                      ",".join(str(pc) + "u" for pc in pcs) + "};\n")
        registry.append(f"    {{{number},lifted_{name},continuation_pcs_{number}}}")
    header.append("inline const tooie::continuation::Function continuation_functions[]={\n" +
                  ",\n".join(registry) +
                  ",\n    {4,lifted_osRecvMesg_recomp,continuation_receive_pcs}\n};\n")
    header.insert(1, '#include "persistent_state_hle_probe.hpp"\n')
    for filename, parts in (("persistent_state_probe_generated.hpp", header),
                            ("persistent_state_probe_generated.cpp", source)):
        destination = args.output_dir / filename
        content = "".join(parts)
        if not destination.exists() or destination.read_text(encoding="utf-8") != content:
            destination.write_text(content, encoding="utf-8", newline="\n")
    print(f"Lifted {len(NAMES)} original generated functions; program SHA256 {identity}")


if __name__ == "__main__":
    main()
