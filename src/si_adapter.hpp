#pragma once
#include "recomp.h"
#include <array>
#include <cstdint>
#include <span>

namespace tooie::si {
using Challenge = std::array<uint8_t, 15>;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
struct PersistentState {
    std::uint32_t version=1;
    std::array<std::uint8_t,64> pif{};
    bool response_available=false;
    std::uint64_t transfer_sequence=0,challenge_sequence=0;
    Challenge original_challenge{};
};
PersistentState persistent_export();
void persistent_import(const PersistentState&);
#endif
// High nibble then low nibble; the final PIF control byte is not challenge data.
Challenge cic6105_response(const Challenge& challenge);

enum class Phase { DmaTransferred, CompletionCall, CompletionReturned, CompletionThrew };
struct Observation {
    Phase phase = Phase::DmaTransferred;
    uint64_t transfer_id = 0, challenge_id = 0;
    uint32_t direction = 0, guest_address = 0, physical_address = 0, ra = 0, sp = 0;
    Challenge challenge{}; // Original write challenge, retained across reads.
    std::array<uint8_t, 64> payload{}; // Actual canonical bytes of this DMA.
};
using Observer = void (*)(const Observation&);
// Configure only before workers start/after they join; nullptr disables tracing.
// Callback receives a detached read-only snapshot, never mutable guest/device data.
// Observations prove neither guest receive nor successful internal queue allocation.
// Exceptions from the observer are contained/counted and never suppress completion.
void configure_observer(Observer observer) noexcept;
uint64_t observer_failures() noexcept;
const char* phase_name(Phase phase) noexcept;

class Device {
public:
    // Native 64-byte SI DMA. Invalid/unsupported transactions throw before commit.
    // direction: 0 OS_READ (PIF -> RDRAM), 1 OS_WRITE (RDRAM -> PIF).
    void transfer(std::span<uint8_t> rdram, uint32_t direction, uint32_t address);
    void reset();
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    PersistentState persistent_export() const;
    void persistent_import(const PersistentState&);
#endif
private:
    std::array<uint8_t, 64> pif_{};
    bool response_available_ = false;
};
// Call only before guest workers start; defaults to Tooie's 8 MiB Expansion Pak RAM.
void reset(size_t rdram_size = 8 * 1024 * 1024);
}

extern "C" {
void tooie_si_get_access(uint8_t*, recomp_context*);
void tooie_si_raw_start_dma(uint8_t*, recomp_context*);
void tooie_si_rel_access(uint8_t*, recomp_context*);
}
