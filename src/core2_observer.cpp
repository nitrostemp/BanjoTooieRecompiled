#include "game.hpp"
#include "continuous_host.hpp"
#include "build_metadata.hpp"
#include "platform_support.hpp"
#include "librecomp/overlays.hpp"
#include "miniz.h"
#include <stdexcept>
#include <vector>

extern "C" void func_800815CC(uint8_t*,recomp_context*);
namespace {
tooie::Json metadata;
std::vector<uint8_t> expected;
bool loading=false,ready=false;
unsigned next_stage=0;
uint32_t scratch=0;

gpr guest(uint32_t a){return (gpr)(int32_t)a;}
uint32_t word(uint8_t* rdram,uint32_t a){return MEM_W(0,guest(a));}
void require(bool good,const char* message){if(!good)throw std::runtime_error(message);}

std::vector<uint8_t> inflate_stream(std::span<const uint8_t> rom,const tooie::Json& stream) {
    const uint32_t start=stream.at("rom_start"),end=stream.at("rom_end"),size=stream.at("output_bytes");
    require(start+2<=end&&end<=rom.size(),"Core2 metadata ROM range invalid");
    require((uint32_t(rom[start])<<8|rom[start+1])*16u==size,"Core2 stream size header mismatch");
    std::vector<uint8_t> output(size);
    const size_t written=tinfl_decompress_mem_to_mem(output.data(),output.size(),rom.data()+start+2,end-start-2,0);
    require(written==output.size(),"Core2 selected-ROM host inflate failed");
    uint32_t a=0,b=0;
    for(uint8_t value:output){a+=value;b^=uint32_t(value)<<(a&0x17);}
    require(stream.at("crc")==tooie::Json::array({a,b}),"Core2 selected-ROM checksum mismatch");
    return output;
}

void prepare_expected_from_selected_rom() {
    const auto& bundle=tooie::build_metadata::current();
    metadata=tooie::Json::parse(bundle.text(tooie::build_metadata::File::Core2Reference));
    const auto rom=recomp::get_rom();
    require(metadata.at("rom_sha256")==tooie::platform::digest(rom,true),"Core2 metadata ROM identity mismatch");
    expected.clear();
    for(const auto& stream:metadata.at("streams")) {
        auto bytes=inflate_stream(rom,stream);
        expected.insert(expected.end(),bytes.begin(),bytes.end());
    }
    require(expected.size()==metadata.at("core2").at("size").get<size_t>(),"Core2 derived size mismatch");
    require(tooie::platform::digest(expected,true)==metadata.at("core2").at("sha256").get<std::string>(),
        "Core2 derived image SHA256 mismatch");
}

void compare(uint8_t* rdram,uint32_t address,uint32_t size) {
    const uint32_t base=metadata.at("core2").at("vram");
    require(address>=base&&uint64_t(address-base)+size<=expected.size(),"Core2 comparison range invalid");
    for(uint32_t i=0;i<size;++i) if(uint8_t(MEM_B(i,guest(address)))!=expected[address-base+i]) {
        tooie::trace("core2_byte_mismatch","G2","failure",address+i,nullptr,
            {{"expected",expected[address-base+i]},{"actual",uint8_t(MEM_B(i,guest(address)))}});
        throw std::runtime_error("Original core2 decompression differs from selected-ROM host inflate");
    }
}
}

