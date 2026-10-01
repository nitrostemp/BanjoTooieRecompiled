"""ROM-free check of tools/macos/mips-linux-gnu-gcc against known encodings.

Assembles a small file the way the decompilation does (its ASFLAGS and include
paths, splat's `.include "macro.inc"`), then checks GCC's MIPS predefines, the
.text bytes and the exported glabel symbol. Needs the bootstrapped decomp and
Homebrew's mips-linux-gnu-binutils; skipped on other hosts.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

if sys.platform != "darwin":
    print("SKIP: the assembler driver shim is macOS-only")
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
decomp = root / "deps" / "banjo-tooie"
assert (decomp / "include" / "macro.inc").is_file(), "run tools/bootstrap_dependencies.py first"
shim = root / "tools" / "macos" / "mips-linux-gnu-gcc"
source = """\
#if __mips != 3 || _MIPS_ISA != _MIPS_ISA_MIPS3 || _MIPS_SIM != _ABIO32 || !defined(_LANGUAGE_ASSEMBLY)
#error "GCC MIPS predefines differ"
#endif
#ifdef __mips_abicalls
#error "-mno-abicalls predefine leaked"
#endif
#define VALUE 0x55
.include "macro.inc"
.section .text
glabel shim_test
    addiu $t0, $zero, 0x1234
    ori $t1, $zero, VALUE
    dsll32 $t0, $t0, 0
    jr $ra
    nop
.end shim_test
"""
expected = bytes.fromhex("24081234" "34090055" "0008403c" "03e00008" "00000000")
asflags = ["-march=vr4300", "-mabi=32", "-mgp32", "-mfp32", "-mips3", "-mno-abicalls", "-G0",
           "-fno-pic", "-gdwarf", "-c", "-x", "assembler-with-cpp", "-D_LANGUAGE_ASSEMBLY"]
cppflags = ["-I", "include", "-I", "lib/ultralib/include", "-I", "src",
            "-DBUILD_VERSION=VERSION_J", "-D_FINALROM", "-DF3DEX_GBI_2"]
with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    (temp / "shim_test.s").write_text(source)
    obj, text = temp / "shim_test.o", temp / "text.bin"
    subprocess.run([str(shim), *asflags, *cppflags, "-c", str(temp / "shim_test.s"), "-o", str(obj)],
                   cwd=decomp, check=True)
    subprocess.run(["mips-linux-gnu-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(text)],
                   check=True)
    data = text.read_bytes()
    assert data[:len(expected)] == expected, data.hex()
    assert not any(data[len(expected):]), "unexpected bytes after the test code"
    symbols = subprocess.run(["mips-linux-gnu-nm", str(obj)], capture_output=True, text=True, check=True).stdout
    assert " T shim_test" in symbols, symbols
print("PASS mips-linux-gnu-gcc shim: GCC predefines, macro.inc include, VR4300/MIPS III encodings")
