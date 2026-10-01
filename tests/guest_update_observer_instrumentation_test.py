from pathlib import Path

root = Path(__file__).resolve().parents[1]
generated = (root / "generated" / "funcs_122.c").read_text(encoding="utf-8")
hook = "tooie_guest_update_observed(MEM_H(0, (gpr)(int32_t)0x80127634U) != 0);"
anchor = "RECOMP_FUNC void func_800A73F4(uint8_t* rdram, recomp_context* ctx) {"
assert generated.count(anchor) == 1
assert generated.count(hook) == 1
body = generated.split(anchor, 1)[1].split("RECOMP_FUNC void ", 1)[0]
assert hook in body
