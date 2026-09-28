// Original-ROM-derived input is installed ONLY into isolated test memory.
// Production validator exposes no guest-memory writing interface.
#include "overlay_validation.hpp"
#include "recomp.h"
#include "json/json.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
extern "C" void ovl_relocate(uint8_t*,recomp_context*);
namespace {
using Json=nlohmann::json;
void check(bool value,const char* detail){if(!value)throw std::runtime_error(detail);}
std::vector<uint8_t> bytes(const std::string& hex) {
    std::vector<uint8_t> out;
    for(size_t i=0;i<hex.size();i+=2)out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i,2),nullptr,16)));
    return out;
}
void put_byte(std::vector<uint8_t>& ram,uint32_t address,uint8_t value){ram.at((address&0x1fffffff)^3)=value;}
void put_word(std::vector<uint8_t>& ram,uint32_t address,uint32_t value){for(unsigned i=0;i<4;++i)put_byte(ram,address+i,value>>(24-i*8));}
uint32_t word(const std::vector<uint8_t>& data,size_t offset){return uint32_t(data.at(offset))<<24|uint32_t(data.at(offset+1))<<16|uint32_t(data.at(offset+2))<<8|data.at(offset+3);}
gpr guest(uint32_t value){return static_cast<gpr>(static_cast<int32_t>(value));}
template<class F>void rejects(F call){bool caught=false;try{call();}catch(const std::exception&){caught=true;}check(caught,"Corruption was accepted");}
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"Expected reference manifest path");
        Json doc;std::ifstream input(argv[1]);input>>doc;
        tooie::overlay_validation::load_references(argv[1],doc.at("source_rom_sha256").get<std::string>());
        rejects([&]{tooie::overlay_validation::load_references(argv[1],"wrong ROM identity");});
        unsigned tested=0,pointers=0;
        for(const auto& ref:doc.at("overlays")) {
            auto prefix=bytes(ref.at("prefix_hex")),raw=bytes(ref.at("original_image_hex"));
            uint32_t id=ref.at("id"),offset=ref.at("text_offset"),entries=ref.at("entries");
            uint32_t packed_count=ref.at("packed_count");
            uint32_t records=(0x38+entries*4+prefix.at(14)+3)&~3u;
            for(uint32_t text:{0x8038fff0u,0x803afff0u}) {
                std::vector<uint8_t> ram(8*1024*1024);uint32_t header=text-offset;
                for(uint32_t i=0;i<prefix.size();++i)put_byte(ram,header+i,prefix[i]);
                for(uint32_t i=0;i<raw.size();++i)put_byte(ram,text+i,raw[i]);
                put_byte(ram,header+0x2c,id>>8);put_byte(ram,header+0x2d,id);
                put_word(ram,header+0x34,text);
                for(uint32_t i=0;i<entries;++i)put_word(ram,header+0x38+i*4,word(prefix,0x38+i*4)+text);
                recomp_context ctx{};ctx.f_odd=&ctx.f0.u32h;
                ctx.r8=guest(text);ctx.r9=guest(text);ctx.r10=guest(header+records);ctx.r11=packed_count;ctx.r21=ref.at("original_key").get<uint16_t>();
                if(packed_count)ovl_relocate(ram.data(),&ctx);
                auto before=ram;
                auto report=tooie::overlay_validation::validate_first_load(ram.data(),header);
                check(ram==before,"Read-only validator modified guest memory");
                check(report.id==id&&report.text==text&&report.initialized_bytes==raw.size(),"Report extent mismatch");
                pointers+=report.relocation_counts[0];++tested;
                for(const auto& rel:ref.at("relocations")) {
                    if(rel.at("kind")!=0)continue;
                    uint32_t address=text+rel.at("offset").get<uint32_t>();
                    ram.at((address&0x1fffffff)^3)^=1;
                    rejects([&]{tooie::overlay_validation::validate_first_load(ram.data(),header);});
                    ram=before;
                }
                ram.at((text&0x1fffffff)^3)^=1;
                rejects([&]{tooie::overlay_validation::validate_first_load(ram.data(),header);});ram=before;
                put_byte(ram,header+14,prefix[14]^1);
                rejects([&]{tooie::overlay_validation::validate_first_load(ram.data(),header);});ram=before;
                if(report.bss_bytes){put_byte(ram,text+raw.size(),1);rejects([&]{tooie::overlay_validation::validate_first_load(ram.data(),header);});}
                std::cout<<"PASS id="<<id<<" text="<<std::hex<<text<<std::dec<<" bytes="<<report.initialized_bytes<<" R_MIPS_32="<<report.relocation_counts[0]<<'\n';
            }
        }
        check(tested>=6&&pointers>=24,"Required representative overlay coverage missing");
        std::cout<<"PASS read-only live-overlay validator: "<<tested<<" original-ROM-derived relocated image cases; "<<pointers<<" R_MIPS_32 mutation rejections; opcode/header/BSS/ROM-identity failures\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
