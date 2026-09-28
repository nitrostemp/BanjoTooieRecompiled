#include "boot_hooks.h"
#include "game.hpp"
#include "thread_gate.hpp"
#include "continuous_host.hpp"
#include "build_metadata.hpp"
#include "platform_support.hpp"
#include "librecomp/overlays.hpp"
#include "miniz.h"
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>
extern "C" void func_80012030(uint8_t*,recomp_context*);
namespace {
using tooie::Json;
Json reference;
std::vector<uint8_t> expected;
bool enabled, reached, installed, idle_mode, main_mode, continuous_mode;
unsigned stream_index;
uint32_t input_slot, output_slot;
std::vector<std::pair<uint32_t,uint32_t>> guards;
std::filesystem::path source_root() {
    if (const char* configured = std::getenv("TOOIE_SOURCE_DIR"); configured && *configured)
        return std::filesystem::path(configured);
    auto directory = tooie::platform::executable_path().parent_path();
    for (unsigned depth = 0; depth < 5; ++depth) {
        if (std::filesystem::is_regular_file(directory / "generated" / "boot-reference.json") &&
            std::filesystem::is_regular_file(directory / "generated" / "core1-expected.bin"))
            return directory;
        if (directory == directory.parent_path()) break;
        directory = directory.parent_path();
    }
    throw std::runtime_error("Boot reference missing; set TOOIE_SOURCE_DIR to the prepared source checkout");
}
std::filesystem::path decomp_root(const std::filesystem::path& source) {
    if (const char* configured = std::getenv("TOOIE_DECOMP_ROOT"); configured && *configured)
        return std::filesystem::path(configured);
    return source / "deps" / "banjo-tooie";
}
gpr guest(uint32_t a){return (gpr)(int32_t)a;}
uint32_t word(uint8_t*rdram,uint32_t a){return MEM_W(0,guest(a));}
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
uint32_t input_address(uint32_t rom){return reference.at("scratch_base").get<uint32_t>()+rom-reference["symbols"]["core1_compressed_ROM_START"].get<uint32_t>();}
Json mismatch(uint8_t*rdram,uint32_t begin,uint32_t size) {
    uint32_t offset=begin-reference["core1"]["vram"].get<uint32_t>();
    require((uint64_t)offset+size<=expected.size(),"Reference comparison range invalid");
    for(uint32_t i=0;i<size;i++) {
        uint8_t actual=MEM_B(i,guest(begin));
        if(actual!=expected[offset+i])return {{"guest_address",tooie::hex32(begin+i)},{"offset",offset+i},{"expected",expected[offset+i]},{"actual",actual}};
    }
    return nullptr;
}
bool guard_check(uint8_t*rdram) {
    for(auto[a,value]:guards)if(word(rdram,a)!=value)return false;
    return true;
}
void derive_expected_from_rom() {
    const auto& bundle=tooie::build_metadata::current();
    reference=Json::parse(bundle.text(tooie::build_metadata::File::Core1Reference));
    require(reference.at("schema")==1,"Core1 metadata schema mismatch");
    const auto rom=recomp::get_rom();
    require(reference.at("source_sha256")==tooie::platform::digest(rom,true),"Core1 metadata ROM identity mismatch");
    const uint32_t compressed_start=reference.at("symbols").at("core1_compressed_ROM_START");
    const uint32_t compressed_end=reference.at("symbols").at("core1_compressed_ROM_END");
    require(compressed_start<compressed_end&&compressed_end<=rom.size(),"Core1 compressed ROM range invalid");
    require(reference.at("streams").size()==2,"Core1 stream count mismatch");
    expected.clear();
    uint32_t next_rom=compressed_start;
    uint32_t next_output=reference.at("core1").at("vram");
    for(const auto& stream:reference.at("streams")) {
        const uint32_t start=stream.at("rom_start"),end=stream.at("rom_end");
        const uint32_t size=stream.at("output_bytes");
        require(start==next_rom&&start+2<=end&&end<=compressed_end,"Core1 stream ROM range invalid");
        require(stream.at("output_start")==next_output,"Core1 stream output range invalid");
        require((uint32_t(rom[start])<<8|rom[start+1])*16u==size,"Core1 stream size header mismatch");
        std::vector<uint8_t> output(size);
        const size_t written=tinfl_decompress_mem_to_mem(output.data(),output.size(),
            rom.data()+start+2,end-start-2,0);
        require(written==output.size(),"Core1 selected-ROM host inflate failed");
        require(tooie::platform::digest(output,true)==stream.at("sha256").get<std::string>(),
            "Core1 selected-ROM stream SHA256 mismatch");
        uint32_t a=0,b=0;
        for(uint8_t value:output){a+=value;b^=uint32_t(value)<<(a&0x17);}
        require(stream.at("crc")==Json::array({a,b}),"Core1 selected-ROM checksum mismatch");
        expected.insert(expected.end(),output.begin(),output.end());
        next_rom=end;
        next_output=(next_output+size+15)&~15u;
        require(stream.at("next_output")==next_output,"Core1 stream alignment mismatch");
    }
    require(next_rom==reference.at("consumed_rom_end")&&
        compressed_end-next_rom==reference.at("dma_padding_bytes"),"Core1 compressed ROM padding mismatch");
    require(expected.size()==reference.at("core1").at("size").get<size_t>(),"Core1 derived size mismatch");
    require(tooie::platform::digest(expected,true)==reference.at("core1").at("sha256").get<std::string>(),
        "Core1 derived image SHA256 mismatch");
}
void core_boundary(uint8_t*rdram,recomp_context*ctx) {
    tooie::capture_guest_state(rdram,ctx);
    bool good=installed&&stream_index==2&&ctx->r29==guest(0x800064C0);
    tooie::trace("core1_boundary","E",good?"diagnostic_stop":"failure",0x80012030,ctx,
        {{"original_func_80000450_executed",true},{"original_func_80012030_executed",false},
         {"ra_precision","actual generated context; JAL return address is not materialized"},{"game_threads_started",false}});
    require(good,"Unexpected core1 boundary state");reached=true;
}
}
namespace tooie {
static void initialize_core1_observer(uint8_t*rdram) {
    uint32_t start=reference["core1"]["vram"],size=reference["core1"]["size"];
    if (!continuous_mode) {
        for(uint32_t i=0;i<size;i++)MEM_B(i,guest(start))=0xA5;
        for(uint32_t a=0x80012000;a<0x80012018;a+=4)MEM_W(0,guest(a))=0xDEADBEEF;
    }
    uint32_t scratch=reference["scratch_base"],dma_size=reference["symbols"]["core1_compressed_ROM_END"].get<uint32_t>()-reference["symbols"]["core1_compressed_ROM_START"].get<uint32_t>();
    guards.clear();
    for(uint32_t a:{start-4,start+size,scratch-4,scratch+dma_size,0x8000E7FCu,0x80011000u,0x80005000u}) {
        if (continuous_mode) break;
        // Stack sentinel lies within entry-cleared BSS, so its expected post-entry value is zero.
        uint32_t value=a==0x80005000?0u:0x739AD5C1u;
        MEM_W(0,guest(a))=value;guards.emplace_back(a,value);
    }
    stream_index=0;reached=false;installed=false;enabled=true;
    trace("core1_oracle","none","recorded",0,nullptr,{{"reference_sha256",reference["core1"]["sha256"]},
        {"reference_used_for",continuous_mode?"selected validated ROM host inflate":"offline reference host comparisons"},
        {"output_poisoned",!continuous_mode},{"checksums_poisoned",!continuous_mode}});
}
void prepare_core1_diagnostic(uint8_t*rdram, bool idle, bool main) {
    continuous_mode=false;
    idle_mode=idle;
    main_mode=main;
    const auto source=source_root();
    auto directory=source/"generated";
    std::ifstream metadata(directory/"boot-reference.json");require(bool(metadata),"Missing offline boot reference");metadata>>reference;
    require(file_sha256(directory/"core1-expected.bin")==reference["core1"]["sha256"].get<std::string>(),"Offline core1 reference hash mismatch");
    require(file_sha256(decomp_root(source)/"build/us/banjotooie_decompressed.elf")==reference["elf_sha256"].get<std::string>(),"Reference ELF changed");
    std::ifstream bytes(directory/"core1-expected.bin",std::ios::binary);
    expected.assign(std::istreambuf_iterator<char>(bytes),{});
    require(expected.size()==reference["core1"]["size"].get<size_t>(),"Reference size mismatch");
    initialize_core1_observer(rdram);
}
void prepare_core1_continuous(uint8_t*rdram) {
    continuous_mode=true;
    idle_mode=false;
    main_mode=false;
    // Ordinary frontend/game startup uses only the sealed sibling metadata
    // and the validated installed ROM. Source-tree and offline ELF discovery
    // are reserved for the explicit bounded diagnostic path above.
    derive_expected_from_rom();
    initialize_core1_observer(rdram);
}
bool core1_boundary_reached(){return reached;}
void restore_core1_boundary() {
    if(installed) {
        recomp::overlays::add_loaded_function((int32_t)0x80012030,func_80012030);
        bool ok=get_function((int32_t)0x80012030)==func_80012030;
        trace("boundary_restored","E",ok?"pass":"failure",0x80012030,nullptr,{{"real_entry_mapping_verified",ok},{"boot_kept_loaded",true}});
        installed=false;require(ok,"Core1 diagnostic mapping restoration failed");
    }
    enabled=false;
}
}
extern "C" void tooie_boot_hook(uint8_t*rdram,recomp_context*ctx,unsigned stage) {
    require(enabled,"Generated boot procedure requires core1 diagnostic observer");
    tooie::capture_guest_state(rdram,ctx);
    if(stage==0){tooie::boot_procedure_entry(rdram,ctx);return;}
    if(stage==1) {
        require(stream_index<2,"Unexpected third decompression");
        auto&s=reference["streams"][stream_index];input_slot=ctx->r4;output_slot=ctx->r5;
        uint32_t in=word(rdram,input_slot),out=word(rdram,output_slot);
        bool ok=in==input_address(s["rom_start"])&&out==s["output_start"].get<uint32_t>();
        tooie::trace("decompression_before","E",ok?"pass":"failure",0x80000560,ctx,
            {{"stream",stream_index},{"input",tooie::hex32(in)},{"output",tooie::hex32(out)},{"generated_decompressor","func_800006D4"},{"workspace", "0x8000E800..0x80011000"}});
        require(ok,"Decompression entry span mismatch");return;
    }
    if(stage==2) {
        require(stream_index<2,"Unexpected decompression completion");
        auto&s=reference["streams"][stream_index];
        uint32_t in=word(rdram,input_slot),out=word(rdram,output_slot),length=word(rdram,0x800064F0);
        Json crc={word(rdram,0x80004240),word(rdram,0x80004244)};
        Json first=mismatch(rdram,s["output_start"],s["output_bytes"]);
        bool ok=in==input_address(s["rom_end"]) && out==s["next_output"].get<uint32_t>() && !(out&15)
            && length==s["output_bytes"].get<uint32_t>() && crc==s["crc"] && first.is_null()&&guard_check(rdram);
        tooie::trace("decompression_after","E",ok?"pass":"failure",0x800005E4,ctx,
            {{"stream",stream_index},{"input",tooie::hex32(in)},{"expected_input",tooie::hex32(input_address(s["rom_end"]))},
             {"output",tooie::hex32(out)},{"expected_output",tooie::hex32(s["next_output"])},{"length",length},
             {"crc_actual",crc},{"crc_expected",s["crc"]},{"first_mismatch",first},{"guards_preserved",continuous_mode?Json(nullptr):Json(guard_check(rdram))}});
        require(ok,"Generated decompression diverged at stream "+std::to_string(stream_index));
        stream_index++;return;
    }
    require(stage==3&&stream_index==2,"Core1 readiness order mismatch");
    Json first=mismatch(rdram,reference["core1"]["vram"],reference["core1"]["size"]);
    Json crcs=Json::array();for(unsigned i=0;i<4;i++)crcs.push_back(word(rdram,0x80012000+i*4));
    Json core2={word(rdram,0x80012010),word(rdram,0x80012014)};
    uint32_t begin=reference["symbols"]["core1_compressed_ROM_START"],end=reference["symbols"]["core1_compressed_ROM_END"],scratch=reference["scratch_base"];
    bool intact=true;auto rom=recomp::get_rom();for(uint32_t i=0;i<end-begin;i++)intact&=(uint8_t)MEM_B(i,guest(scratch))==rom[begin+i];
    bool ok=first.is_null()&&crcs==reference["checksum_words"]&&core2==reference["core2_range"]&&guard_check(rdram)&&intact;
    tooie::trace("core1_ready","E",ok?"pass":"failure",0x80000518,ctx,
        {{"bytes_match",first.is_null()},{"bytes_checked",expected.size()},{"first_mismatch",first},
         {"checksums_actual",crcs},{"checksums_expected",reference["checksum_words"]},
         {"core2_actual",core2},{"core2_expected",reference["core2_range"]},
         {"guards_preserved",continuous_mode?Json(nullptr):Json(guard_check(rdram))},{"compressed_source_preserved",intact},{"core1_sha256",reference["core1"]["sha256"]},
         {"original_func_80000450_executed",true},{"original_func_80012030_executed",false}});
    require(ok,"Core1 readiness validation failed");
    load_overlays(reference["core1"]["rom"],(int32_t)reference["core1"]["vram"].get<uint32_t>(),reference["core1"]["size"]);
    bool mapping=get_function((int32_t)0x80012030)==func_80012030;
    tooie::trace("core1_registered","E",mapping?"pass":"failure",0x80000518,ctx,{{"real_entry_mapping_verified",mapping},{"registry",tooie::registry_snapshot()}});
    require(mapping,"Runtime core1 mapping mismatch");
    if(continuous_mode) {
        tooie::continuous_core1_ready(rdram,ctx);
    } else if(idle_mode) {
        tooie::prepare_thread_gate(rdram,main_mode);
        installed=true;
        require(get_function((int32_t)0x80012030)==func_80012030,"Original core1 entry mapping changed");
    } else {
        recomp::overlays::add_loaded_function((int32_t)0x80012030,core_boundary);installed=true;
        require(get_function((int32_t)0x80012030)==core_boundary,"Diagnostic core1 mapping failed");
    }
}
