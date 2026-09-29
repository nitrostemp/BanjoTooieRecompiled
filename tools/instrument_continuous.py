"""Apply bounded application adaptations to generated code.

Add cooperative cancellation, observations and native hardware reads. Four
source-verified saved-RA returns use the host call stack, like ordinary JR RA.
Their guest register/memory operations and delay slots remain unchanged. This
is an application post-generation pass, not a change to N64Recomp or the ROM.
"""
import hashlib
import re
from pathlib import Path

_GPRS = 'zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra'.split()

# Original core1/1E2B200.s: these four wrappers save their caller's RA in A1/A3,
# call the named math helper (which preserves that register), and JR the saved
# register. N64Recomp represents JAL/JR RA with the host stack and deliberately
# does not update guest RA at each call; generic LOOKUP_FUNC for these returns
# therefore looks up the thread's initial RA instead of returning to the caller.
# Pin complete generator bodies, including every instruction comment, operation
# and delay slot, plus the complete helpers. Do not infer this from arbitrary JR.
_SAVED_RA_BODIES = {
    'func_800137D4': 'fbd2ab949e373ef26e5d05e7feec38876791afd98c33966cdc96bfbb7be1620d',
    'func_800137F4': '2b5525c9dd9718130b8890b8d3e2a05cf4d34ab188689f8cd1a6d3800083f03f',
    'func_80013818': 'ca604a315bb5b950d808cd9dad3bcef017a4d667e89491968324ecc06b20f058',
    'func_80013A5C': '1fda180ae555ce61b5238de304182107a593b0260bc67e2dce94219684ea8094',
    'func_80013A7C': '2113995de5dbd5633a5774daec544544ef7b2a84ce18d6d7d99ae25dff4a632e',
    'func_80013ABC': 'abd1775bcea6ce1b8f882cb87ef4b29637401e0a8dc9ff9615eb6587fe1a0416',
}
_SAVED_RA_RETURNS = {
    'func_800137D4': (0x800137E0, 0x800137EC, 0x800137F0, 5, 'mov.s', '$f0, $f2'),
    'func_800137F4': (0x80013804, 0x80013810, 0x80013814, 5, 'mov.s', '$f0, $f2'),
    'func_80013A5C': (0x80013A60, 0x80013A74, 0x80013A78, 7, 'sub.s', '$f0, $f2, $f0'),
    'func_80013A7C': (0x80013A84, 0x80013A98, 0x80013A9C, 7, 'sub.s', '$f0, $f2, $f0'),
}


def correct_saved_ra_returns(text, source='<generated>'):
    """Correct only four exact original saved-register returns, before hooks.

    Bodies/helpers must all be present and byte-identical to the verified raw
    generator output. This rejects changed math, helper register use, delay-slot
    semantics, or already instrumented input instead of guessing a new idiom.
    """
    found = {}
    for match in re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n', text, re.S|re.M):
        if match[1] in _SAVED_RA_BODIES:
            if match[1] in found:
                raise ValueError(f'{source}: duplicate saved-RA family function {match[1]}')
            found[match[1]] = match[0]
    if not found:
        return text, []
    if set(found) != set(_SAVED_RA_BODIES):
        raise ValueError(f'{source}: incomplete saved-RA wrapper/helper family: {sorted(found)}')
    for name, body in found.items():
        if hashlib.sha256(body.encode()).hexdigest() != _SAVED_RA_BODIES[name]:
            raise ValueError(f'{source}: changed original instructions/generated body for {name}')
    corrected = []
    for name, (save_pc, jr_pc, delay_pc, register, delay_op, delay_args) in _SAVED_RA_RETURNS.items():
        body = found[name]
        instructions = [(int(pc,16),op,args.strip()) for pc,op,args in re.findall(
            r'^\s*// 0x([0-9A-Fa-f]+):\s+([\w.]+)\s*(.*)$',body,re.M)]
        required = [(save_pc,'or',f'${_GPRS[register]}, $ra, $zero'),
                    (jr_pc,'jr',f'${_GPRS[register]}'),(delay_pc,delay_op,delay_args)]
        if any(item not in instructions for item in required):
            raise ValueError(f'{source}: original saved-RA instruction mismatch for {name}')
        call = f'    LOOKUP_FUNC(ctx->r{register})(rdram, ctx);\n'
        if body.count(call) != 1 or body.count('    return;\n') != 1:
            raise ValueError(f'{source}: saved-RA return shape mismatch for {name}')
        replacement = body.replace(call, f'    // Original JR ${_GPRS[register]} at 0x{jr_pc:08X} returns to this host caller.\n')
        text = text.replace(body,replacement,1)
        corrected.append(name)
    return text, corrected


def callsite_observations(lines, stubs, source='<generated>'):
    """Map executable calls to their own instruction plus delay-slot comments.

    This only reads generated text. A previous JAL is never used as a fallback.
    JR returns and jump-table switches have no executable lookup call and are
    deliberately excluded. Pin the LOOKUP_FUNC register to the original operand.
    """
    observations = {}
    recent = []
    function = ''
    for index, line in enumerate(lines):
        start = re.match(r'RECOMP_FUNC void (\w+)\(', line)
        if start:
            function = start[1]
            recent = []
        instruction = re.match(r'\s*// 0x([0-9A-Fa-f]+):\s+(\w+)\s*(.*)', line)
        if instruction:
            recent = (recent + [(int(instruction[1],16), instruction[2], instruction[3].strip())])[-2:]
        direct = re.fullmatch(r'\s*(\w+)\(rdram, ctx\);\s*', line)
        lookup = re.fullmatch(r'\s*LOOKUP_FUNC\((ctx->r\d+|0x[0-9A-Fa-f]+)\)\(rdram, ctx\);\s*', line)
        if 'LOOKUP_FUNC' in line and not line.lstrip().startswith('//'):
            if not lookup:
                raise ValueError(f'{source}:{index+1} {function}: unmatched executable lookup: {line.strip()}')
        if lookup or (direct and direct[1] in stubs):
            where = f'{source}:{index+1} {function}'
            if len(recent) != 2 or recent[1][0] != recent[0][0] + 4:
                raise ValueError(f'{where}: call lacks adjacent original instruction/delay-slot PCs: {recent}')
            pc, opcode, operand = recent[0]
            if lookup and lookup[1].startswith('ctx->r'):
                register = int(lookup[1][6:])
                original_register = operand.split(',')[-1].strip().removeprefix('$')
                if opcode not in ('jalr','jr') or not 0 <= register < 32 or original_register != _GPRS[register] or (opcode == 'jr' and register == 31):
                    raise ValueError(f'{where}: lookup register/control transfer mismatch: {recent[0]}, {lookup[1]}')
                kind = 'indirect_jalr' if opcode == 'jalr' else 'indirect_tail_jr'
            else:
                if opcode not in ('jal','j','bal','b'):
                    raise ValueError(f'{where}: named/constant call lacks a direct transfer: {recent[0]}')
                target = re.fullmatch(r'(?:0x|L_)([0-9A-Fa-f]+)', operand)
                expected_target = stubs[direct[1]] if direct else int(lookup[1],16)
                if not target or int(target[1],16) != expected_target:
                    raise ValueError(f'{where}: generated call target disagrees with original transfer: {recent[0]}')
                kind = 'direct_stub' if direct else 'direct_lookup'
            observations[index] = (pc, kind)
        # Even an unobserved ordinary generated call consumes its origin.
        # Authored tooie_* helpers may sit between a generated transfer's two
        # comments and its generated C call, so they preserve that provenance.
        # Labels/control boundaries still prevent inheritance by a later call.
        if ((direct and not direct[1].startswith('tooie_')) or lookup or
                re.match(r'^\s*(?:L_[0-9A-Fa-f]+:|after_\d+:|return;|;}|goto\s)', line)):
            recent = []
    return observations


def observe_global_settings(text, source='<generated>'):
    """Read-only hooks scoped to original overlay699 function/call/delay pairs.

    Run before callsite instrumentation. No bare overlay-PC matching: other
    overlays share these virtual PCs. The five added calls never replace an
    original operation; removing those lines recovers the exact input bodies.
    """
    targets = {
        'func_80800168_glglobalsettings': [
            ('    // 0x808001A8: jal         0x808000A4\n'
             '    // 0x808001AC: or          $a1, $s1, $zero\n'
             '    ctx->r5 = ctx->r17 | 0;\n',
             '    func_808000A4_glglobalsettings(rdram, ctx);\n', None, 0x808001B0),
        ],
        'glglobalsettings_entrypoint_1': [
            ('    // 0x8080022C: jal         0x80800168\n'
             '    // 0x80800230: addiu       $a0, $sp, 0x18\n'
             '    ctx->r4 = ADD32(ctx->r29, 0X18);\n',
             '    func_80800168_glglobalsettings(rdram, ctx);\n', None, 0x80800234),
            ('    // 0x80800240: jal         0x80800000\n'
             '    // 0x80800244: nop\n\n',
             '    glglobalsettings_entrypoint_0(rdram, ctx);\n', 0x80800240, None),
            ('    // 0x80800250: jal         0x800885B8\n'
             '    // 0x80800254: lw          $a0, 0x18($sp)\n'
             '    ctx->r4 = MEM_W(ctx->r29, 0X18);\n',
             '    _glglobaldata_entrypoint_2(rdram, ctx);\n', 0x80800250, 0x80800258),
        ],
    }
    found = {}
    for match in re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n',text,re.S|re.M):
        if match[1] in targets:
            if match[1] in found:raise ValueError(f'{source}: duplicate global settings function {match[1]}')
            found[match[1]]=match[0]
    count=0
    for name,original in found.items():
        if 'tooie_observe_global_settings' in original:
            raise ValueError(f'{source}: already instrumented global settings function {name}')
        body=original
        for prefix,call,before,after in targets[name]:
            anchor=prefix+call
            if body.count(anchor)!=1:
                raise ValueError(f'{source}: changed original global settings call/delay anchor in {name}: {call.strip()}')
            replacement=prefix
            if before is not None:replacement+=f'    tooie_observe_global_settings(ctx, 0x{before:08X}u);\n';count+=1
            replacement+=call
            if after is not None:replacement+=f'    tooie_observe_global_settings(ctx, 0x{after:08X}u);\n';count+=1
            body=body.replace(anchor,replacement,1)
        text=text.replace(original,body,1)
    return text,count


def apply_widescreen_profile(text, source='<generated>'):
    """Override only overlay699's original stored-flag import value.

    The subsequent original entrypoint still calls set_widescreen and the
    game-owned GFLAG_BB9 setter. Pin the producer/call pair so regeneration
    rejects source or generator drift instead of patching a similarly shaped
    overlay call.
    """
    matches=list(re.finditer(r'RECOMP_FUNC void func_80800034_glglobalsettings\([^\n]*\n.*?^;}\n',text,re.S|re.M))
    if not matches:return text,0
    if len(matches)!=1:raise ValueError(f'{source}: duplicate global-settings import function')
    body=matches[0][0]
    if 'tooie_widescreen_apply_profile' in body:
        raise ValueError(f'{source}: widescreen profile hook already present')
    anchor=('    // 0x80800044: jal         0x808003C8\n'
            '    // 0x80800048: or          $a0, $v0, $zero\n'
            '    ctx->r4 = ctx->r2 | 0;\n'
            '    glglobalsettings_entrypoint_4(rdram, ctx);\n')
    if body.count(anchor)!=1:
        raise ValueError(f'{source}: changed global-settings widescreen import anchor')
    replacement=anchor.replace('    glglobalsettings_entrypoint_4(rdram, ctx);\n',
        '    tooie_widescreen_apply_profile(ctx);\n'
        '    glglobalsettings_entrypoint_4(rdram, ctx);\n')
    return text.replace(body,body.replace(anchor,replacement,1),1),1


