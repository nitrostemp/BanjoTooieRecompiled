#pragma once

#include <cstddef>
#include <cstdint>
#include "recomp.h"

namespace tooie::practice::travel {

struct Destination {
    const char* world;
    const char* area;
    std::uint16_t map_id;
    std::uint8_t entrance_id;
    std::uint8_t pad_number;
    bool silo = false;
};

enum class Outcome : std::uint8_t {
    None, Queued, TransitionRequested, Arrived, Rejected, TimedOut,
};

struct Status {
    Outcome outcome = Outcome::None;
    std::size_t destination_index = 0;
    std::uint64_t request_number = 0;
};

std::size_t destination_count() noexcept;
const Destination* destination(std::size_t index) noexcept;
bool request(std::size_t index) noexcept;
Status status() noexcept;
void tick(std::uint8_t* rdram, recomp_context* ctx);
void reset() noexcept;

} // namespace tooie::practice::travel
