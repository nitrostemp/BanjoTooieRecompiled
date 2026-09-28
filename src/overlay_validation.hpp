#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
namespace tooie::overlay_validation {
struct Report {
    uint32_t id,header,text,initialized_bytes,bss_bytes,metadata_bytes;
    std::array<uint32_t,4> relocation_counts; // 32,26,HI16,LO16
    uint16_t original_key;
    std::string name;
};
void load_reference_bytes(std::string_view bytes,std::string_view actual_rom_sha256,
    void (*on_validation)(const Report&)=nullptr);
// The distributable reference contains only ranges, relocation metadata, and
// hashes. Comparison bytes are independently inflated from the already
// validated user ROM in memory; no API copies them into guest RAM.
void load_references(const std::filesystem::path&,std::string_view actual_rom_sha256,
                     void (*on_validation)(const Report&)=nullptr);
Report validate_first_load(const uint8_t* rdram,uint32_t header);
}
// Place after original relocation and before original heap-thunk construction.
// A move is not a fresh load: this hook deliberately validates only delta==0.
extern "C" void tooie_validate_overlay(uint8_t*,uint32_t header,int32_t delta);
