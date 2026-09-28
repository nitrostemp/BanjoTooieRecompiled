#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string_view>

namespace tooie::overlay_calls {
inline constexpr std::size_t capacity=1024,stable_ids=885;
enum class Operation:uint8_t {entry,returned};
struct Record {
    uint64_t ordinal=0,host_monotonic_ns=0,generation=0;
    uint32_t id=0,header=0,text=0,entry=0,raw_ra=0,observed_callsite=0;
    Operation operation=Operation::entry;
};
struct Counts {uint64_t entries=0,returns=0;};
struct Snapshot {
    bool enabled=false,invalid=false;
    uint64_t total=0,entries=0,returns=0,overwritten=0;
    std::size_t retained=0;
    std::array<Counts,stable_ids> by_id{};
    std::array<Record,capacity> history{}; // Chronological retained order.
};
inline bool ordinary(const char* value) noexcept {
    return value&&(std::string_view(value)=="entry"||std::string_view(value)=="return");
}
inline bool increment_overflows(uint64_t value) noexcept {return value==std::numeric_limits<uint64_t>::max();}
class Recorder {
public:
    // A lower ceiling enables deterministic saturation fixtures; production uses UINT64_MAX.
    explicit Recorder(uint64_t ceiling=std::numeric_limits<uint64_t>::max()):ceiling_(ceiling){}
    void reset(bool enabled); // Before producers start; never while producers are running.
    bool enabled() const noexcept {return enabled_.load(std::memory_order_relaxed);}
    bool record(Record value) noexcept; // True means suppress this ordinary JSON event.
    Snapshot snapshot() const; // Bounded copy only while holding the recorder mutex.
    void check_health() const;
private:
    mutable std::mutex mutex_;
    std::atomic_bool enabled_{false},invalid_{false};
    uint64_t ceiling_,total_=0,entries_=0,returns_=0;
    std::array<Counts,stable_ids> by_id_{};
    std::array<Record,capacity> ring_{};
};
}