def observe_graphics(text, source='<generated>'):
    """Five const observations, scoped to original core1 call+delay pairs."""
    targets={
        'func_80014A88':(
            '    // 0x80014AA4: jal         0x8002E1F0\n'
            '    // 0x80014AA8: addiu       $a2, $zero, 0x1\n'
            '    ctx->r6 = ADD32(0, 0X1);\n',
            '    osRecvMesg_recomp(rdram, ctx);\n',0x80014AAC,True),
        'func_80013E34':(
            '    // 0x80013ECC: jal         0x8002F1F4\n'
            '    // 0x80013ED0: or          $a0, $s0, $zero\n'
            '    ctx->r4 = ctx->r16 | 0;\n',
            '    osSpTaskStartGo_recomp(rdram, ctx);\n',0x80013ECC,False),
        'func_80013EF0':(
            '    // 0x80013F7C: jal         0x8002F1F4\n'
            '    // 0x80013F80: or          $a0, $s0, $zero\n'
            '    ctx->r4 = ctx->r16 | 0;\n',
            '    osSpTaskStartGo_recomp(rdram, ctx);\n',0x80013F7C,False),
        'func_80013FE4':(
            '    // 0x80014070: jal         0x8002F1F4\n'
            '    // 0x80014074: or          $a0, $s0, $zero\n'
            '    ctx->r4 = ctx->r16 | 0;\n',
            '    osSpTaskStartGo_recomp(rdram, ctx);\n',0x80014070,False),
        'func_800146D8':(
            '    // 0x800147A8: jal         0x8002F1F4\n'
            '    // 0x800147AC: addiu       $a0, $a0, -0xC10\n'
            '    ctx->r4 = ADD32(ctx->r4, -0XC10);\n',
            '    osSpTaskStartGo_recomp(rdram, ctx);\n',0x800147A8,False),
    }
    found={}
    for match in re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n',text,re.S|re.M):
        if match[1] in targets:
            if match[1] in found:raise ValueError(f'{source}: duplicate graphics observation function {match[1]}')
            found[match[1]]=match[0]
    for name,body in found.items():
        if 'tooie_observe_graphics' in body:raise ValueError(f'{source}: graphics function already instrumented: {name}')
        prefix,call,pc,after=targets[name];anchor=prefix+call
        if body.count(anchor)!=1:raise ValueError(f'{source}: changed original graphics call/delay anchor in {name}')
        hook=f'    tooie_observe_graphics(rdram, ctx, 0x{pc:08X}u);\n'
        replacement=prefix+(call+hook if after else hook+call)
        text=text.replace(body,body.replace(anchor,replacement,1),1)
    return text,len(found)


def observe_title(text, source='<generated>'):
    """Nine bounded title/attract timing observations at original instruction anchors."""
    targets={
        'func_808005AC_chintroticker':[
            ('    // 0x80800614: lbu         $v0, 0x79($s3)\n    ctx->r2 = MEM_BU(ctx->r19, 0X79);\n',0x80800614,'before'),
            ('    // 0x80800644: beq         $v0, $zero, L_80800694\n    if (ctx->r2 == 0) {\n',0x80800644,'before'),
            ('    // 0x80800654: bne         $v0, $zero, L_80800694\n    if (ctx->r2 != 0) {\n',0x80800654,'before'),
            ('    // 0x80800664: bne         $v0, $s0, L_80800694\n    if (ctx->r2 != ctx->r16) {\n',0x80800664,'before'),
            ('    // 0x8080066C: jal         0x80088180\n    // 0x80800670: nop\n\n    _gcfrontend_entrypoint_10(rdram, ctx);\n',0x8080066C,'call'),
            ('    // 0x808006C4: swc1        $f10, 0x2C($s3)\n    MEM_W(0X2C, ctx->r19) = ctx->f10.u32l;\n',0x808006C4,'after')],
        'func_80800738_gcfrontend':[
            ('    // 0x808007AC: jal         0x800A791C\n    // 0x808007B0: addiu       $a2, $zero, 0x1\n    ctx->r6 = ADD32(0, 0X1);\n    func_800A791C(rdram, ctx);\n',0x808007AC,'call')],
        'gsworldDll_entrypoint_2':[
            ('    // 0x808002C4: sh          $a1, 0x2($a0)\n    MEM_H(0X2, ctx->r4) = ctx->r5;\n',0x808002C4,'after')],
        'gsattract_entrypoint_1':[
            ('    // 0x80800050: swc1        $f8, 0x4($a1)\n    MEM_W(0X4, ctx->r5) = ctx->f8.u32l;\n',0x80800050,'after')],
    }
    found={};count=0
    for match in re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n',text,re.S|re.M):
        if match[1] in targets:
            if match[1] in found:raise ValueError(f'{source}: duplicate title observation function {match[1]}')
            found[match[1]]=match[0]
    for name,original in found.items():
        if 'tooie_observe_title' in original:raise ValueError(f'{source}: title function already instrumented: {name}')
        body=original
        if name=='func_808005AC_chintroticker':
            for guard in [
                '    // 0x808005C4: jal         0x800EA05C\n    // 0x808005C8: sw          $s0, 0x14($sp)\n    MEM_W(0X14, ctx->r29) = ctx->r16;\n    func_800EA05C(rdram, ctx);\n',
                '    // 0x8080063C: jal         0x800DA298\n    // 0x80800640: addiu       $a0, $zero, 0xD56\n    ctx->r4 = ADD32(0, 0XD56);\n    func_800DA298(rdram, ctx);\n',
                '    // 0x8080064C: jal         0x800A8264\n    // 0x80800650: nop\n\n    func_800A8264(rdram, ctx);\n',
                '    // 0x8080065C: jal         0x80015FA0\n    // 0x80800660: or          $a0, $zero, $zero\n    ctx->r4 = 0 | 0;\n    func_80015FA0(rdram, ctx);\n']:
                if body.count(guard)!=1:raise ValueError(f'{source}: changed title getter call/delay anchor in {name}')
        for anchor,pc,where in targets[name]:
            if body.count(anchor)!=1:raise ValueError(f'{source}: changed original title anchor in {name} at {pc:08X}')
            hook=f'    tooie_observe_title(rdram, ctx, 0x{pc:08X}u);\n'
            if where=='before':replacement=hook+anchor
            elif where=='after':replacement=anchor+hook
            else:
                lines=anchor.splitlines(keepends=True);replacement=''.join(lines[:-1])+hook+lines[-1]
            body=body.replace(anchor,replacement,1);count+=1
        text=text.replace(original,body,1)
    return text,count


def observe_map_actor_list(text, source='<generated>'):
    """One const snapshot after sort/before descriptor iteration; pinned body."""
    matches=list(re.finditer(r'RECOMP_FUNC void gspropsDll_entrypoint_1\([^\n]*\n.*?^;}\n',text,re.S|re.M))
    if not matches:return text,0
    if len(matches)!=1:raise ValueError(f'{source}: duplicate map actor list function')
    body=matches[0][0]
    # Includes all register producers, sorting, loop branches, calls and delay
    # slots. Fail closed on any generator/source drift, not merely a PC match.
    if hashlib.sha256(body.encode()).hexdigest()!='5ef5d033b99e044c71923141ec0dcece947cb97b782a5b8e77f5b2c169badea4':
        raise ValueError(f'{source}: changed original map actor list body')
    anchor='L_808001E0:\n    // 0x808001E0: blez        $s4, L_80800284\n'
    if body.count(anchor)!=1:raise ValueError(f'{source}: missing map actor iteration anchor')
    updated=body.replace(anchor,'L_808001E0:\n    tooie_observe_map_actor_list(rdram, ctx, 0x808001E0u);\n'+anchor.split('\n',1)[1],1)
    return text.replace(body,updated,1),1


