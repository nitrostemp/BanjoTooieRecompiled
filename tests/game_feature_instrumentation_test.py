import importlib.util
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("instrument_continuous", ROOT / "tools" / "instrument_continuous.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GameFeatureInstrumentationTests(unittest.TestCase):
    def test_followup_canonical_hooks_replay_to_installed_bodies(self):
        """The canonical transforms reproduce each currently installed hook.

        Build compilation covers the materialized C, while this check starts
        from an in-memory pre-hook body and proves regeneration lands the same
        function body at every new feature seam.
        """
        cases = (
            ("funcs_245.c", "func_808002A4_bafpctrl",
             MODULE.apply_first_person_analog_hooks, 2,
             ("    ctx->f0.fl = tooie_camera_first_person_axis(ctx->f0.fl, 1);\n",
              "    ctx->f0.fl = tooie_camera_first_person_axis(ctx->f0.fl, 0);\n"),
             "extern float tooie_camera_first_person_axis(float, int);\n"),
            ("funcs_160.c", "func_800E9F20", MODULE.apply_free_camera_hooks, 2,
             ("    tooie_free_camera_begin(rdram, ctx);\n",
              "    tooie_free_camera_end(rdram, ctx);\n"),
             ("extern void tooie_free_camera_begin(uint8_t*, recomp_context*);\n"
              "extern void tooie_free_camera_end(uint8_t*, recomp_context*);\n")),
            ("funcs_3.c", "func_80015D14", MODULE.apply_native_aspect_hook, 1,
             ("    ctx->f2.fl = tooie_widescreen_adjust_projection_aspect(\n"
              "        ctx->f2.fl, ctx->r24 != 0);\n",),
             "extern float tooie_widescreen_adjust_projection_aspect(float, int);\n"),
            ("funcs_177.c", "func_80101970", MODULE.apply_actor_draw_distance_hooks, 2,
             ("    ctx->f12.fl = tooie_actor_draw_distance_adjust(ctx->f12.fl);\n",) * 2,
             "extern float tooie_actor_draw_distance_adjust(float);\n"),
            ("funcs_173.c", "func_800FA508", MODULE.apply_hud_counter_layout_hook, 1,
             ("    tooie_hud_counter_adjust(rdram, ctx);\n",),
             "extern void tooie_hud_counter_adjust(uint8_t*, recomp_context*);\n"),
            ("funcs_326.c", "func_808005AC_chintroticker",
             MODULE.apply_cutscene_skip_input_hook, 1,
             ("    ctx->r2 = tooie_cutscene_skip_input((int)ctx->r2);\n",),
             "extern int tooie_cutscene_skip_input(int);\n"),
        )
        for filename, function, transform, expected_count, hooks, declarations in cases:
            with self.subTest(function=function):
                installed = (ROOT / "generated" / filename).read_text()
                pre_hook = installed
                for declaration in declarations.splitlines(keepends=True):
                    self.assertEqual(pre_hook.count(declaration), 1)
                    pre_hook = pre_hook.replace(declaration, "", 1)
                for hook in hooks:
                    self.assertGreaterEqual(pre_hook.count(hook), 1)
                    pre_hook = pre_hook.replace(hook, "", 1)
                replayed, count = transform(pre_hook, f"replay:{filename}")
                self.assertEqual(count, expected_count)
                pattern = rf"RECOMP_FUNC void {re.escape(function)}\([^\n]*\n.*?^;\}}\n"
                installed_body = re.search(pattern, installed, re.S | re.M)
                replayed_body = re.search(pattern, replayed, re.S | re.M)
                self.assertIsNotNone(installed_body)
                self.assertIsNotNone(replayed_body)
                self.assertEqual(replayed_body[0], installed_body[0])
                for declaration in declarations.splitlines(keepends=True):
                    self.assertEqual(replayed.count(declaration), 1)

    def test_exact_guest_entries_receive_hooks(self):
        source = (
            '#include "funcs.h"\n'
            'RECOMP_FUNC void bainput_update(uint8_t* rdram, recomp_context* ctx) {\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n'
            '    int c1cs = 0;\n'
            '    return;\n;}\n'
            'RECOMP_FUNC void func_80800018_glcutDll(uint8_t* rdram, recomp_context* ctx) {\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n'
            '    int c1cs = 0;\n'
            '    return;\n;}\n')
        result, count = MODULE.apply_game_feature_hooks(source)
        self.assertEqual(count, 2)
        self.assertEqual(result.count('tooie_cheats_tick(rdram, ctx);'), 1)
        self.assertEqual(result.count('tooie_cutscene_state_command(rdram, ctx, (uint32_t)ctx->r4);'), 1)
        self.assertEqual(result.count('extern void tooie_cheats_tick(uint8_t*, recomp_context*);'), 1)
        self.assertEqual(result.count('extern void tooie_cutscene_state_command(uint8_t*, recomp_context*, uint32_t);'), 1)

    def test_repeat_and_changed_entry_rejected(self):
        source = (
            '#include "funcs.h"\n'
            'RECOMP_FUNC void bainput_update(uint8_t* rdram, recomp_context* ctx) {\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n'
            '    int c1cs = 0;\n'
            '    return;\n;}\n')
        result, _ = MODULE.apply_game_feature_hooks(source)
        with self.assertRaises(ValueError):
            MODULE.apply_game_feature_hooks(result)
        changed = source.replace('int c1cs = 0;', 'int c1cs = 1;')
        with self.assertRaises(ValueError):
            MODULE.apply_game_feature_hooks(changed)

    def test_lifecycle_and_camera_followups_are_exact(self):
        source = (
            '#include "funcs.h"\n'
            'RECOMP_FUNC void func_800DA0B4(uint8_t* rdram, recomp_context* ctx) {\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n'
            '    int c1cs = 0;\n;}\n'
            'RECOMP_FUNC void func_800DA188(uint8_t* rdram, recomp_context* ctx) {\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n'
            '    int c1cs = 0;\n;}\n'
            'RECOMP_FUNC void func_800DAC10(uint8_t* rdram, recomp_context* ctx) {\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n'
            '    int c1cs = 0;\n;}\n'
            'RECOMP_FUNC void func_800A4878(uint8_t* rdram, recomp_context* ctx) {\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n'
            '    int c1cs = 0;\n'
            '    after_5:\n'
            '    // 0x800A4908: beq         $v0, $zero, L_800A4920\n'
            '    if (ctx->r2 == 0) {\n'
            '    }\n;}\n')
        result, count = MODULE.apply_game_feature_followup_hooks(source)
        self.assertEqual(count, 4)
        self.assertEqual(result.count('tooie_cheats_invalidate();'), 3)
        self.assertEqual(result.count('? tooie_camera_analog_apply(rdram, ctx) : -1;'), 1)
        self.assertEqual(result.count('if (tooie_camera_result >= 0)'), 1)
        self.assertEqual(result.count('ctx->r2 = tooie_camera_result;'), 1)
        self.assertEqual(result.count('extern void tooie_cheats_invalidate(void);'), 1)
        self.assertEqual(result.count('extern int tooie_camera_analog_apply(uint8_t*, recomp_context*);'), 1)
        with self.assertRaises(ValueError):
            MODULE.apply_game_feature_followup_hooks(result)


if __name__ == '__main__':
    unittest.main(verbosity=2)