namespace tooie { bool core2_image_ready(){return ready;} }
extern "C" void tooie_core2_hook(uint8_t* rdram,recomp_context* ctx,unsigned stage) {
    if(!tooie::continuous_enabled())return;
    if(stage==0) {
        if(ctx->r4!=0)return;
        require(!loading&&!ready,"Unexpected repeated core2 initial load");
        prepare_expected_from_selected_rom();
        require(uint32_t(ctx->r5)==metadata.at("core2").at("vram").get<uint32_t>(),"Core2 load destination mismatch");
        require(word(rdram,0x80012010)==metadata.at("compressed_start").get<uint32_t>()&&
            word(rdram,0x80012014)==metadata.at("compressed_end").get<uint32_t>(),"Boot core2 original-ROM range mismatch");
        loading=true;next_stage=1;
        tooie::trace("core2_load_begin","G2","entered",0x80019EC0,ctx,
            {{"linked_decompressed_rom_argument",tooie::hex32(ctx->r7)},
             {"actual_original_rom_start",tooie::hex32(word(rdram,0x80012010))},
             {"actual_original_rom_end",tooie::hex32(word(rdram,0x80012014))},
             {"comparison_bytes",expected.size()},
             {"comparison_source","independent host inflate of selected validated ROM"}});
        return;
    }
    if(!loading)return;
    require(stage==next_stage++,"Core2 loader observation order mismatch");
    const uint32_t sp=ctx->r29;
    if(stage==1) {
        scratch=word(rdram,sp+0x34);
        const uint32_t start=metadata.at("compressed_start"),end=metadata.at("compressed_end");
        const auto rom=recomp::get_rom();
        require(scratch>=0x80000000u&&uint64_t(scratch)+(end-start)<=0x80800000ull,"Core2 compressed destination out of RDRAM");
        for(uint32_t i=0;i<end-start;++i)require(uint8_t(MEM_B(i,guest(scratch)))==rom[start+i],"Core2 DMA differs from selected ROM");
        tooie::trace("core2_dma_completed","G2","pass",0x80019F70,ctx,
            {{"rom_start",tooie::hex32(start)},{"rom_end",tooie::hex32(end)},
             {"destination",tooie::hex32(scratch)},{"bytes",end-start},{"original_blocking_dma_returned",true}});
    } else if(stage==2||stage==3) {
        const auto& stream=metadata.at("streams").at(stage-2);
        const uint32_t address=stream.at("output_start"),size=stream.at("output_bytes");
        compare(rdram,address,size);
        require(word(rdram,sp+0x3C)==((address+size+15)&~15u),"Core2 decompressor output pointer mismatch");
        require(word(rdram,sp+0x34)==scratch+stream.at("rom_end").get<uint32_t>()-
            metadata.at("compressed_start").get<uint32_t>(),"Core2 decompressor consumed pointer mismatch");
        tooie::trace("core2_stream_verified","G2","pass",stage==2?0x80019F7C:0x80019F9C,ctx,
            {{"stream",stage-2},{"address",tooie::hex32(address)},{"bytes",size},{"bytes_match",true},
             {"source","selected validated ROM"}});
    } else if(stage==4) {
        const uint32_t begin=metadata.at("core2").at("bss_start"),end=metadata.at("core2").at("bss_end");
        tooie::Json crcs=tooie::Json::array();
        for(unsigned i=0;i<4;++i)crcs.push_back(word(rdram,begin+4*i));
        require(crcs==metadata.at("checksums"),"Core2 checksum words mismatch");
        for(uint32_t a=begin+16;a<end;++a)require(MEM_B(0,guest(a))==0,"Core2 BSS not zero after loader");
        const uint32_t vram=metadata.at("core2").at("vram"),rom=metadata.at("core2").at("rom"),size=metadata.at("core2").at("size");
        compare(rdram,vram,size);
        load_overlays(rom,int32_t(vram),size);
        require(get_function(int32_t(0x800815CC))==func_800815CC,"Core2 native mapping missing after readiness");
        loading=false;ready=true;
        tooie::capture_guest_state(rdram,ctx);
        tooie::trace("core2_ready_registered","G2","pass",0x80019FF0,ctx,
            {{"image_bytes",size},{"image_sha256",metadata.at("core2").at("sha256")},
             {"image_source","selected validated ROM; independent host inflate compared with guest output"},
             {"bss_start",tooie::hex32(begin)},{"bss_end",tooie::hex32(end)},{"bss_zero_bytes",end-begin-16},
             {"checksum_words",crcs},{"native_mapping_published_after_validation",true}});
    } else throw std::runtime_error("Unknown core2 observation stage");
}
