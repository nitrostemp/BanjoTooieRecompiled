import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "instrument_continuous", ROOT / "tools" / "instrument_continuous.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
LIFT_SPEC = importlib.util.spec_from_file_location(
    "lift_persistent_continuations", ROOT / "tools" / "lift_persistent_continuations.py")
LIFT = importlib.util.module_from_spec(LIFT_SPEC)
LIFT_SPEC.loader.exec_module(LIFT)


class CallsiteObservationOrderTest(unittest.TestCase):
    def test_native_helper_does_not_consume_guest_call_origin(self):
        lines = [
            "RECOMP_FUNC void f(uint8_t* rdram, recomp_context* ctx) {\n",
            "    // 0x80000000: jal         0x80000100\n",
            "    // 0x80000004: nop\n",
            "    tooie_hud_counter_adjust(rdram, ctx);\n",
            "    target(rdram, ctx);\n",
            ";}\n",
        ]
        self.assertEqual(
            MODULE.callsite_observations(lines, {"target": 0x80000100}),
            {4: (0x80000000, "direct_stub")},
        )

    def test_ordinary_call_still_consumes_origin(self):
        lines = [
            "RECOMP_FUNC void f(uint8_t* rdram, recomp_context* ctx) {\n",
            "    // 0x80000000: jal         0x80000100\n",
            "    // 0x80000004: nop\n",
            "    unrelated(rdram, ctx);\n",
            "    target(rdram, ctx);\n",
            ";}\n",
        ]
        with self.assertRaisesRegex(ValueError, "lacks adjacent original instruction"):
            MODULE.callsite_observations(lines, {"target": 0x80000100})


class RawGenerationOrderTest(unittest.TestCase):
    def test_backpack_bracket_accepts_pristine_call_and_closes_before_yield(self):
        source = """#include "funcs.h"
RECOMP_FUNC void babackpack_entrypoint_3(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    int c1cs = 0;
    // 0x808003FC: jal         0x800DE448
    // 0x80800400: lw          $a3, 0x38($sp)
    ctx->r7 = MEM_W(ctx->r29, 0X38);
    func_800DE448(rdram, ctx);
        goto after_14;
after_14:
    return;
;}\n"""
        changed, count = MODULE.apply_backpack_model_draw_hooks(source, "fixture")
        self.assertEqual(count, 2)
        self.assertIn(
            "tooie_model_backpack_draw_begin((uint32_t)ctx->r16);\n"
            "    func_800DE448(rdram, ctx);\n"
            "    tooie_model_backpack_draw_end();\n"
            "        goto after_14;", changed)
        body = next(LIFT.BODY.finditer(changed))[0]
        lifted, _ = LIFT.lift_body(body, 1,
            {"babackpack_entrypoint_3", "func_800DE448"})
        self.assertIn(
            "func_800DE448(rdram, ctx);\n"
            "    tooie_model_backpack_draw_end();\n"
            "    if (tooie_persist_yielded()) return;\n"
            "persistent_resume_1:", lifted)
        self.assertEqual(lifted.count("tooie_model_backpack_draw_end();"), 1)

    def test_replay_hooks_accept_raw_generator_shape(self):
        source = """#include "funcs.h"
RECOMP_FUNC void func_8001608C(uint8_t* rdram, recomp_context* ctx) {
    uint64_t hi = 0, lo = 0, result = 0;
    // 0x800160F0: jal         0x80800000
    // 0x800160F4: addiu       $a1, $sp, 0x50
    ctx->r5 = ADD32(ctx->r29, 0X50);
    _glrecord_entrypoint_0(rdram, ctx);
        goto after_3;
;}
"""
        changed, count = MODULE.apply_replay_timing_hooks(source)
        self.assertEqual(count, 2)
        self.assertIn(
            "ctx) {\n    tooie_replay_timing_begin_record();\n    uint64_t", changed)
        self.assertIn(
            "_glrecord_entrypoint_0(rdram, ctx);\n"
            "    tooie_replay_timing_record_divisor", changed)

    def test_native_aspect_accepts_raw_generator_shape(self):
        source = """#include "funcs.h"
RECOMP_FUNC void func_80015D14(uint8_t* rdram, recomp_context* ctx) {
L_80015D4C:
    // 0x80015D4C: jr          $ra
    // 0x80015D50: mov.s       $f0, $f2
;}
"""
        changed, count = MODULE.apply_native_aspect_hook(source)
        self.assertEqual(count, 1)
        self.assertIn(
            "L_80015D4C:\n"
            "    ctx->f2.fl = tooie_widescreen_adjust_projection_aspect(\n"
            "        ctx->f2.fl, ctx->r24 != 0);\n"
            "    // 0x80015D4C: jr", changed)


if __name__ == "__main__":
    unittest.main()
