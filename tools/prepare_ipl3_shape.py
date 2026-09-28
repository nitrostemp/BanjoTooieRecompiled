"""Extract the bounded IPL3 validation words from the validated local ROM."""
from pathlib import Path
import argparse

SHAPE_OFFSETS=(0x084,0x088,0x08C,0x090,0x094,0x098,0x09C,0x0A0,0x0A4,0x0A8,
    0x4F0,0x4F8,0x4FC,0x500,0x510,0x520,0x524,0x528,0x52C,0x530,0x534,0x538,
    0x848,0x84C,0x850,0x854,0x858,0x85C,0x860,0x864)
FINGERPRINT_OFFSETS=(0x73C,0x750)
def prepare(rom_path,output):
    rom=rom_path.read_bytes();word=lambda offset:int.from_bytes(rom[offset:offset+4],'big')
    lines=['// Generated from the validated local user-owned ROM; do not commit.','#pragma once','#include <array>','#include <cstdint>','#include <utility>','namespace tooie::boot::detail {',
        'inline constexpr std::array<std::pair<uint32_t,uint32_t>, 30> ipl3_shape{{']
    lines += [f'    std::pair{{0x{offset:03X}u, 0x{word(offset):08X}u}},' for offset in SHAPE_OFFSETS]
    lines += ['}};','inline constexpr std::array<uint32_t,2> ipl3_fingerprints{{'+','.join(f'0x{word(o):08X}u' for o in FINGERPRINT_OFFSETS)+'}};','}']
    output.write_text('\n'.join(lines)+'\n')
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--rom',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();prepare(a.rom.resolve(),a.output.resolve())
if __name__=='__main__':main()
