import importlib.util
from pathlib import Path
import re
import tempfile
import unittest
import shutil
import subprocess

TOOL = Path(__file__).resolve().parents[1] / 'tools/instrument_continuous.py'
spec = importlib.util.spec_from_file_location('instrument_continuous', TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

PREREQUISITES = '''RECOMP_FUNC void uncached(uint8_t* rdram, recomp_context* ctx) {
    ctx->r15 = MEM_W(ctx->r14, -0X4E0C);
    ctx->r25 = MEM_W(ctx->r24, -0X1E40);
;}
RECOMP_FUNC void func_80082088(uint8_t* rdram, recomp_context* ctx) {
    ctx->r10 = MEM_W(ctx->r9, 0X0);
    ctx->r10 = MEM_W(ctx->r9, 0X0);
    ctx->r10 = MEM_W(ctx->r9, 0X0);
    ctx->r21 = MEM_W(ctx->r21, 0X40);
    return;
;}
RECOMP_FUNC void ovl_unload(uint8_t* rdram, recomp_context* ctx) {
    heap_free(rdram, ctx);
;}
'''
SECTIONS = [{'functions': [
    {'name': '_test_entrypoint_0', 'vram': 0x80088540, 'size': 8},
    {'name': 'caller', 'vram': 0x80800000, 'size': 0x100},
    {'name': 'func_80082088', 'vram': 0x80082088, 'size': 0x200},
    {'name': 'ovl_unload', 'vram': 0x80083000, 'size': 0x40}]}]


def rewrite(body):
    source = PREREQUISITES + 'RECOMP_FUNC void caller(uint8_t* rdram, recomp_context* ctx) {\n' + body + ';}\n'
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'fixture.c'
        path.write_text(source)
        counts = module.instrument(folder, SECTIONS)
        return path.read_text(), counts


class CallsiteTests(unittest.TestCase):
    def test_run011_jalr_overrides_enclosing_direct_pc(self):
        body = '''    // 0x80800030: jal         0x80088540
    // 0x80800034: nop
    _test_entrypoint_0(rdram, ctx);
    // 0x8080004C: jalr        $v0
    // 0x80800050: nop
    LOOKUP_FUNC(ctx->r2)(rdram, ctx);
'''
        text, counts = rewrite(body)
        self.assertIn('tooie_overlay_callsite_push(0x8080004Cu);\n    LOOKUP_FUNC(ctx->r2)(rdram, ctx);\n    tooie_overlay_callsite_pop();', text)
        self.assertEqual(text.count('tooie_overlay_callsite_push('), 2)

    def test_indirect_tail_preserves_delay_slot_and_return(self):
        body = '''    // 0x8000042C: jr          $t2
    // 0x80000430: addiu       $sp, $sp, 0x64E0
    ctx->r29 = ADD32(ctx->r29, 0X64E0);
    LOOKUP_FUNC(ctx->r10)(rdram, ctx);
    return;
'''
        text, _ = rewrite(body)
        self.assertIn('tooie_overlay_callsite_push(0x8000042Cu);', text)
        self.assertIn('LOOKUP_FUNC(ctx->r10)(rdram, ctx);\n    tooie_overlay_callsite_pop();\n    return;', text)
        stripped = re.sub(r'^    tooie_overlay_callsite_(?:push\([^\n]*|pop\([^\n]*)\n', '', text, flags=re.M)
        self.assertIn(body, stripped)

    def test_direct_tail_jump_to_stub(self):
        text, _ = rewrite('''    // 0x80800080: j           0x80088540
    // 0x80800084: or          $a0, $s0, $zero
    ctx->r4 = ctx->r16 | 0;
    _test_entrypoint_0(rdram, ctx);
    return;
''')
        self.assertIn('tooie_overlay_callsite_push(0x80800080u);', text)

    def test_return_and_jump_table_are_not_calls(self):
        text, _ = rewrite('''    // 0x80800010: jr          $t9
    // 0x80800014: nop
    switch (ctx->r25) { default: break; }
    // 0x80800018: jr          $ra
    // 0x8080001C: nop
    return;
''')
        self.assertNotIn('tooie_overlay_callsite_push(', text)

    def test_lookup_without_origin_fails(self):
        with self.assertRaises((AssertionError, ValueError)):
            rewrite('    LOOKUP_FUNC(ctx->r2)(rdram, ctx);\n')

    def test_stale_jal_cannot_label_later_stub(self):
        with self.assertRaises((AssertionError, ValueError)):
            rewrite('''    // 0x80800000: jal         0x80012300
    // 0x80800004: nop
    ordinary(rdram, ctx);
    // 0x80800008: addiu       $v0, $zero, 0x1
    ctx->r2 = 1;
    _test_entrypoint_0(rdram, ctx);
''')

    def test_wrong_delay_pc_fails(self):
        with self.assertRaises((AssertionError, ValueError)):
            rewrite('''    // 0x8080004C: jalr        $v0
    // 0x80800054: nop
    LOOKUP_FUNC(ctx->r2)(rdram, ctx);
''')

    def test_wrong_register_fails(self):
        with self.assertRaises((AssertionError, ValueError)):
            rewrite('''    // 0x8080004C: jalr        $v0
    // 0x80800050: nop
    LOOKUP_FUNC(ctx->r25)(rdram, ctx);
''')

    def test_wrong_direct_target_fails(self):
        with self.assertRaises(ValueError):
            rewrite('''    // 0x8080004C: jal         0x80012300
    // 0x80800050: nop
    _test_entrypoint_0(rdram, ctx);
''')

    def test_unknown_lookup_syntax_fails(self):
        with self.assertRaises(ValueError):
            rewrite('''    // 0x8080004C: jalr        $v0
    // 0x80800050: nop
    LOOKUP_FUNC((uint32_t)ctx->r2)(rdram, ctx);
''')

    def test_constant_lookup_and_duplicate_delay_comment(self):
        text, counts = rewrite('''    // 0x80800020: jal         0x80088540
    // 0x80800024: nop
    LOOKUP_FUNC(0x80088540)(rdram, ctx);
        goto after_0;
    // 0x80800024: nop
    after_0:
    // 0x8080004C: jalr        $v0
    // 0x80800050: nop
    LOOKUP_FUNC(ctx->r2)(rdram, ctx);
''')
        self.assertEqual(counts['observed_direct_lookup'], 1)
        self.assertEqual(counts['observed_indirect_jalr'], 1)
        self.assertIn('tooie_overlay_callsite_push(0x8080004Cu);', text)

    def test_native_lookup_sees_immediate_site_and_restores_outer(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('c++ required for native observation fixture')
        text, _ = rewrite('''    // 0x8080004C: jalr        $v0
    // 0x80800050: addiu       $a0, $zero, 0x7
    ctx->r4 = 7;
    LOOKUP_FUNC(ctx->r2)(rdram, ctx);
    return;
''')
        caller = text[text.index('RECOMP_FUNC void caller'):]
        program = '''#include <cstdint>
#include <vector>
#include <stdexcept>
#define RECOMP_FUNC
struct recomp_context { uint64_t r2=0x80088540, r4=0, r31=0x800329A8; };
std::vector<uint32_t> sites;
void check(bool ok) { if (!ok) throw std::runtime_error("fixture mismatch"); }
void tooie_overlay_callsite_push(uint32_t pc) { sites.push_back(pc); }
void tooie_overlay_callsite_pop() { check(!sites.empty()); sites.pop_back(); }
void tooie_continuous_poll(uint8_t*,recomp_context*,uint32_t) {}
void callee(uint8_t* memory,recomp_context* ctx) {
    check(sites.size()==2 && sites.back()==0x8080004C && ctx->r4==7);
    check(ctx->r31==0x800329A8 && memory[0]==42);
    ctx->r2=99;
}
bool fail_lookup=false;
using Function=void(*)(uint8_t*,recomp_context*);
Function lookup(uint64_t address) {
    check(address==0x80088540 && sites.back()==0x8080004C);
    if(fail_lookup) throw std::runtime_error("injected missing target");
    return callee;
}
#define LOOKUP_FUNC lookup
''' + caller + '''
int main() {
    uint8_t memory[]={42}; recomp_context ctx;
    tooie_overlay_callsite_push(0x800A7C28);
    caller(memory,&ctx);
    check(sites.size()==1 && sites.back()==0x800A7C28 && ctx.r2==99 && memory[0]==42 && ctx.r31==0x800329A8);
    ctx.r2=0x80088540; fail_lookup=true;
    try { caller(memory,&ctx); return 2; }
    catch(const std::runtime_error&) { check(sites.back()==0x8080004C); }
}
'''
        with tempfile.TemporaryDirectory() as folder:
            source=Path(folder)/'fixture.cpp';exe=Path(folder)/'fixture'
            source.write_text(program)
            compiled=subprocess.run([compiler,'-std=c++20',str(source),'-o',str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            executed=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
            self.assertEqual(executed.returncode,0,executed.stderr)

    def test_preserves_existing_loader_and_poll_hooks(self):
        text, counts = rewrite('''L_80800020:
    // 0x80800020: jr          $ra
    // 0x80800024: nop
    return;
''')
        self.assertEqual(counts['scoped_uncached_reads'], 2)
        self.assertEqual(counts['overlay_hooks'], {'capture':1,'publish':1,'free':1,'pi_status':3,'rom_key':1})
        self.assertIn('tooie_continuous_poll(rdram, ctx, 0x80800020u);', text)


if __name__ == '__main__':
    unittest.main()
