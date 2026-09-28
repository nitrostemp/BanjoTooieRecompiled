#include "overlay_validation.hpp"
#include "game.hpp"
#include "platform_support.hpp"
#include "json/json.hpp"
#include "miniz.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
namespace {
using Json=nlohmann::json;
struct Reloc {uint32_t offset,kind,target;};
struct Reference {
    uint32_t id,canonical,text_offset,text_bytes,bss_bytes,entries;
    uint16_t key;
    std::string name;
    std::vector<uint8_t> prefix,image;
    std::vector<Reloc> relocations;
    std::array<uint32_t,4> counts{};
};
std::unordered_map<uint32_t,Reference> references;
void (*callback)(const tooie::overlay_validation::Report&)=nullptr;
void check(bool value,const std::string& detail) {if(!value)throw std::runtime_error("Overlay validation: "+detail);}
std::string hex(uint32_t value) {std::ostringstream out;out<<"0x"<<std::hex<<value;return out.str();}
uint32_t word(const std::vector<uint8_t>& data,size_t offset) {
    check(offset+4<=data.size(),"reference word out of range");
    return uint32_t(data[offset])<<24|uint32_t(data[offset+1])<<16|uint32_t(data[offset+2])<<8|data[offset+3];
}
void put(std::vector<uint8_t>& data,size_t offset,uint32_t value) {
    check(offset+4<=data.size(),"reference relocation out of range");
    for(unsigned i=0;i<4;++i)data[offset+i]=uint8_t(value>>(24-8*i));
}
uint16_t half(const std::vector<uint8_t>& data,size_t offset) {
    check(offset+2<=data.size(),"reference halfword out of range");
    return uint16_t(data[offset])<<8|data[offset+1];
}
std::vector<uint8_t> derive_image(const Json& row,std::span<const uint8_t> rom,Reference& ref) {
    const uint32_t start=row.at("rom_start"),end=row.at("rom_end");
    check(start+16<=end&&end<=rom.size(),"selected-ROM overlay span invalid");
    std::vector<uint8_t> encoded(rom.begin()+start,rom.begin()+end);
    check(tooie::platform::digest(encoded,true)==row.at("raw_source_sha256").get<std::string>(),"selected-ROM overlay span hash mismatch");
    std::vector<uint8_t> image(encoded.begin(),encoded.begin()+16);
    if(image[15]&0x80) {
        check(encoded.size()>=18,"compressed overlay header truncated");
        const size_t output_bytes=half(encoded,16)*16u;
        std::vector<uint8_t> body(output_bytes);
        const size_t written=tinfl_decompress_mem_to_mem(body.data(),body.size(),encoded.data()+18,encoded.size()-18,0);
        check(written==body.size(),"selected-ROM overlay inflate failed");
        uint32_t a=0,b=0;
        for(uint8_t value:body){a+=value;b^=uint32_t(value)<<(a&0x17);}
        check(row.at("crc")==Json::array({a,b}),"selected-ROM overlay checksum mismatch");
        put(image,0,word(image,0)^a);put(image,8,word(image,8)^b);
        image.insert(image.end(),body.begin(),body.end());
    } else image.insert(image.end(),encoded.begin()+16,encoded.end());
    check(ref.text_offset<=image.size()&&uint64_t(ref.text_offset)+row.at("initialized_bytes").get<uint32_t>()<=image.size(),
        "selected-ROM overlay image dimensions invalid");
    ref.prefix.assign(image.begin(),image.begin()+ref.text_offset);
    std::vector<uint8_t> original(image.begin()+ref.text_offset,
        image.begin()+ref.text_offset+row.at("initialized_bytes").get<uint32_t>());
    check(tooie::platform::digest(ref.prefix,true)==row.at("prefix_sha256").get<std::string>(),"derived overlay prefix hash mismatch");
    check(tooie::platform::digest(original,true)==row.at("original_image_sha256").get<std::string>(),"derived overlay image hash mismatch");
    const uint32_t records=(0x38+ref.entries*4+ref.prefix.at(14)+3)&~3u;
    check(uint64_t(records)+row.at("packed_count").get<uint32_t>()*2<=ref.prefix.size(),"packed relocation table outside prefix");
    auto canonical=original;
    for(uint32_t i=0;i<row.at("packed_count").get<uint32_t>();++i) {
        const uint16_t packed=half(ref.prefix,records+i*2)^ref.key;
        const uint32_t offset=packed&~3u,kind=packed&3u;
        uint32_t value=word(canonical,offset);
        if(kind==0)value|=ref.canonical;
        else if(kind==1)value|=(ref.canonical&0x0fffffffu)>>2;
        else if(kind==2)value|=ref.canonical>>16;
        put(canonical,offset,value);
    }
    check(tooie::platform::digest(canonical,true)==row.at("canonical_image_sha256").get<std::string>(),"derived canonical overlay hash mismatch");
    return canonical;
}
uint8_t byte(const uint8_t* ram,uint32_t address) {
    if(address<0x80000400||address>=0x80800000)check(false,"guest read outside RDRAM at "+hex(address));
    return ram[(address&0x1fffffff)^3];
}
uint16_t half(const uint8_t* ram,uint32_t address) {return uint16_t(byte(ram,address))<<8|byte(ram,address+1);}
void compare(const uint8_t* ram,uint32_t start,const std::vector<uint8_t>& bytes,const Reference& ref,const char* region) {
    for(size_t i=0;i<bytes.size();++i) {
        uint8_t actual=byte(ram,start+static_cast<uint32_t>(i));
        if(actual!=bytes[i])check(false,"id="+std::to_string(ref.id)+" "+region+" address="+hex(start+static_cast<uint32_t>(i))+
            " expected="+hex(bytes[i])+" actual="+hex(actual));
    }
}
}
namespace tooie::overlay_validation {
void load_references(const std::filesystem::path& path,std::string_view actual_rom_sha256,void (*on_validation)(const Report&)) {
    std::ifstream input(path);check(bool(input),"cannot open reference manifest");
    std::ostringstream bytes;bytes<<input.rdbuf();
    check(!input.bad(),"cannot read reference manifest");
    load_reference_bytes(bytes.str(),actual_rom_sha256,on_validation);
}
void load_reference_bytes(std::string_view bytes,std::string_view actual_rom_sha256,void (*on_validation)(const Report&)) {
    Json doc=Json::parse(bytes);
    check(doc.at("schema")==2,"unsupported reference schema");
    check(doc.at("source_rom_sha256").get<std::string>()==actual_rom_sha256,"reference ROM identity differs from installed ROM");
    const auto rom=recomp::get_rom();
    check(tooie::platform::digest(rom,true)==actual_rom_sha256,"installed ROM bytes differ from validated identity");
    std::unordered_map<uint32_t,Reference> fresh;
    for(const auto& row:doc.at("overlays")) {
        Reference ref{};ref.id=row.at("id");ref.name=row.at("name");ref.canonical=row.at("canonical_base");
        ref.text_offset=row.at("text_offset");ref.text_bytes=row.at("text_bytes");ref.bss_bytes=row.at("bss_bytes");
        ref.entries=row.at("entries");ref.key=row.at("original_key");
        ref.image=derive_image(row,rom,ref);
        check(ref.id>0&&ref.id<885&&ref.canonical==0x80800000&&ref.prefix.size()==ref.text_offset&&
              ref.text_offset>=0x38+ref.entries*4&&ref.text_offset%16==0&&ref.image.size()%16==0&&
              ref.image.size()+ref.prefix.size()+ref.bss_bytes<8*1024*1024,"invalid reference dimensions");
        check(row.at("secondary_count")==0,"selected secondary relocations require independent validation support");
        for(const auto& rel:row.at("relocations")) {
            Reloc r{rel.at("offset"),rel.at("kind"),rel.at("target")};
            check(r.kind<4&&r.offset%4==0&&uint64_t(r.offset)+4<=ref.image.size(),"invalid reference relocation");
            ++ref.counts[r.kind];ref.relocations.push_back(r);
        }
        check(ref.relocations.size()==row.at("packed_count").get<size_t>(),"reference packed count mismatch");
        check(fresh.emplace(ref.id,std::move(ref)).second,"duplicate stable ID reference");
    }
    references=std::move(fresh);callback=on_validation;
}
Report validate_first_load(const uint8_t* ram,uint32_t header) {
    check(ram!=nullptr&&header%16==0,"invalid header or RDRAM pointer");
    uint32_t id=half(ram,header+0x2c);auto found=references.find(id);
    check(found!=references.end(),"no independent reference for stable ID "+std::to_string(id));
    const auto& ref=found->second;
    check(uint64_t(header)+ref.text_offset+ref.image.size()+ref.bss_bytes<=0x80800000,"live extent exceeds RDRAM");
    uint32_t text=header+ref.text_offset,delta=text-ref.canonical;
    auto expected=ref.image;
    for(const auto& rel:ref.relocations) {
        uint32_t target=rel.target+delta,value=word(expected,rel.offset);
        switch(rel.kind) {
            case 0:value=target;break;
            case 1:value=(value&0xfc000000)|((target>>2)&0x03ffffff);break;
            case 2:value=(value&0xffff0000)|(((target+0x8000)>>16)&0xffff);break;
            case 3:value=(value&0xffff0000)|(target&0xffff);break;
        }
        put(expected,rel.offset,value);
    }
    compare(ram,text,expected,ref,"initialized image");
    auto prefix=ref.prefix;
    prefix[0x2c]=static_cast<uint8_t>(id>>8);prefix[0x2d]=static_cast<uint8_t>(id);
    for(uint32_t i=0;i<ref.entries;++i)put(prefix,0x38+i*4,word(prefix,0x38+i*4)+text);
    put(prefix,0x34,text);
    // Original loader/dispatcher assigns ID, syscall-group index and loaded-
    // list index. ID is checked above; age has not yet been changed by a thunk.
    // Only the two dynamic indexes lack an independent value at this seam.
    uint32_t checked_metadata=0;
    for(uint32_t i=0;i<prefix.size();++i) {
        if(i>=0x2e&&i<0x32)continue;
        auto actual=byte(ram,header+i);
        if(actual!=prefix[i])check(false,"id="+std::to_string(id)+" metadata address="+hex(header+i)+" expected="+hex(prefix[i])+" actual="+hex(actual));
        ++checked_metadata;
    }
    for(uint32_t i=0;i<ref.bss_bytes;++i)
        if(byte(ram,text+static_cast<uint32_t>(expected.size())+i)!=0)check(false,"id="+std::to_string(id)+" first-load BSS not zero");
    return {id,header,text,static_cast<uint32_t>(expected.size()),ref.bss_bytes,checked_metadata,ref.counts,ref.key,ref.name};
}
}
extern "C" void tooie_validate_overlay(uint8_t* ram,uint32_t header,int32_t delta) {
    if(delta)return; // Dirty state on movement is not compared to a fresh image.
    auto report=tooie::overlay_validation::validate_first_load(ram,header);
    if(callback)callback(report);
}
