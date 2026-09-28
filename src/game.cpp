#include "game.hpp"
#include "boot_hooks.h"
#include "thread_gate.hpp"
#include "continuous_host.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/addresses.hpp"
#include "platform_support.hpp"
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
// Exact signature of the unmodified runtime's non-static startup routine.
void init(uint8_t*,recomp_context*,gpr);
extern "C" void recomp_entrypoint(uint8_t*,recomp_context*);
extern "C" void func_80000450(uint8_t*,recomp_context*);
namespace tooie {
RomIdentity validate_and_install_rom(const std::filesystem::path& path) {
    if(std::filesystem::file_size(path)!=32u*1024*1024)throw std::runtime_error("Original ROM must be 32 MiB");
    std::ifstream file(path,std::ios::binary);std::vector<uint8_t> bytes(32u*1024*1024);
    if(!file.read((char*)bytes.data(),bytes.size()))throw std::runtime_error("ROM read failed");
    RomIdentity id{platform::digest(bytes,false),platform::digest(bytes,true),XXH3_64bits(bytes.data(),bytes.size()),bytes.size()};
    if(id.sha1!="af1a89e12b638b8d82cc4c085c8e01d4cba03fb3" || id.sha256!="9ec37fba6890362eba86fb855697a9cff1519275531b172083a1a6a045483583")
        throw std::runtime_error("ROM identity mismatch: requires original NTSC-U compressed ROM");
    recomp::set_rom_contents(std::move(bytes));
    // Independent runtime hash check operates on the installed original bytes.
    if(XXH3_64bits(recomp::get_rom().data(),recomp::get_rom().size())!=id.xxh3)throw std::runtime_error("Installed DMA source changed");
    return id;
}
std::string file_sha256(const std::filesystem::path& path) {
    return platform::file_sha256(path);
}
recomp::GameEntry game_entry(uint64_t hash, bool idle) {
    auto entry = recomp::GameEntry{.rom_hash=hash,.internal_name="BANJO TOOIE",.display_name="Banjo-Tooie (bounded diagnostic)",.game_id=u8"bt.n64.us.1.0",.mod_game_id="",.save_type=recomp::SaveType::Eep16k,
        .thumbnail_bytes={},.is_enabled=true,.decompression_routine=nullptr,.has_compressed_code=true,
        .entrypoint_address=(gpr)(int32_t)0x80000400,.entrypoint=recomp_entrypoint};
    if(idle)entry.thread_create_callback=thread_create_callback;
    return entry;
}
static bool reached_boundary;
static uint32_t guard_before,guard_after;
static void check_boot_entry(uint8_t*rdram,recomp_context*ctx,bool diagnostic_stop) {
    capture_guest_state(rdram,ctx);
    bool zero=true;for(uint32_t a=0x44E0;a<0x8470;a++)zero &= rdram[a^3]==0;
    bool guards=continuous_enabled() || ((uint32_t)MEM_W(0,(gpr)(int32_t)0x800044DC)==guard_before && (uint32_t)MEM_W(0,(gpr)(int32_t)0x80008470)==guard_after);
    bool state=ctx->r29==(gpr)(int32_t)0x800064E0 && ctx->r8==(gpr)(int32_t)0x80008470 && ctx->r9==0 && ctx->r10==(gpr)(int32_t)0x80000450;
    trace(diagnostic_stop?"boot_boundary":"boot_procedure",diagnostic_stop?"D":"E",zero&&guards&&state?(diagnostic_stop?"diagnostic_stop":"entered"):"failure",0x80000450,ctx,
        {{"bss_zero",zero},{"bss_bytes",0x3F90},{"bss_guards_preserved",continuous_enabled()?Json(nullptr):Json(guards)},{"t0",hex32(ctx->r8)},{"t1",hex32(ctx->r9)},{"t2",hex32(ctx->r10)},{"original_func_80000450_executed",!diagnostic_stop}});
    if(!zero||!guards||!state)throw std::runtime_error("Generated entry BSS/stack state mismatch");
    reached_boundary=true;
}
static void boot_boundary(uint8_t*rdram,recomp_context*ctx){check_boot_entry(rdram,ctx,true);}
void boot_procedure_entry(uint8_t*rdram,recomp_context*ctx){check_boot_entry(rdram,ctx,false);}
bool run_boot_diagnostic(bool core1, bool idle, bool main) {
    idle = idle || main;
    core1 = core1 || idle;
    // Same reservation/protection sizes as recomp::start, without frontend or thread startup.
    auto* allocation=platform::reserve_rdram(recomp::allocation_size,recomp::mem_size);
    struct Memory {
        uint8_t*p;bool idle;
        ~Memory(){
            // A cleanup exception must never free storage still read by a worker.
            if(idle&&!thread_gate_memory_safe()) {
                trace("rdram_retained_cleanup_failure","F-stop","failure",0,nullptr);
                return;
            }
            platform::release_rdram(p,recomp::allocation_size);
            if(idle)trace("rdram_released_after_cleanup","F-stop","observed",0,nullptr);
        }
    } memory{allocation,idle};
    auto*rdram=(uint8_t*)allocation;recomp_context ctx{};
    register_overlays();init(rdram,&ctx,(gpr)(int32_t)0x80000400);
    capture_guest_state(rdram,&ctx);
    auto rom=recomp::get_rom();
    for(size_t i=0;i<0x100000;i++)if(rdram[(0x400+i)^3]!=rom[0x1000+i])throw std::runtime_error("Original initial DMA mismatch");
    if(MEM_W(0,(gpr)(int32_t)0x80000300)!=1 || MEM_W(0,(gpr)(int32_t)0x80000308)!=(int32_t)0xB0000000 || MEM_W(0,(gpr)(int32_t)0x8000030C)!=0 || MEM_W(0,(gpr)(int32_t)0x80000318)!=0x800000 || ctx.f_odd!=&ctx.f0.u32h || ctx.mips3_float_mode)
        throw std::runtime_error("Runtime IPL3/float setup mismatch");
    if(get_function((int32_t)0x80000400)!=recomp_entrypoint || get_function((int32_t)0x80000450)!=func_80000450)throw std::runtime_error("Boot registry mismatch");
    trace("checkpoint","C","pass",0x80000400,&ctx,{{"original_initial_dma_bytes",0x100000},{"guest_reported_ram_bytes",0x800000},{"host_accessible_bytes",recomp::mem_size},{"runtime_init","original librecomp init"},{"float_mode","FR=0"}});
    // Poison only the BSS interval. Retain neighboring ROM-derived words as overrun guards.
    guard_before=MEM_W(0,(gpr)(int32_t)0x800044DC);guard_after=MEM_W(0,(gpr)(int32_t)0x80008470);
    for(size_t i=0x44E0;i<0x8470;i++)rdram[i^3]=0xA5;
    capture_guest_state(rdram,&ctx);
    reached_boundary=false;
    if(core1)prepare_core1_diagnostic(rdram,idle,main);
    else recomp::overlays::add_loaded_function((int32_t)0x80000450,boot_boundary);
    trace("checkpoint","D","entered",0x80000400,&ctx,{{"entry_kind","actual generated recomp_entrypoint"},{"diagnostic_lookup_override",core1?Json(nullptr):Json("0x80000450")}});
    start_watchdog();
    try {
        get_function((int32_t)0x80000400)(rdram,&ctx);
        if(idle)finish_thread_gate(rdram);
    }catch(...) {
        if(idle)cleanup_thread_gate(rdram);
        stop_watchdog();capture_guest_state(rdram,&ctx);fault_packet();restore_core1_boundary();recomp::overlays::add_loaded_function((int32_t)0x80000450,func_80000450);throw;
    }
    stop_watchdog();
    if(core1){restore_core1_boundary();if(!idle&&!core1_boundary_reached())throw std::runtime_error("Core1 handoff not reached");}
    recomp::overlays::add_loaded_function((int32_t)0x80000450,func_80000450);
    if(get_function((int32_t)0x80000450)!=func_80000450)throw std::runtime_error("Diagnostic lookup not restored");
    if(!reached_boundary)throw std::runtime_error("Generated entry returned without checkpoint");
    return true;
}
}