def apply_save_progress_pause_hook(text, source='<generated>'):
    """Add one pause-only call to the original pause-menu update entry.

    The native helper only consumes a short-lived frontend request after it
    rechecks the original PauseState and save-manager admission fields. This
    transformation is intentionally scoped to the unique update entry rather
    than broad ``continuous_poll`` locations, which also run at title, overlay
    and worker contexts.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void gcnewpause_entrypoint_2\(uint8_t\* rdram, recomp_context\* ctx\) \{\n.*?^;}\n',
        text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate gcnewpause update entry')
    body = matches[0][0]
    if 'tooie_save_progress_pause_tick' in body:
        raise ValueError(f'{source}: save-progress pause hook already present')
    anchor = ('RECOMP_FUNC void gcnewpause_entrypoint_2(uint8_t* rdram, recomp_context* ctx) {\n'
              '    uint64_t hi = 0, lo = 0, result = 0;\n')
    if body.count(anchor) != 1:
        raise ValueError(f'{source}: changed gcnewpause update-entry anchor')
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected gcnewpause include layout')
    text = text.replace(include, include +
        'extern void tooie_save_progress_pause_tick(uint8_t*, recomp_context*, uint32_t);\n', 1)
    hook = ('RECOMP_FUNC void gcnewpause_entrypoint_2(uint8_t* rdram, recomp_context* ctx) {\n'
            '    tooie_save_progress_pause_tick(rdram, ctx, (uint32_t)ctx->r4);\n'
            '    uint64_t hi = 0, lo = 0, result = 0;\n')
    return text.replace(body, body.replace(anchor, hook, 1), 1), 1


def apply_game_feature_hooks(text, source='<generated>'):
    """Pin guest-thread hooks to original input and cutscene state entries."""
    targets = {
        'bainput_update': ('    tooie_cheats_tick(rdram, ctx);\n'
                           '    tooie_minimap_tick(rdram, ctx);\n'),
        'func_80800018_glcutDll':
            '    tooie_cutscene_state_command(rdram, ctx, (uint32_t)ctx->r4);\n',
    }
    count = 0
    declarations = []
    for match in list(re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n', text, re.S | re.M)):
        name, body = match[1], match[0]
        if name not in targets:
            continue
        hook = targets[name]
        if hook in body:
            raise ValueError(f'{source}: game feature hook already present: {name}')
        anchor = ('    uint64_t hi = 0, lo = 0, result = 0;\n'
                  '    int c1cs = 0;\n')
        if body.count(anchor) != 1:
            raise ValueError(f'{source}: changed game feature entry anchor: {name}')
        text = text.replace(body, body.replace(anchor, anchor + hook, 1), 1)
        declarations.append(
            ('extern void tooie_cheats_tick(uint8_t*, recomp_context*);\n'
             'extern void tooie_minimap_tick(uint8_t*, recomp_context*);\n')
            if name == 'bainput_update' else
            'extern void tooie_cutscene_state_command(uint8_t*, recomp_context*, uint32_t);\n')
        count += 1
    if declarations:
        include = '#include "funcs.h"\n'
        if text.count(include) != 1:
            raise ValueError(f'{source}: unexpected generated include layout for game features')
        text = text.replace(include, include + ''.join(declarations), 1)
    text, followup_count = apply_game_feature_followup_hooks(text, source)
    return text, count + followup_count


def apply_practice_form_hooks(text, source='<generated>'):
    """Scope form overrides to the original saved-record and basetup selectors.

    These run only during an explicitly reserved same-map Practice reload. The
    original guest functions still own model, behavior, and scene setup.
    """
    hooks = {
        'func_800F8B94': [(
            '    // 0x800F8C20: lbu         $a1, 0x0($s0)\n'
            '    ctx->r5 = MEM_BU(ctx->r16, 0X0);\n'
            '    func_800F7E64(rdram, ctx);\n',
            '    // 0x800F8C20: lbu         $a1, 0x0($s0)\n'
            '    ctx->r5 = MEM_BU(ctx->r16, 0X0);\n'
            '    tooie_practice_form_restore_override(rdram, ctx);\n'
            '    func_800F7E64(rdram, ctx);\n',
            'extern void tooie_practice_form_restore_override(uint8_t*, recomp_context*);\n')],
        'basetup_entrypoint_2': [(
            '    after_9:\n'
            '    // 0x80800908: addiu       $at, $zero, 0x9\n',
            '    after_9:\n'
            '    tooie_practice_form_basetup_override(rdram, ctx);\n'
            '    // 0x80800908: addiu       $at, $zero, 0x9\n',
            'extern void tooie_practice_form_basetup_override(uint8_t*, recomp_context*);\n'),
            ('L_80800980:\n'
             '    // 0x80800980: or          $a0, $s0, $zero\n',
             'L_80800980:\n'
             '    tooie_practice_form_basetup_final_override(rdram, ctx);\n'
             '    // 0x80800980: or          $a0, $s0, $zero\n',
             'extern void tooie_practice_form_basetup_final_override(uint8_t*, recomp_context*);\n')],
    }
    count = 0
    declarations = []
    for match in list(re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n', text, re.S | re.M)):
        name, body = match[1], match[0]
        if name not in hooks:
            continue
        if 'tooie_practice_form_' in body:
            raise ValueError(f'{source}: practice form hook already present: {name}')
        updated = body
        for before, after, declaration in hooks[name]:
            if updated.count(before) != 1:
                raise ValueError(f'{source}: changed practice form selector anchor: {name}')
            updated = updated.replace(before, after, 1)
            declarations.append(declaration)
            count += 1
        text = text.replace(body, updated, 1)
    if declarations:
        include = '#include "funcs.h"\n'
        if text.count(include) != 1:
            raise ValueError(f'{source}: unexpected generated include layout for practice forms')
        text = text.replace(include, include + ''.join(declarations), 1)
    return text, count


def apply_game_feature_followup_hooks(text, source='<generated>', *, lifecycle=True, restore=True, camera=True):
    """Pin save-generation invalidation and horizontal analog camera hooks."""
    count = 0
    declarations = set()
    lifecycle_names = []
    if lifecycle:
        lifecycle_names.extend(('func_800DA0B4', 'func_800DA188'))
    if restore:
        lifecycle_names.append('func_800DAC10')
    lifecycle_pattern = '|'.join(lifecycle_names)
    lifecycle_matches = list(re.finditer(
        rf'RECOMP_FUNC void ({lifecycle_pattern})\([^\n]*\n.*?^;}}\n', text, re.S | re.M)) if lifecycle_names else []
    for match in lifecycle_matches:
        name, body = match[1], match[0]
        hook = '    tooie_cheats_invalidate();\n'
        if hook in body:
            raise ValueError(f'{source}: cheat lifecycle hook already present: {name}')
        anchor = ('    uint64_t hi = 0, lo = 0, result = 0;\n'
                  '    int c1cs = 0;\n')
        if body.count(anchor) != 1:
            raise ValueError(f'{source}: changed cheat lifecycle entry anchor: {name}')
        text = text.replace(body, body.replace(anchor, anchor + hook, 1), 1)
        declarations.add('extern void tooie_cheats_invalidate(void);\n')
        count += 1

    matches = list(re.finditer(
        r'RECOMP_FUNC void func_800A4878\([^\n]*\n.*?^;}\n', text, re.S | re.M)) if camera else []
    if len(matches) > 1:
        raise ValueError(f'{source}: duplicate horizontal camera integration function')
    if matches:
        body = matches[0][0]
        hook = ('    const int tooie_camera_result = (ctx->r2 == 0)\n'
                '        ? tooie_camera_analog_apply(rdram, ctx) : -1;\n'
                '    if (tooie_camera_result >= 0) {\n'
                '        ctx->r2 = tooie_camera_result;\n'
                '        goto L_800A4964;\n'
                '    }\n')
        if 'tooie_camera_analog_apply(rdram, ctx)' in body:
            raise ValueError(f'{source}: horizontal camera hook already present')
        anchor = ('    after_5:\n'
                  '    // 0x800A4908: beq         $v0, $zero, L_800A4920\n'
                  '    if (ctx->r2 == 0) {\n')
        if body.count(anchor) != 1:
            raise ValueError(f'{source}: changed horizontal camera call/delay anchor')
        replacement = anchor.replace(
            '    // 0x800A4908: beq         $v0, $zero, L_800A4920\n',
            hook + '    // 0x800A4908: beq         $v0, $zero, L_800A4920\n')
        text = text.replace(body, body.replace(anchor, replacement, 1), 1)
        declarations.add('extern int tooie_camera_analog_apply(uint8_t*, recomp_context*);\n')
        count += 1

    declarations = {declaration for declaration in declarations if declaration not in text}
    if declarations:
        include = '#include "funcs.h"\n'
        if text.count(include) != 1:
            raise ValueError(f'{source}: unexpected generated include layout for feature followups')
        text = text.replace(include, include + ''.join(sorted(declarations)), 1)
    return text, count


def apply_first_person_analog_hooks(text, source='<generated>'):
    """Route right-stick axes through Tooie's original first-person math.

    func_808002A4_bafpctrl admits only the active ncba1p controller state,
    applies the game's sensitivity and delta, clamps pitch, and writes the
    resulting aim vector. Replace only the two original bastick return values;
    zero/disabled right axes retain the original left-stick values.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_808002A4_bafpctrl\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate first-person camera controller')
    body = matches[0][0]
    if 'tooie_camera_first_person_axis' in body:
        raise ValueError(f'{source}: first-person analog hooks already present')
    anchors = (
        ('    after_7:\n'
         '    // 0x80800320: lwc1        $f8, 0x2C($sp)\n',
         '    after_7:\n'
         '    ctx->f0.fl = tooie_camera_first_person_axis(ctx->f0.fl, 1);\n'
         '    // 0x80800320: lwc1        $f8, 0x2C($sp)\n'),
        ('    after_8:\n'
         '    // 0x80800344: lwc1        $f8, 0x2C($sp)\n',
         '    after_8:\n'
         '    ctx->f0.fl = tooie_camera_first_person_axis(ctx->f0.fl, 0);\n'
         '    // 0x80800344: lwc1        $f8, 0x2C($sp)\n'),
    )
    updated = body
    for anchor, replacement in anchors:
        if updated.count(anchor) != 1:
            raise ValueError(f'{source}: changed first-person stick return anchor')
        updated = updated.replace(anchor, replacement, 1)
    include = '#include "funcs.h"\n'
    declaration = 'extern float tooie_camera_first_person_axis(float, int);\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for first-person camera')
    text = text.replace(include, include + declaration, 1)
    return text.replace(body, updated, 1), 2


