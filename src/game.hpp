#pragma once
#include "librecomp/game.hpp"
#include "trace.hpp"
namespace tooie {
struct RomIdentity {std::string sha1,sha256; uint64_t xxh3; size_t size;};
RomIdentity validate_and_install_rom(const std::filesystem::path& path);
std::string file_sha256(const std::filesystem::path& path);
recomp::GameEntry game_entry(uint64_t rom_hash, bool idle=false);
void register_overlays();
void retire_boot_mappings();
void checked_load_overlay(uint32_t id,uint32_t text_base);
void checked_move_overlay(uint32_t id,int32_t delta);
void checked_unload_overlay(uint32_t id);
size_t registered_function_count();
size_t registered_section_index(uint32_t id);
Json registry_snapshot();
bool run_boot_diagnostic(bool core1=false, bool idle=false, bool main=false);
[[noreturn]] void unsupported(const char* symbol,recomp_context* ctx,uint32_t pc=0);
}
