# Save checksum provenance

`src/cheat_save_reset.cpp` recomputes the checksum of Banjo-Tooie's save blocks
after clearing saved cheat flags. This note maps each step of that C++ code to
the original game's instructions in the pinned Banjo-Tooie decompilation
(`deps/banjo-tooie`), so the algorithm can be reviewed without a ROM.

## Game instruction mapping

| C++ behavior | Game instruction evidence in `deps/banjo-tooie` |
| --- | --- |
| Initial 64-bit state `0x8F809F473108B3C1` | `asm/nonmatchings/overlays/gl/crc/func_80800000_glcrc.s`, `80800004`–`80800024`: `lui`/`ori` construct both 32-bit halves and store them at stack `0x48/0x4C`. |
| Forward byte pass, add byte shifted by `shift & 15`, then advance shift by 7 and XOR mixed state into first accumulator | Same function, `80800064`–`808000A8`: `lbu`, `andi 0xF`, `sllv`, 64-bit addition via word halves and carry, call to `func_801168AC`, pointer increment, `addiu 7`, `xor $s3`. |
| Backward byte pass, shift advances by 3 and XOR into second accumulator | Same function, `808000B0`–`80800108`: pointer starts at end minus one, `lbu`/`andi`/`sllv`, the same mix call, pointer decrement, `addiu 3`, `xor $s4`. |
| Mixing transform | `asm/nonmatchings/core2/1EF0140/func_801168AC.s`, `801168AC`–`801168E4`: the 64-bit left/right shifts, OR, XOR, right shift by 20 and mask `0xFFF` match the C++ `mix` lambda at `src/cheat_save_reset.cpp:133`–`140`. Its sibling `func_80116850.s` performs the same transform on a global state. |
| First accumulator in high 32 bits, second in low 32 bits, stored as two big-endian game words | `func_80800000_glcrc.s`, `8080010C`–`80800114` writes `$s3` then `$s4` to consecutive words. `src/cheat_save_reset.cpp:153` assembles those words into a 64-bit result; `write_be64` at lines 42–45 emits the same byte order for the save file. |
| Hash all but the trailing eight checksum bytes of each block | `asm/nonmatchings/overlays/gl/crc/glcrc_entrypoint_0.s`, `80800140`–`80800150`, and `glcrc_entrypoint_1.s`, `80800170`–`80800180`, compute `end = start + length - 8` before calling the checksum function. `asm/nonmatchings/overlays/gl/savegame/glsavegame_entrypoint_0.s` at `80800204` and `glsavegame_entrypoint_10.s` at `80800730` use `0x1C0` as the game save block size. Thus `0x1C0 - 8 = 0x1B8` data bytes, matching `src/cheat_save_reset.cpp:23`–`24`. |

## Reference implementation

The [rare-n64-chksm C implementation](https://github.com/bryc/rare-n64-chksm/blob/master/rare-n64-chksm.c)
by bryc describes the same Rare type-A algorithm: forward and reverse passes,
shift steps 7 and 3, two XOR accumulators, and a high/low word combination. The
C++ code cites it. It uses the game's full seed `0x8F809F473108B3C1` from the
assembly, whereas that reference starts from `0x13108B3C1`; both seeds share the
low 33 bits, which are the only bits the first mix keeps, so they produce the
same checksum.

For `0x1B8` zero bytes the checksum is `0xB0E934687B9CF8C6`, which
`tests/cheat_save_reset_test.cpp` checks.