def apply_free_camera_hooks(text, source='<generated>'):
    """Temporarily translate Tooie's camera for only the active world draw.

    The begin/end pair brackets every draw call after the original camera prep.
    The native helper restores the exact position words at end, so camera and
    player simulation never inherit the detached rendering offset.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_800E9F20\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate active frame composer')
    body = matches[0][0]
    if 'tooie_free_camera_begin' in body or 'tooie_free_camera_end' in body:
        raise ValueError(f'{source}: free-camera hooks already present')
    raw_begin_anchor = ('L_800E9F5C:\n'
                        '    // 0x800E9F5C: jal         0x800BE9B0\n')
    polled_begin_anchor = ('L_800E9F5C:\n'
                           '    tooie_continuous_poll(rdram, ctx, 0x800E9F5Cu);\n'
                           '    // 0x800E9F5C: jal         0x800BE9B0\n')
    end_anchor = ('    after_24:\n'
                  '    // 0x800EA048: lw          $ra, 0x1C($sp)\n')
    begin_matches = body.count(raw_begin_anchor) + body.count(polled_begin_anchor)
    if begin_matches != 1 or body.count(end_anchor) != 1:
        raise ValueError(f'{source}: changed active frame begin/end anchors')
    begin_anchor = polled_begin_anchor if polled_begin_anchor in body else raw_begin_anchor
    updated = body.replace(begin_anchor,
        begin_anchor.replace('    // 0x800E9F5C: jal         0x800BE9B0\n',
                             '    tooie_free_camera_begin(rdram, ctx);\n'
                             '    // 0x800E9F5C: jal         0x800BE9B0\n'), 1)
    updated = updated.replace(end_anchor,
        '    after_24:\n    tooie_free_camera_end(rdram, ctx);\n'
        '    // 0x800EA048: lw          $ra, 0x1C($sp)\n', 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for free camera')
    text = text.replace(include, include +
        'extern void tooie_free_camera_begin(uint8_t*, recomp_context*);\n'
        'extern void tooie_free_camera_end(uint8_t*, recomp_context*);\n', 1)
    return text.replace(body, updated, 1), 2


def apply_camera_interpolation_hook(text, source='<generated>'):
    """Observe only Tooie's original active-camera selector.

    func_800E42B4 stores its argument in D_8012D500. Camera backup/restore uses
    separate routines without changing this handle, so the hook identifies a
    real projection-owner change without position thresholds or guessed modes.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_800E42B4\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate active-camera selector')
    body = matches[0][0]
    if 'tooie_camera_interpolation_observe_active' in body:
        raise ValueError(f'{source}: camera interpolation hook already present')
    anchor = ('    // 0x800E42BC: sw          $a0, -0x2B00($at)\n'
              '    MEM_W(-0X2B00, ctx->r1) = ctx->r4;\n'
              '    return;\n')
    if body.count(anchor) != 1:
        raise ValueError(f'{source}: changed active-camera store/return anchor')
    updated = body.replace(anchor,
        '    // 0x800E42BC: sw          $a0, -0x2B00($at)\n'
        '    MEM_W(-0X2B00, ctx->r1) = ctx->r4;\n'
        '    tooie_camera_interpolation_observe_active((uint32_t)ctx->r4);\n'
        '    return;\n', 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for camera interpolation')
    text = text.replace(include, include +
        'extern void tooie_camera_interpolation_observe_active(uint32_t);\n', 1)
    return text.replace(body, updated, 1), 1


def apply_model_interpolation_hooks(text, source='<generated>'):
    """Observe one original model draw and its actual CPU-skinning calls.

    Only func_800DE498 owns the complete GraphicsBuffers matrix allocation
    interval. The two dbanim calls are reached after both the model skinning
    section and resolved animation list were checked by original code.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_800DE498\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate model renderer')
    body = matches[0][0]
    if 'tooie_model_draw_' in body:
        raise ValueError(f'{source}: model interpolation hooks already present')
    entry = 'RECOMP_FUNC void func_800DE498(uint8_t* rdram, recomp_context* ctx) {\n'
    exit_label = 'L_800DF310:\n'
    if body.count(entry) != 1 or body.count(exit_label) != 1:
        raise ValueError(f'{source}: changed model renderer entry/exit')
    updated = body.replace(entry, entry +
        '    tooie_model_draw_begin(rdram, ctx);\n', 1)
    for pc, target, call in (
        (0x800DEEE4, 0x800879B8, '_dbanim_entrypoint_1'),
        (0x800DEEF8, 0x800879B0, '_dbanim_entrypoint_0'),
    ):
        anchor = (f'    // 0x{pc:08X}: jal         0x{target:08X}\n'
                  f'    // 0x{pc + 4:08X}: lw          $a1, 0x1C($s0)\n'
                  '    ctx->r5 = MEM_W(ctx->r16, 0X1C);\n'
                  f'    {call}(rdram, ctx);\n')
        if updated.count(anchor) != 1:
            raise ValueError(f'{source}: changed CPU-skinning call {pc:08X}')
        updated = updated.replace(anchor, anchor.replace(
            f'    {call}(rdram, ctx);\n',
            f'    tooie_model_draw_cpu_skinning();\n    {call}(rdram, ctx);\n'), 1)
    updated = updated.replace(exit_label, exit_label +
        '    tooie_model_draw_end(rdram, ctx);\n', 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for model interpolation')
    text = text.replace(include, include +
        'extern void tooie_model_draw_begin(uint8_t*, recomp_context*);\n'
        'extern void tooie_model_draw_cpu_skinning(void);\n'
        'extern void tooie_model_draw_end(uint8_t*, recomp_context*);\n', 1)
    return text.replace(body, updated, 1), 4


def apply_intro_model_draw_hooks(text, source='<generated>'):
    """Keep the measured title-character CPU poses on their paired guest camera.

    The title renderer can execute with an existing save slot, so a slot/level
    gameplay gate alone is insufficient to distinguish its camera-follow draws.
    Mark only its three direct func_800DE448 calls and restore the thread-local
    marker immediately afterward; the ordinary gameplay actor path is untouched.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_80803A78_chintrochar\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate title-character renderer')
    body = matches[0][0]
    updated = body
    for pc in (0x80803C20, 0x80803C48, 0x80803CD4):
        anchor = (f'    // 0x{pc:08X}: jal         0x800DE448\n')
        if updated.count(anchor) != 1:
            raise ValueError(f'{source}: changed title model call {pc:08X}')
        call_start = updated.index(anchor)
        call = '    func_800DE448(rdram, ctx);\n'
        call_pos = updated.find(call, call_start)
        if call_pos < 0 or call_pos - call_start > 250:
            raise ValueError(f'{source}: changed title model call body {pc:08X}')
        updated = (updated[:call_pos] +
            '    tooie_model_intro_draw_begin();\n' +
            '    func_800DE448(rdram, ctx);\n' +
            '    tooie_model_intro_draw_end();\n' +
            updated[call_pos + len(call):])
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for title model')
    text = text.replace(include, include +
        'extern void tooie_model_intro_draw_begin(void);\n'
        'extern void tooie_model_intro_draw_end(void);\n', 1)
    return text.replace(body, updated, 1), 6


def apply_scene_observer_hooks(text, source='<generated>'):
    """Observe Tooie's current map and bounded ordered activation calls."""
    count = 0
    declarations = []
    map_matches = list(re.finditer(
        r'RECOMP_FUNC void func_800EA05C\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if len(map_matches) > 1:
        raise ValueError(f'{source}: duplicate current-map getter')
    if map_matches:
        body = map_matches[0][0]
        if 'tooie_scene_observe_map' in body:
            raise ValueError(f'{source}: map observer already present')
        anchor = ('    // 0x800EA064: lhu         $v0, 0x2DC2($v0)\n'
                  '    ctx->r2 = MEM_HU(ctx->r2, 0X2DC2);\n'
                  '    return;\n')
        if body.count(anchor) != 1:
            raise ValueError(f'{source}: changed current-map load/return anchor')
        updated = body.replace(anchor,
            '    // 0x800EA064: lhu         $v0, 0x2DC2($v0)\n'
            '    ctx->r2 = MEM_HU(ctx->r2, 0X2DC2);\n'
            '    tooie_scene_observe_map((uint16_t)ctx->r2);\n'
            '    return;\n', 1)
        text = text.replace(body, updated, 1)
        declarations.append('extern void tooie_scene_observe_map(uint16_t);\n')
        count += 1

    activation_matches = list(re.finditer(
        r'RECOMP_FUNC void func_800A72A4\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if len(activation_matches) > 1:
        raise ValueError(f'{source}: duplicate scene-activation routine')
    if activation_matches:
        body = activation_matches[0][0]
        if 'tooie_scene_activation_started' in body or 'tooie_scene_activation_phase_started' in body:
            raise ValueError(f'{source}: scene activation observers already present')
        entry = 'RECOMP_FUNC void func_800A72A4(uint8_t* rdram, recomp_context* ctx) {\n'
        variables = '    uint64_t hi = 0, lo = 0, result = 0;\n'
        poll = '    tooie_continuous_poll(rdram, ctx, 0x800A72A4u);\n'
        start_variants = ((entry + variables,
                           entry + '    tooie_scene_activation_started();\n' + variables),
                          (entry + poll + variables,
                           entry + poll + '    tooie_scene_activation_started();\n' + variables))
        finish_anchor = ('    after_12:\n'
                         '    // 0x800A7370: lw          $ra, 0x14($sp)\n')
        phases = (
            (0, '    MEM_W(0X1C, ctx->r29) = ctx->r14;\n', '_gcsectionDll_entrypoint_4', 'after_0', 0x800A72C4),
            (1, '', '_gclevel_entrypoint_1', 'after_3', 0x800A7300),
            (2, '', '_gclevel_entrypoint_0', 'after_6', 0x800A7328),
            (3, '', '_gcsectionDll_entrypoint_0', 'after_7', 0x800A7334),
            (4, '    ctx->r6 = 0 | 0;\n', 'func_800EA0CC', 'after_11', None),
        )
        start_matches = [pair for pair in start_variants if body.count(pair[0]) == 1]
        if len(start_matches) != 1 or body.count(finish_anchor) != 1:
            raise ValueError(f'{source}: changed scene-activation boundary anchors')
        updated = body.replace(*start_matches[0], 1)
        for phase, prefix, callee, label, call_pc in phases:
            call = f'    {callee}(rdram, ctx);\n'
            suffix = f'        goto {label};\n'
            raw = prefix + call + suffix
            variants = [(raw, prefix +
                f'    tooie_scene_activation_phase_started({phase});\n' + call +
                f'    tooie_scene_activation_phase_finished({phase});\n' + suffix)]
            if call_pc is not None:
                push = f'    tooie_overlay_callsite_push(0x{call_pc:08X}u);\n'
                pop = '    tooie_overlay_callsite_pop();\n'
                observed = prefix + push + call + pop + suffix
                variants.append((observed, prefix +
                    f'    tooie_scene_activation_phase_started({phase});\n' + push + call + pop +
                    f'    tooie_scene_activation_phase_finished({phase});\n' + suffix))
            matches = [(anchor, replacement) for anchor, replacement in variants
                       if updated.count(anchor) == 1]
            if len(matches) != 1:
                raise ValueError(f'{source}: changed scene phase {phase} anchors')
            updated = updated.replace(*matches[0], 1)
        updated = updated.replace(finish_anchor,
            '    after_12:\n'
            '    tooie_scene_activation_finished();\n'
            '    // 0x800A7370: lw          $ra, 0x14($sp)\n', 1)
        text = text.replace(body, updated, 1)
        declarations.extend((
            'extern void tooie_scene_activation_phase_started(uint32_t);\n',
            'extern void tooie_scene_activation_phase_finished(uint32_t);\n',
            'extern void tooie_scene_activation_started(void);\n',
            'extern void tooie_scene_activation_finished(void);\n'))
        count += 12

    if declarations:
        include = '#include "funcs.h"\n'
        if text.count(include) != 1:
            raise ValueError(f'{source}: unexpected generated include layout for scene observer')
        text = text.replace(include, include + ''.join(declarations), 1)
    return text, count

def apply_gsworld_call_timing_hooks(text, source='<generated>'):
    """Time only the fixed original calls in world activation; retain no history."""
    matches = list(re.finditer(
        r'RECOMP_FUNC void gsworldDll_entrypoint_2\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate gsworld activation')
    body = matches[0][0]
    if 'tooie_scene_world_call_started' in body:
        raise ValueError(f'{source}: gsworld timings already present')
    lines = body.splitlines(keepends=True)
    count = 0
    for index in range(len(lines) - 1, -1, -1):
        pc_match = re.fullmatch(r'    // 0x([0-9A-F]+): jal .*\n', lines[index])
        if not pc_match:
            continue
        target = next((candidate for candidate in range(index + 1, min(index + 12, len(lines)))
                       if re.fullmatch(r'    \w+\(rdram, ctx\);\n', lines[candidate])), None)
        if target is None:
            raise ValueError(f'{source}: changed gsworld call anchor at {pc_match[1]}')
        pc = int(pc_match[1], 16)
        lines.insert(target, f'    tooie_scene_world_call_started(0x{pc:08X}u);\n')
        lines.insert(target + 2, f'    tooie_scene_world_call_finished(0x{pc:08X}u);\n')
        count += 1
    if count != 59:
        raise ValueError(f'{source}: expected 59 original gsworld calls, found {count}')
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for gsworld timing')
    declarations = ('extern void tooie_scene_world_call_started(uint32_t);\n'
                    'extern void tooie_scene_world_call_finished(uint32_t);\n')
    text = text.replace(include, include + declarations, 1)
    return text.replace(body, ''.join(lines), 1), count


def apply_scene_nested_call_timing_hooks(text, source='<generated>'):
    """Retain only each dominant world-setup parent's slowest direct child.

    These are fixed direct calls inside func_800A5C28 and func_800F73C4, not a
    general call profiler. The native observer discards all work outside an
    active scene activation and stores one winner per parent.
    """
    targets = {
        'func_800A5C28': (0x800A5C28, {
            0x800A5C30, 0x800A5C3C, 0x800A5C44, 0x800A5C4C, 0x800A5C54,
            0x800A5C5C, 0x800A5C64, 0x800A5C6C, 0x800A5C7C,
        }),
        'func_800F73C4': (0x800F73C4, {
            0x800F7424, 0x800F742C, 0x800F743C, 0x800F744C, 0x800F7464,
            0x800F7478, 0x800F7480, 0x800F7490, 0x800F74A4, 0x800F74C0,
            0x800F74CC, 0x800F74D4, 0x800F74E8, 0x800F74F4, 0x800F7510,
            0x800F7528, 0x800F7540, 0x800F754C, 0x800F7570,
        }),
    }
    count = 0
    processed = set()
    for match in list(re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n', text, re.S | re.M)):
        name, body = match[1], match[0]
        if name not in targets:
            continue
        processed.add(name)
        if 'tooie_scene_nested_call_started' in body:
            raise ValueError(f'{source}: nested scene timing already present: {name}')
        parent_pc, call_pcs = targets[name]
        lines = body.splitlines(keepends=True)
        local_count = 0
        for index in range(len(lines) - 1, -1, -1):
            pc_match = re.fullmatch(r'    // 0x([0-9A-F]+): jal .*\n', lines[index])
            if not pc_match:
                continue
            child_pc = int(pc_match[1], 16)
            if child_pc not in call_pcs:
                continue
            target = next((candidate for candidate in range(index + 1, min(index + 14, len(lines)))
                           if re.fullmatch(r'    \w+\(rdram, ctx\);\n', lines[candidate])), None)
            if target is None:
                raise ValueError(f'{source}: changed nested scene call anchor at {child_pc:08X}')
            lines.insert(target, f'    tooie_scene_nested_call_started(0x{parent_pc:08X}u, 0x{child_pc:08X}u);\n')
            lines.insert(target + 2, f'    tooie_scene_nested_call_finished(0x{parent_pc:08X}u, 0x{child_pc:08X}u);\n')
            local_count += 2
        if local_count != 2 * len(call_pcs):
            raise ValueError(f'{source}: expected {len(call_pcs)} nested scene calls in {name}, found {local_count // 2}')
        text = text.replace(body, ''.join(lines), 1)
        count += local_count
    expected = sum(len(targets[name][1]) for name in processed)
    if count and count != 2 * expected:
        raise ValueError(f'{source}: incomplete nested scene call coverage: {count // 2}/{expected}')
    if count:
        include = '#include "funcs.h"\n'
        if text.count(include) != 1:
            raise ValueError(f'{source}: unexpected generated include layout for nested scene timing')
        text = text.replace(include, include +
            'extern void tooie_scene_nested_call_started(uint32_t, uint32_t);\n'
            'extern void tooie_scene_nested_call_finished(uint32_t, uint32_t);\n', 1)
    return text, count


def apply_guest_update_observer(text, source='<generated>'):
    """Report the original guest-update entry and whether its wait was bypassed."""
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_800A73F4\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:return text,0
    if len(matches)!=1:raise ValueError(f'{source}: duplicate guest-update owner')
    body=matches[0][0]
    hook='    tooie_guest_update_observed(MEM_H(0X80127634, 0) != 0);\n'
    if hook in body:raise ValueError(f'{source}: guest-update observer already present')
    entry='RECOMP_FUNC void func_800A73F4(uint8_t* rdram, recomp_context* ctx) {\n'
    variables='    uint64_t hi = 0, lo = 0, result = 0;\n'
    poll='    tooie_continuous_poll(rdram, ctx, 0x800A73F4u);\n'
    variants=((entry+variables,entry+hook+variables),
              (entry+poll+variables,entry+poll+hook+variables))
    found=[pair for pair in variants if body.count(pair[0])==1]
    if len(found)!=1:raise ValueError(f'{source}: changed guest-update entry anchor')
    text=text.replace(body,body.replace(*found[0],1),1)
    declaration='extern void tooie_guest_update_observed(int wait_bypassed);\n'
    scene_tail='extern void tooie_scene_activation_finished(void);\n'
    if text.count(scene_tail)==1:
        text=text.replace(scene_tail,scene_tail+'\n'+declaration,1)
    else:
        include='#include "funcs.h"\n'
        if text.count(include)!=1:raise ValueError(f'{source}: unexpected generated include layout for guest update')
        text=text.replace(include,include+declaration,1)
    return text,1


def apply_heap_realloc_safety_fix(text, source='<generated>'):
    """Repair only the verified moving-growth over-copy and failed-pin paths.

    The original subtraction at 0x8001B220 is the physical old payload
    capacity. The allocation/copy path can request a larger aligned size, so
    the generated memcpy length must not exceed that original capacity. A
    failed allocation returns zero after the original pin setup; release that
    pin before preserving the zero result. The repair preserves allocation
    ordering and all shrink/in-place paths; it changes only the moving-copy
    bound on a successful growth path.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void heap_realloc\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate heap_realloc')
    body = matches[0][0]
    if 'tooie_heap_realloc_old_capacity' in body:
        raise ValueError(f'{source}: heap_realloc safety fix already present')
    locals_anchor = '    uint64_t hi = 0, lo = 0, result = 0;\n    int c1cs = 0;\n'
    capacity_anchor = ('    // 0x8001B220: subu        $t7, $t6, $a0\n'
                       '    ctx->r15 = SUB32(ctx->r14, ctx->r4);\n')
    failure_anchor = ('    // 0x8001B2FC: b           L_8001B374\n'
                      '    // 0x8001B300: or          $v0, $zero, $zero\n'
                      '    ctx->r2 = 0 | 0;\n'
                      '        goto L_8001B374;\n')
    copy_anchor = ('    // 0x8001B358: jal         0x80019E70\n'
                   '    // 0x8001B35C: or          $a2, $v0, $zero\n'
                   '    ctx->r6 = ctx->r2 | 0;\n'
                   '    aligned8_memcpy(rdram, ctx);\n')
    anchors = (locals_anchor, capacity_anchor, failure_anchor, copy_anchor)
    if any(body.count(anchor) != 1 for anchor in anchors):
        raise ValueError(f'{source}: changed heap_realloc safety anchor')
    updated = body.replace(locals_anchor, locals_anchor +
        '    uint32_t tooie_heap_realloc_old_capacity = 0;\n', 1)
    updated = updated.replace(capacity_anchor, capacity_anchor +
        '    tooie_heap_realloc_old_capacity = (uint32_t)ctx->r15;\n', 1)
    updated = updated.replace(failure_anchor,
        ('    // 0x8001B2FC: allocation failed; release the original pin before returning zero.\n'
         '    ctx->r4 = 0 | 0;\n'
         '    func_8001B864(rdram, ctx);\n' + failure_anchor), 1)
    updated = updated.replace(copy_anchor,
        ('    // 0x8001B358: jal         0x80019E70\n'
         '    // 0x8001B35C: or          $a2, $v0, $zero\n'
         '    ctx->r6 = ctx->r2 | 0;\n'
         '    ctx->r6 = ctx->r6 < tooie_heap_realloc_old_capacity ? ctx->r6 : tooie_heap_realloc_old_capacity;\n'
         '    aligned8_memcpy(rdram, ctx);\n'), 1)
    return text.replace(body, updated, 1), 3


def apply_game_delta_observer(text, source='<generated>'):
    """Observe the game-delta word after replay input may replace it.

    A7D30 supplies the ordinary accumulated-VI delta.  func_8001608C can then
    replace that same word with glrecord byte four before the update uses it.
    Sampling at the latter return makes health diagnostics describe the actual
    game delta without changing either source of timing.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_800123F4\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate game-delta owner')
    body = matches[0][0]
    if 'tooie_game_delta_observed' in body:
        raise ValueError(f'{source}: game-delta observer already present')
    anchor = ('    func_8001608C(rdram, ctx);\n'
              '        goto after_4;\n'
              '    // 0x8001245C: nop\n\n'
              '    after_4:\n')
    if body.count(anchor) != 1:
        raise ValueError(f'{source}: changed post-replay game-delta anchor')
    # MEM_* requires a sign-extended guest base. A positive 0x8012C760 offset
    # with zero base would address outside RDRAM on 64-bit hosts.
    updated = body.replace(anchor, anchor +
        '    tooie_game_delta_observed((uint32_t)MEM_W(0, (gpr)(int32_t)0x8012C760U));\n', 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for game delta')
    text = text.replace(include, include +
        'extern void tooie_game_delta_observed(uint32_t);\n', 1)
    return text.replace(body, updated, 1), 1


def apply_replay_timing_hooks(text, source='<generated>'):
    """Pace replay input records at their recorded retrace divisor only.

    func_8001608C first clears the published rate, then receives a record from glrecord, then D8FA0 consumes its
    byte-4 tick count.  The following frame's core1 wait loop is the original
    pacing owner.  Override only its two local divisor returns; never change
    the global getter or virtual VI production used by ordinary gameplay.
    """
    begin = '    tooie_replay_timing_begin_record();\n'
    record = ('    tooie_replay_timing_record_divisor('
              '(uint32_t)MEM_W(0X50, ctx->r29), ctx->r2 != 0);\n')
    entry = 'RECOMP_FUNC void func_8001608C(uint8_t* rdram, recomp_context* ctx) {\n'
    variables = '    uint64_t hi = 0, lo = 0, result = 0;\n'
    poll = '    tooie_continuous_poll(rdram, ctx, 0x8001608Cu);\n'
    raw_call = ('    _glrecord_entrypoint_0(rdram, ctx);\n'
                '        goto after_3;\n')
    observed_call = ('    _glrecord_entrypoint_0(rdram, ctx);\n'
                     '    tooie_overlay_callsite_pop();\n'
                     '        goto after_3;\n')
    targets = {
        'func_8001608C': (
            (((entry + variables, entry + begin + variables),
              (entry + poll + variables, entry + poll + begin + variables)),
             ((raw_call, raw_call.replace('        goto after_3;\n', record + '        goto after_3;\n')),
              (observed_call, observed_call.replace('        goto after_3;\n', record + '        goto after_3;\n')))),
            2),
        'func_800151BC': (
            (((('    after_4:\n'
                '    // 0x8001520C: lui         $t6, 0x8008\n'),
               ('    after_4:\n'
                '    ctx->r2 = (gpr)(int32_t)tooie_replay_timing_scheduler_divisor((uint32_t)ctx->r2, 1);\n'
                '    // 0x8001520C: lui         $t6, 0x8008\n')),),
             ((('    after_6:\n'
                '    // 0x8001524C: lui         $t0, 0x8008\n'),
               ('    after_6:\n'
                '    ctx->r2 = (gpr)(int32_t)tooie_replay_timing_scheduler_divisor((uint32_t)ctx->r2, 0);\n'
                '    // 0x8001524C: lui         $t0, 0x8008\n')),)),
            2),
    }
    count = 0
    declarations = []
    for match in list(re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n', text, re.S | re.M)):
        name, body = match[1], match[0]
        if name not in targets:
            continue
        if 'tooie_replay_timing_begin_record' in body or 'tooie_replay_timing_record_divisor' in body or 'tooie_replay_timing_scheduler_divisor' in body:
            raise ValueError(f'{source}: replay timing hook already present: {name}')
        anchor_groups, expected_count = targets[name]
        if expected_count != len(anchor_groups):
            raise AssertionError(f'{source}: malformed replay timing hook definition: {name}')
        updated = body
        for variants in anchor_groups:
            matches = [(anchor, replacement) for anchor, replacement in variants
                       if updated.count(anchor) == 1]
            if len(matches) != 1:
                raise ValueError(f'{source}: changed replay timing anchor: {name}')
            anchor, replacement = matches[0]
            updated = updated.replace(anchor, replacement, 1)
        text = text.replace(body, updated, 1)
        count += expected_count
        declarations.append(name)
    if declarations:
        include = '#include "funcs.h"\n'
        if text.count(include) != 1:
            raise ValueError(f'{source}: unexpected generated include layout for replay timing')
        hook_names = set(declarations)
        c_declarations = ''
        if 'func_8001608C' in hook_names:
            c_declarations += ('extern void tooie_replay_timing_begin_record(void);\n'
                               'extern void tooie_replay_timing_record_divisor(uint32_t, int);\n')
        if 'func_800151BC' in hook_names:
            c_declarations += 'extern uint32_t tooie_replay_timing_scheduler_divisor(uint32_t, int);\n'
        text = text.replace(include, include + c_declarations, 1)
    return text, count


def apply_native_aspect_hook(text, source='<generated>'):
    """Scale only func_80015D14's completed, game-owned widescreen aspect.

    r24 still contains the original signed widescreen flag loaded at 80015D28.
    Passing it to the host helper leaves original 4:3 and scripted 4:3 cutscene
    projection untouched. func_80015CE8 subsequently forwards f0 to CA510,
    the original camera projection/frustum rebuild owner.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_80015D14\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate viewport-aspect owner')
    body = matches[0][0]
    if 'tooie_widescreen_adjust_projection_aspect' in body:
        raise ValueError(f'{source}: native-aspect hook already present')
    raw_anchor = ('L_80015D4C:\n'
                  '    // 0x80015D4C: jr          $ra\n')
    polled_anchor = ('L_80015D4C:\n'
                     '    tooie_continuous_poll(rdram, ctx, 0x80015D4Cu);\n'
                     '    // 0x80015D4C: jr          $ra\n')
    anchors = [anchor for anchor in (raw_anchor, polled_anchor)
               if body.count(anchor) == 1]
    if len(anchors) != 1:
        raise ValueError(f'{source}: changed viewport-aspect return anchor')
    anchor = anchors[0]
    updated = body.replace(anchor,
        anchor.replace('    // 0x80015D4C: jr          $ra\n',
                       '    ctx->f2.fl = tooie_widescreen_adjust_projection_aspect(\n'
                       '        ctx->f2.fl, ctx->r24 != 0);\n'
                       '    // 0x80015D4C: jr          $ra\n'), 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for native aspect')
    text = text.replace(include, include +
        'extern float tooie_widescreen_adjust_projection_aspect(float, int);\n', 1)
    return text.replace(body, updated, 1), 1


def apply_actor_draw_distance_hooks(text, source='<generated>'):
    """Scale only the common actor model fade/cull distances.

    func_80101970 selects either the actor descriptor's +0x1A distance or the
    original map-derived fallback, then sends it through func_800DF428 to the
    model renderer. Camera planes, terrain, and special-camera paths are not
    part of these two pinned call sites.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_80101970\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate common actor setup owner')
    body = matches[0][0]
    if 'tooie_actor_draw_distance_adjust' in body:
        raise ValueError(f'{source}: actor-distance hooks already present')
    call = '    func_800DF428(rdram, ctx);\n'
    if body.count(call) != 2 or body.count('// 0x801019CC: jal         0x800DF428') != 1 or \
            body.count('// 0x80101A1C: jal         0x800DF428') != 1:
        raise ValueError(f'{source}: changed actor-distance owner/calls')
    updated = body.replace(call,
        '    ctx->f12.fl = tooie_actor_draw_distance_adjust(ctx->f12.fl);\n' + call)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for actor distance')
    text = text.replace(include, include +
        'extern float tooie_actor_draw_distance_adjust(float);\n', 1)
    return text.replace(body, updated, 1), 2


def apply_hud_counter_layout_hook(text, source='<generated>'):
    """Adjust source-identified ordinary counters at the scinfobar draw seam.

    func_800FA508 walks the 48 original 0x1c-byte slots. r16 is the current
    slot, r6/r7 are its animated horizontal/vertical origins, and D_8012B2C0
    retains the counter ID written by func_800D22E0. The helper changes r6
    only for the exact ordinary-counter allowlist in hud_layout.cpp.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_800FA508\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate scinfobar renderer')
    body = matches[0][0]
    if 'tooie_hud_counter_adjust' in body:
        raise ValueError(f'{source}: HUD counter-layout hook already present')
    anchor = ('    // 0x800FA5CC: jal         0x8008A640\n'
              '    // 0x800FA5D0: lh          $a3, 0x8($s0)\n'
              '    ctx->r7 = MEM_H(ctx->r16, 0X8);\n')
    if body.count(anchor) != 1 or body.count('// 0x800FA5C8: lh          $a2, 0x6($s0)') != 1:
        raise ValueError(f'{source}: changed scinfobar slot/call anchor')
    updated = body.replace(anchor,
        '    // 0x800FA5CC: jal         0x8008A640\n'
        '    // 0x800FA5D0: lh          $a3, 0x8($s0)\n'
        '    ctx->r7 = MEM_H(ctx->r16, 0X8);\n'
        '    tooie_hud_counter_adjust(rdram, ctx);\n', 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for HUD counters')
    text = text.replace(include, include +
        'extern void tooie_hud_counter_adjust(uint8_t*, recomp_context*);\n', 1)
    return text.replace(body, updated, 1), 1


def apply_hud_counter_rect_hook(text, source='<generated>'):
    """Scope RT64 rectangle/scissor alignment to the numeric callback."""
    matches = list(re.finditer(
        r'RECOMP_FUNC void scinfobar_entrypoint_24\([^\n]*\n.*?^;}\n',
        text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate scinfobar callback owner')
    body = matches[0][0]
    if 'tooie_hud_counter_rect_begin' in body or 'tooie_hud_counter_rect_end' in body:
        raise ValueError(f'{source}: HUD counter rectangle hooks already present')
    if re.search(r'MEM_[A-Z]+\(0X30, ctx->r29\)|MEM_[A-Z]+\(ctx->r29, 0X30\)|0x30\(\$sp\)',
            body, re.I):
        raise ValueError(f'{source}: scinfobar guest-frame marker is no longer free')
    # This hook runs before the generic observed-callsite pass adds the
    # push/pop wrapper; anchor the original generated jalr itself.
    anchor = '    LOOKUP_FUNC(ctx->r2)(rdram, ctx);\n'
    if body.count(anchor) != 1 or body.count(
            '// 0x808007D0: jalr        $v0') != 1:
        raise ValueError(f'{source}: changed scinfobar callback anchor')
    updated = body.replace(anchor,
        '    tooie_hud_counter_rect_begin(rdram, ctx);\n' +
        anchor +
        '    tooie_hud_counter_rect_end(rdram, ctx);\n', 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for HUD rectangles')
    text = text.replace(include, include +
        'extern void tooie_hud_counter_rect_begin(uint8_t*, recomp_context*);\n'
        'extern void tooie_hud_counter_rect_end(uint8_t*, recomp_context*);\n', 1)
    return text.replace(body, updated, 1), 2


def apply_hud_ortho_hook(text, source='<generated>'):
    """Normalize both native 2D ortho branches to the selected HUD width.

    func_800E42F0 is shared by sprite and text submission. Its original
    4:3 and anamorphic-widescreen branches load different horizontal extents;
    both must be corrected at their guOrtho seam so paired art/text agree.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_800E42F0\([^\n]*\n.*?^;}\n', text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate HUD ortho owner')
    body = matches[0][0]
    if 'tooie_hud_ortho_adjust' in body:
        raise ValueError(f'{source}: HUD ortho hook already present')
    anchors = (
        ('    // 0x800E4360: jal         0x8002DE04\n'
         '    // 0x800E4364: swc1        $f10, 0x1C($sp)\n'
         '    MEM_W(0X1C, ctx->r29) = ctx->f10.u32l;\n'
         '    guOrtho(rdram, ctx);\n'),
        ('    // 0x800E43B0: jal         0x8002DE04\n'
         '    // 0x800E43B4: swc1        $f6, 0x1C($sp)\n'
         '    MEM_W(0X1C, ctx->r29) = ctx->f6.u32l;\n'
         '    guOrtho(rdram, ctx);\n'),
    )
    for anchor in anchors:
        if body.count(anchor) != 1:
            raise ValueError(f'{source}: changed HUD ortho callsite')
        body = body.replace(anchor, anchor.replace(
            '    guOrtho(rdram, ctx);\n',
            '    tooie_hud_ortho_adjust(rdram, ctx);\n'
            '    guOrtho(rdram, ctx);\n'), 1)
    include = '#include "funcs.h"\n'
    declaration = 'extern void tooie_hud_ortho_adjust(uint8_t*, recomp_context*);\n'
    if text.count(include) != 1 or declaration in text:
        raise ValueError(f'{source}: unexpected generated include layout for HUD ortho')
    text = text.replace(include, include + declaration, 1)
    original = matches[0][0]
    return text.replace(original, body, 1), 2


def apply_egg_reticle_ortho_hook(text, source='<generated>'):
    """Restore only the egg cursor sprite's original X matrix coefficient.

    The cursor sprite applies its own inverse-aspect X scale before its draw.
    Hook after that draw, while D_8012D554 still names its ortho matrix, so
    no caller-wide state or world projection is changed.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void baeggcursor_entrypoint_1\([^\n]*\n.*?^;}\n',
        text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate egg cursor draw owner')
    body = matches[0][0]
    if 'tooie_hud_egg_reticle_ortho_restore' in body:
        raise ValueError(f'{source}: egg reticle ortho hook already present')
    anchor = ('    func_800E30E0(rdram, ctx);\n'
              '        goto after_9;\n'
              '    // 0x808000F8: addiu       $a3, $sp, 0x30\n'
              '    ctx->r7 = ADD32(ctx->r29, 0X30);\n'
              '    after_9:\n')
    if body.count(anchor) != 1:
        raise ValueError(f'{source}: changed egg cursor sprite draw anchor')
    body = body.replace(anchor, anchor +
        '    tooie_hud_egg_reticle_ortho_restore(rdram, ctx);\n', 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for egg reticle')
    text = text.replace(include, include +
        'extern void tooie_hud_egg_reticle_ortho_restore(uint8_t*, recomp_context*);\n', 1)
    return text.replace(matches[0][0], body, 1), 1


def apply_cutscene_skip_input_hook(text, source='<generated>'):
    """Merge a queued host request into the intro tick's original Start test.

    This preserves func_80800530_chintroticker as the sole transition owner,
    including its original map-specific skip table and state changes.
    """
    matches = list(re.finditer(
        r'RECOMP_FUNC void func_808005AC_chintroticker\([^\n]*\n.*?^;}\n',
        text, re.S | re.M))
    if not matches:
        return text, 0
    if len(matches) != 1:
        raise ValueError(f'{source}: duplicate intro ticker')
    body = matches[0][0]
    if 'tooie_cutscene_skip_input' in body:
        raise ValueError(f'{source}: cutscene-skip input hook already present')
    anchor = ('    func_80016B30(rdram, ctx);\n'
              '        goto after_7;\n'
              '    // 0x80800680: or          $a1, $s0, $zero\n'
              '    ctx->r5 = ctx->r16 | 0;\n'
              '    after_7:\n')
    if body.count(anchor) != 1 or body.count('// 0x80800684: bne         $v0, $s0, L_80800694') != 1:
        raise ValueError(f'{source}: changed original intro Start-test anchor')
    updated = body.replace(anchor, anchor +
        '    ctx->r2 = tooie_cutscene_skip_input((int)ctx->r2);\n', 1)
    include = '#include "funcs.h"\n'
    if text.count(include) != 1:
        raise ValueError(f'{source}: unexpected generated include layout for cutscene skip')
    text = text.replace(include, include +
        'extern int tooie_cutscene_skip_input(int);\n', 1)
    return text.replace(body, updated, 1), 1


def apply_music_volume_hooks(text, source='<generated>'):
    """Pin volume hooks to Tooie's private music manager and setter.

    func_800FB968 owns the six 0x50-byte music slots and consumes the original
    D_801359B0 refresh byte. func_80017404 is their master-volume setter. Its
    complete caller set is confined to that manager and its player housekeeping;
    the separate func_800C4xxx spatial sound-effect service does not call it.
    """
    targets = {
        'func_800FB968': (
            ('    uint64_t hi = 0, lo = 0, result = 0;\n'
             '    int c1cs = 0;\n'),
            '    tooie_music_volume_tick(rdram, ctx);\n',
            'extern void tooie_music_volume_tick(uint8_t*, recomp_context*);\n'),
        'func_80017404': (
            ('    // 0x8001744C: sh          $a1, 0x26($sp)\n'
             '    MEM_H(0X26, ctx->r29) = ctx->r5;\n'
             '    // 0x80017450: addiu       $a0, $v0, 0x1E0\n'),
            '    tooie_music_volume_apply(rdram, ctx);\n',
            'extern void tooie_music_volume_apply(uint8_t*, recomp_context*);\n'),
    }
    count = 0
    declarations = []
    for match in list(re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n', text, re.S | re.M)):
        name, body = match[1], match[0]
        if name not in targets:
            continue
        anchor, hook, declaration = targets[name]
        if hook in body:
            raise ValueError(f'{source}: music-volume hook already present: {name}')
        if body.count(anchor) != 1:
            raise ValueError(f'{source}: changed music-volume entry anchor: {name}')
        if name == 'func_80017404':
            # The original 0x80017448 and 0x8001744C stores have already
            # preserved the unscaled low-level cache and stack copy here.
            # Original a0 still identifies the six-player slot for Jukebox
            # classification. Scale only a1 for the event-posting call;
            # 0x80017460 reloads the original before housekeeping paths run.
            replacement = anchor.replace(
                '    // 0x80017450: addiu       $a0, $v0, 0x1E0\n',
                hook + '    // 0x80017450: addiu       $a0, $v0, 0x1E0\n')
        else:
            replacement = anchor + hook
        text = text.replace(body, body.replace(anchor, replacement, 1), 1)
        declarations.append(declaration)
        count += 1
    if declarations:
        include = '#include "funcs.h"\n'
        if text.count(include) != 1:
            raise ValueError(f'{source}: unexpected generated include layout for music volume')
        text = text.replace(include, include + ''.join(declarations), 1)
    return text, count


_MENU_BODIES={
    'func_808006A0_chgameselect':('aa44ebe3bfc0bd58ac4fed2d5a4cd2059c1ce371bd7901d40aa6b704a414436e',(0x808006A0,0x8080074C,0x8080085C)),
    'func_8080086C_chgameselect':('b875c2f72f1293da5a13a86921896b3b0a037a65f0642a4c0ba1ff0481d47c21',(0x8080086C,0x808008B0)),
    'func_80800D3C_chgameselect':('6ee500c84c2b2cafea8898d28f8ba8a907b61b9c12cf049670677bbb1a97b3a6',(0x80800D4C,)),
    'func_80800E24_chgameselect':('0770131ff58f15389e8329577905f08ab30cac25380b3b21c91a406c9d0b58c7',(0x80800EB0,0x80801000,0x80801014,0x80801048)),
    'func_808013C8_chgameselect':('78ac57f94c00f4d2d8cb19d8a356e2eb23a08e39720b2d8d2503937d84a6eff8',(0x8080140C,)),
}

def observe_menu(text,source='<generated>',section_index=58):
    """Eleven before-instruction snapshots; complete pinned bodies include producers/delay slots."""
    found=set();count=0
    for match in list(re.finditer(r'RECOMP_FUNC void (\w+)\([^\n]*\n.*?^;}\n',text,re.S|re.M)):
        name,body=match[1],match[0]
        if name not in _MENU_BODIES:continue
        if name in found:raise ValueError(f'{source}: duplicate menu function {name}')
        found.add(name);expected,sites=_MENU_BODIES[name]
        if section_index is None:raise ValueError(f'{source}: menu function lacks selected section metadata')
        # Only the compiler's selected-section ordinal is variable. Bind every
        # own-section relocation to the supplied .chgameselect metadata index,
        # then canonicalize that integer for hashing; emit original bytes.
        def canonical_reloc(match):
            if int(match[2])!=section_index:raise ValueError(f'{source}: wrong menu relocation section in {name}')
            return match[1]+'58'+match[3]
        canonical=re.sub(r'(RELOC_(?:HI16|LO16)\()(\d+)(,)',canonical_reloc,body)
        if hashlib.sha256(canonical.encode()).hexdigest()!=expected:raise ValueError(f'{source}: changed original menu body {name}')
        updated=body
        for pc in sites:
            anchor=f'    // 0x{pc:08X}:'
            if updated.count(anchor)!=1:raise ValueError(f'{source}: ambiguous menu instruction {name}:{pc:08X}')
            updated=updated.replace(anchor,f'    tooie_observe_menu(rdram, ctx, 0x{pc:08X}u);\n'+anchor,1);count+=1
        text=text.replace(body,updated,1)
    return text,count


def instrument(directory, sections):
    addresses={f['name']: f['vram'] for s in sections for f in s['functions']}
    menu_indices=[i for i,s in enumerate(sections) if s.get('name')=='.chgameselect']
    if len(menu_indices)>1:raise ValueError('duplicate selected chgameselect section')
    menu_section_index=menu_indices[0] if menu_indices else None
    stubs={f['name']:f['vram'] for s in sections for f in s['functions'] if f['name'].startswith('_') and '_entrypoint_' in f['name'] and f['size']==8}
    counts={'entries':0,'labels':0,'observed_syscall_callsites':0,'si_completion_observations':0,
            'observed_indirect_jalr':0,'observed_indirect_tail_jr':0,'observed_direct_lookup':0,
            'corrected_saved_ra_returns':[],'global_settings_observations':0,'widescreen_profile_hooks':0,'graphics_observations':0,'title_observations':0,'map_actor_list_observations':0,'menu_observations':0,'save_progress_pause_hooks':0,'game_feature_hooks':0,'first_person_analog_hooks':0,'free_camera_hooks':0,'camera_interpolation_hooks':0,'scene_observer_hooks':0,'guest_update_observers':0,'world_call_timing_hooks':0,'nested_scene_call_timing_hooks':0,'heap_realloc_safety_fixes':0,'game_delta_observers':0,'replay_timing_hooks':0,'native_aspect_hooks':0,'actor_draw_distance_hooks':0,'hud_counter_layout_hooks':0,'hud_counter_rect_hooks':0,'hud_ortho_hooks':0,'egg_reticle_ortho_hooks':0,'cutscene_skip_input_hooks':0,'music_volume_hooks':0}
    counts['practice_form_hooks']=0
    counts['model_interpolation_hooks']=0
    counts['intro_model_draw_hooks']=0
    rewrites={
        '    ctx->r15 = MEM_W(ctx->r14, -0X4E0C);': '    ctx->r15 = tooie_uncached_word(rdram, (uint32_t)ADD32(ctx->r14, -0X4E0C));',
        '    ctx->r25 = MEM_W(ctx->r24, -0X1E40);': '    ctx->r25 = tooie_uncached_word(rdram, (uint32_t)ADD32(ctx->r24, -0X1E40));',
    }
    rewritten={key:0 for key in rewrites}
    overlay_counts={'capture':0,'publish':0,'free':0,'pi_status':0,'rom_key':0}
    for path in sorted(Path(directory).glob('*.c')):
        text, corrected=correct_saved_ra_returns(path.read_text(), str(path))
        counts['corrected_saved_ra_returns'].extend(corrected)
        text,settings_count=observe_global_settings(text,str(path))
        counts['global_settings_observations']+=settings_count
        text,widescreen_count=apply_widescreen_profile(text,str(path))
        counts['widescreen_profile_hooks']+=widescreen_count
        text,graphics_count=observe_graphics(text,str(path))
        counts['graphics_observations']+=graphics_count
        text,title_count=observe_title(text,str(path))
        counts['title_observations']+=title_count
        text,map_list_count=observe_map_actor_list(text,str(path))
        counts['map_actor_list_observations']+=map_list_count
        text,menu_count=observe_menu(text,str(path),menu_section_index)
        counts['menu_observations']+=menu_count
        text,save_progress_count=apply_save_progress_pause_hook(text,str(path))
        counts['save_progress_pause_hooks']+=save_progress_count
        text,game_feature_count=apply_game_feature_hooks(text,str(path))
        counts['game_feature_hooks']+=game_feature_count
        text,practice_form_count=apply_practice_form_hooks(text,str(path))
        counts['practice_form_hooks']+=practice_form_count
        text,first_person_analog_count=apply_first_person_analog_hooks(text,str(path))
        counts['first_person_analog_hooks']+=first_person_analog_count
        text,free_camera_count=apply_free_camera_hooks(text,str(path))
        counts['free_camera_hooks']+=free_camera_count
        text,camera_interpolation_count=apply_camera_interpolation_hook(text,str(path))
        counts['camera_interpolation_hooks']+=camera_interpolation_count
        text,model_interpolation_count=apply_model_interpolation_hooks(text,str(path))
        counts['model_interpolation_hooks']+=model_interpolation_count
        text,intro_model_draw_count=apply_intro_model_draw_hooks(text,str(path))
        counts['intro_model_draw_hooks']+=intro_model_draw_count
        text,scene_observer_count=apply_scene_observer_hooks(text,str(path))
        counts['scene_observer_hooks']+=scene_observer_count
        text,guest_update_count=apply_guest_update_observer(text,str(path))
        counts['guest_update_observers']+=guest_update_count
        text,world_call_count=apply_gsworld_call_timing_hooks(text,str(path))
        counts['world_call_timing_hooks']+=world_call_count
        text,nested_scene_call_count=apply_scene_nested_call_timing_hooks(text,str(path))
        counts['nested_scene_call_timing_hooks']+=nested_scene_call_count
        text,heap_realloc_safety_count=apply_heap_realloc_safety_fix(text,str(path))
        counts['heap_realloc_safety_fixes']+=heap_realloc_safety_count
        text,game_delta_count=apply_game_delta_observer(text,str(path))
        counts['game_delta_observers']+=game_delta_count
        text,native_aspect_count=apply_native_aspect_hook(text,str(path))
        counts['native_aspect_hooks']+=native_aspect_count
        text,replay_timing_count=apply_replay_timing_hooks(text,str(path))
        counts['replay_timing_hooks']+=replay_timing_count
        text,actor_distance_count=apply_actor_draw_distance_hooks(text,str(path))
        counts['actor_draw_distance_hooks']+=actor_distance_count
        text,hud_counter_layout_count=apply_hud_counter_layout_hook(text,str(path))
        counts['hud_counter_layout_hooks']+=hud_counter_layout_count
        text,hud_counter_rect_count=apply_hud_counter_rect_hook(text,str(path))
        counts['hud_counter_rect_hooks']+=hud_counter_rect_count
        text,hud_ortho_count=apply_hud_ortho_hook(text,str(path))
        counts['hud_ortho_hooks']+=hud_ortho_count
        text,egg_reticle_ortho_count=apply_egg_reticle_ortho_hook(text,str(path))
        counts['egg_reticle_ortho_hooks']+=egg_reticle_ortho_count
        text,cutscene_skip_count=apply_cutscene_skip_input_hook(text,str(path))
        counts['cutscene_skip_input_hooks']+=cutscene_skip_count
        text,music_volume_count=apply_music_volume_hooks(text,str(path))
        counts['music_volume_hooks']+=music_volume_count
        lines=text.splitlines(keepends=True)
        observations=callsite_observations(lines, stubs, str(path))
        starts={}
        ends={}
        timing_start=re.compile(r'\s*tooie_scene_(?:world_call|nested_call)_started\(.*\);\s*')
        timing_finish=re.compile(r'\s*tooie_scene_(?:world_call|nested_call)_finished\(.*\);\s*')
        for call_index, observation in observations.items():
            start=call_index-1 if call_index and timing_start.fullmatch(lines[call_index-1]) else call_index
            end=call_index+1 if call_index+1<len(lines) and timing_finish.fullmatch(lines[call_index+1]) else call_index
            if start in starts or end in ends:raise ValueError(f'{path}: overlapping observed call wrappers')
            starts[start]=observation;ends[end]=observation
        result=[]
        current=0
        current_name=''
        for index, line in enumerate(lines):
            match=re.match(r'RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{',line)
            if match: current_name=match[1]
            if index in starts:
                call_pc, kind=starts[index]
                result.append(f'    tooie_overlay_callsite_push(0x{call_pc:08X}u);\n')
                counts['observed_syscall_callsites' if kind=='direct_stub' else 'observed_'+kind]+=1
            if line.rstrip('\n') in rewrites:
                key=line.rstrip('\n');line=rewrites[key]+'\n';rewritten[key]+=1
            if current_name=='func_80082088':
                if line.strip()=='ctx->r10 = MEM_W(ctx->r9, 0X0);':
                    line='    ctx->r10 = tooie_overlay_hardware_word((uint32_t)ctx->r9);\n';overlay_counts['pi_status']+=1
                if line.strip()=='ctx->r21 = MEM_W(ctx->r21, 0X40);':
                    line='    ctx->r21 = tooie_overlay_hardware_word((uint32_t)ADD32(ctx->r21,0X40));\n';overlay_counts['rom_key']+=1
                if line.strip()=='return;':
                    result.append('    tooie_validate_overlay(rdram, tooie_overlay_header, tooie_overlay_delta);\n')
                    result.append('    tooie_overlay_after_relocate(rdram, tooie_overlay_header, tooie_overlay_delta);\n')
                    overlay_counts['publish']+=1
            if current_name=='ovl_unload' and line.strip()=='heap_free(rdram, ctx);':
                result.append('    tooie_overlay_before_free(rdram, (uint32_t)ctx->r4);\n')
                overlay_counts['free']+=1
            si_pc=re.match(r'\s*// 0x(8001DFB0|8001DFC8|8001E1B0):',line)
            if si_pc:
                pc=int(si_pc[1],16)
                assert current_name==('func_8001E170' if pc==0x8001E1B0 else 'func_8001DEE8'),current_name
                result.append(f'    tooie_continuous_poll(rdram, ctx, 0x{pc:08X}u);\n')
                counts['si_completion_observations']+=1
            result.append(line)
            if index in ends:result.append('    tooie_overlay_callsite_pop();\n')
            match=re.match(r'RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{',line)
            if match:
                name=match[1]
                current=addresses.get(name,0x80000400 if name=='recomp_entrypoint' else 0)
                result.append(f'    tooie_continuous_poll(rdram, ctx, 0x{current:08X}u);\n')
                counts['entries']+=1
                if name=='func_80082088':
                    result.append('    uint32_t tooie_overlay_header=(uint32_t)ctx->r4;\n    int32_t tooie_overlay_delta=(int32_t)ctx->r5;\n')
                    overlay_counts['capture']+=1
            elif re.match(r'^L_[0-9A-Fa-f]+:',line):
                pc=int(line.split(':')[0][2:],16)
                result.append(f'    tooie_continuous_poll(rdram, ctx, 0x{pc:08X}u);\n')
                counts['labels']+=1
        path.write_text(''.join(result))
    assert all(value==1 for value in rewritten.values()),rewritten
    expected_si=2*int('func_8001DEE8' in addresses)+int('func_8001E170' in addresses)
    assert counts['si_completion_observations']==expected_si,counts
    counts['scoped_uncached_reads']=sum(rewritten.values())
    # PC80082144 is emitted twice for the branch-likely delay slot, plus the
    # initial PC80082138 read: three generated expressions for two instructions.
    assert overlay_counts=={'capture':1,'publish':1,'free':1,'pi_status':3,'rom_key':1},overlay_counts
    expected_saved_ra=sorted(set(addresses)&set(_SAVED_RA_RETURNS))
    assert sorted(counts['corrected_saved_ra_returns'])==expected_saved_ra,counts['corrected_saved_ra_returns']
    expected_settings=int('func_80800168_glglobalsettings' in addresses)+4*int('glglobalsettings_entrypoint_1' in addresses)
    assert counts['global_settings_observations']==expected_settings,counts
    assert counts['widescreen_profile_hooks']==int('func_80800034_glglobalsettings' in addresses),counts
    expected_graphics=len(set(addresses)&{'func_80014A88','func_80013E34','func_80013EF0','func_80013FE4','func_800146D8'})
    assert counts['graphics_observations']==expected_graphics,counts
    expected_title=6*int('func_808005AC_chintroticker' in addresses)+int('func_80800738_gcfrontend' in addresses)+int('gsworldDll_entrypoint_2' in addresses)+int('gsattract_entrypoint_1' in addresses)
    assert counts['title_observations']==expected_title,counts
    assert counts['map_actor_list_observations']==int('gspropsDll_entrypoint_1' in addresses),counts
    assert counts['menu_observations']==sum(len(value[1]) for name,value in _MENU_BODIES.items() if name in addresses),counts
    assert counts['save_progress_pause_hooks']==int('gcnewpause_entrypoint_2' in addresses),counts
    counts['expected_game_feature_hooks']=sum(int(name in addresses) for name in (
        'bainput_update','func_80800018_glcutDll','func_800DA0B4','func_800DA188','func_800DAC10','func_800A4878'))
    assert counts['game_feature_hooks']==counts['expected_game_feature_hooks'],counts
    counts['expected_practice_form_hooks']=int('func_800F8B94' in addresses)+2*int('basetup_entrypoint_2' in addresses)
    assert counts['practice_form_hooks']==counts['expected_practice_form_hooks'],counts
    counts['expected_first_person_analog_hooks']=2*int('func_808002A4_bafpctrl' in addresses)
    assert counts['first_person_analog_hooks']==counts['expected_first_person_analog_hooks'],counts
    counts['expected_free_camera_hooks']=2*int('func_800E9F20' in addresses)
    assert counts['free_camera_hooks']==counts['expected_free_camera_hooks'],counts
    counts['expected_camera_interpolation_hooks']=int('func_800E42B4' in addresses)
    assert counts['camera_interpolation_hooks']==counts['expected_camera_interpolation_hooks'],counts
    counts['expected_model_interpolation_hooks']=4*int('func_800DE498' in addresses)
    assert counts['model_interpolation_hooks']==counts['expected_model_interpolation_hooks'],counts
    counts['expected_intro_model_draw_hooks']=6*int('func_80803A78_chintrochar' in addresses)
    assert counts['intro_model_draw_hooks']==counts['expected_intro_model_draw_hooks'],counts
    counts['expected_scene_observer_hooks']=int('func_800EA05C' in addresses)+12*int('func_800A72A4' in addresses)
    assert counts['scene_observer_hooks']==counts['expected_scene_observer_hooks'],counts
    counts['expected_guest_update_observers']=int('func_800A73F4' in addresses)
    assert counts['guest_update_observers']==counts['expected_guest_update_observers'],counts
    counts['expected_world_call_timing_hooks']=59*int('gsworldDll_entrypoint_2' in addresses)
    assert counts['world_call_timing_hooks']==counts['expected_world_call_timing_hooks'],counts
    counts['expected_nested_scene_call_timing_hooks']=56*int('func_800A5C28' in addresses and 'func_800F73C4' in addresses)
    assert counts['nested_scene_call_timing_hooks']==counts['expected_nested_scene_call_timing_hooks'],counts
    counts['expected_game_delta_observers']=int('func_800123F4' in addresses)
    assert counts['game_delta_observers']==counts['expected_game_delta_observers'],counts
    counts['expected_replay_timing_hooks']=(
        2*int('func_8001608C' in addresses)+2*int('func_800151BC' in addresses))
    assert counts['replay_timing_hooks']==counts['expected_replay_timing_hooks'],counts
    counts['expected_native_aspect_hooks']=int('func_80015D14' in addresses)
    assert counts['native_aspect_hooks']==counts['expected_native_aspect_hooks'],counts
    counts['expected_actor_draw_distance_hooks']=2*int('func_80101970' in addresses)
    assert counts['actor_draw_distance_hooks']==counts['expected_actor_draw_distance_hooks'],counts
    counts['expected_hud_counter_layout_hooks']=int('func_800FA508' in addresses)
    assert counts['hud_counter_layout_hooks']==counts['expected_hud_counter_layout_hooks'],counts
    counts['expected_hud_counter_rect_hooks']=2*int('scinfobar_entrypoint_24' in addresses)
    assert counts['hud_counter_rect_hooks']==counts['expected_hud_counter_rect_hooks'],counts
    counts['expected_hud_ortho_hooks']=2*int('func_800E42F0' in addresses)
    assert counts['hud_ortho_hooks']==counts['expected_hud_ortho_hooks'],counts
    counts['expected_egg_reticle_ortho_hooks']=int('baeggcursor_entrypoint_1' in addresses)
    assert counts['egg_reticle_ortho_hooks']==counts['expected_egg_reticle_ortho_hooks'],counts
    counts['expected_cutscene_skip_input_hooks']=int('func_808005AC_chintroticker' in addresses)
    assert counts['cutscene_skip_input_hooks']==counts['expected_cutscene_skip_input_hooks'],counts
    counts['expected_music_volume_hooks']=sum(int(name in addresses) for name in (
        'func_800FB968','func_80017404'))
    assert counts['music_volume_hooks']==counts['expected_music_volume_hooks'],counts
    counts['overlay_hooks']=overlay_counts
    return counts
