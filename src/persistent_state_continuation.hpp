#pragma once

// Experimental generated-execution vertical slice, NOT the game runtime API.
// No graphics, scheduler, overlays, EEPROM or personal profiles are attached.
#include "recomp.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace tooie::continuation {
struct Frame {
    std::uint32_t function=0, pc=0;
    std::uint64_t hi=0, lo=0, result=0;
    std::int32_t c1cs=0;
    // Explicit HLE operands/result for the bounded receive-stage experiment.
    std::array<std::uint64_t,4> operands{};
    std::vector<std::uint64_t> extra_locals;
    bool operator==(const Frame&) const=default;
};
struct Identity {
    std::string program, executable, input, expected_completion;
};
struct Function {
    std::uint32_t id;
    recomp_func_t* entry;
    std::span<const std::uint32_t> allowed_pcs;
    std::uint32_t local_words=4;
};
class Machine {
public:
    static constexpr std::size_t memory_bytes=0x800000;
    std::vector<std::uint8_t> memory;
    recomp_context context{};
    std::vector<Frame> frames;
    Identity identity;
    explicit Machine(std::span<const Function> functions,bool tracking_only=false);
    Machine(const Machine&)=delete;
    Machine& operator=(const Machine&)=delete;
    void start(std::uint32_t function,std::uint32_t yield_pc=0);
    void resume(std::uint32_t yield_pc=0);
    bool suspended() const noexcept { return yielded_; }
    std::string completion_hash() const;
    void save(const std::filesystem::path& path) const;
    void load(const std::filesystem::path& path,const Identity& expected);

    // Called only by mechanically lifted generated functions while bound.
    Frame& enter(std::uint32_t function);
    Frame& top();
    void leave(std::uint32_t function);
    bool checkpoint(std::uint32_t pc);
    static Machine& current();
    static Machine* current_if_bound() noexcept;
    recomp_func_t* validate_function_pointer(recomp_func_t* entry) const;
    // Admission/coverage mode for the actual guest workers. It never captures,
    // dispatches, loads or saves. Native HLE stacks remain owned by the runtime.
    void bind_tracking(std::uint8_t* rdram,recomp_context* guest_context);
    void unbind_tracking() noexcept;
    bool tracking_only() const noexcept { return tracking_only_; }
    void native_dependency(const char* symbol);
    void validate_snapshot_frames() const {validate_frames(frames);}
    void validate_snapshot_frames(std::span<const Frame> values) const {validate_frames(values);}
    // Only the all-owner scheduler transaction can install these borrowed
    // images. Ordinary isolated save/load APIs remain forbidden in live mode.
    void resume_borrowed(std::span<const Frame> values,const recomp_context& saved);
    std::uint64_t entries=0,native_dependencies=0,indirect_native_targets=0;
    std::size_t maximum_depth=0;
private:
    std::span<const Function> functions_;
    std::unordered_map<std::uint32_t,const Function*> functions_by_id_;
    std::unordered_set<recomp_func_t*> function_pointers_;
    bool yielded_=false, dispatching_=false;
    std::uint32_t yield_pc_=0;
    bool tracking_only_=false;
    std::uint8_t* borrowed_rdram_=nullptr;
    recomp_context* borrowed_context_=nullptr;
    const Function& function(std::uint32_t id) const;
    void validate_frames(std::span<const Frame> frames) const;
    void dispatch(std::uint32_t id,bool resume);
};
}

extern "C" {
std::uint32_t tooie_continuation_enter(std::uint32_t function,
    std::uint64_t* hi,std::uint64_t* lo,std::uint64_t* result,int* c1cs);
void tooie_continuation_store(std::uint32_t pc,
    std::uint64_t hi,std::uint64_t lo,std::uint64_t result,int c1cs);
void tooie_continuation_leave(std::uint32_t function);
int tooie_continuation_checkpoint(std::uint32_t pc);
int tooie_continuation_yielded();
}
