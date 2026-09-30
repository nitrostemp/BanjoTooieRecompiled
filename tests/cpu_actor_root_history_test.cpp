#include "cpu_actor_root_history.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

int main() {
    using tooie::model_interpolation::CpuActorRootHistory;
    using tooie::model_interpolation::CpuActorRoot;
    using tooie::model_interpolation::CpuActorRootKey;
    const CpuActorRootKey a{0x80100000U, 0x6001U, 7};
    const CpuActorRootKey b{0x80100040U, 0x6001U, 7};
    const CpuActorRootKey c{0x80100080U, 0x6001U, 7};
    const CpuActorRootKey next_epoch{0x80100000U, 0x6001U, 8};
    CpuActorRootHistory history;
    std::array<std::uint32_t, 2> ids{};
    const std::array first{CpuActorRoot{a, 0, 0, 0}, CpuActorRoot{b, 100, 0, 0}};
    assert(history.assign(first, ids));
    const auto id_a = ids[0], id_b = ids[1];
    assert(id_a == 0x60000000U && id_b == id_a + 1);
    const std::array crossed{CpuActorRoot{b, 40, 0, 0}, CpuActorRoot{a, 60, 0, 0}};
    assert(history.assign(crossed, ids));
    assert(ids[0] == id_b && ids[1] == id_a); // Same model, crossing positions.
    const std::array one_a{CpuActorRoot{a, 65, 0, 0}};
    assert(history.assign(one_a, std::span(ids).first(1)));
    assert(ids[0] == id_a);
    const std::array teleported{CpuActorRoot{a, 322, 0, 0}};
    assert(history.assign(teleported, std::span(ids).first(1)));
    assert(ids[0] != id_a && ids[0] != 0); // 257-unit discontinuity.
    const auto after_teleport = ids[0];
    const std::array near{CpuActorRoot{a, 66, 0, 0}};
    assert(history.assign(near, std::span(ids).first(1)));
    assert(ids[0] == after_teleport); // Exactly 256 units is accepted.
    const auto boundary_id = ids[0];
    const std::array edge{CpuActorRoot{a, 322, 0, 0}};
    assert(history.assign(edge, std::span(ids).first(1)));
    assert(ids[0] == boundary_id);

    assert(history.assign(std::span<const CpuActorRoot>{}, std::span<std::uint32_t>{}));
    assert(history.assign(edge, std::span(ids).first(1)));
    assert(ids[0] != boundary_id && ids[0] != 0); // Empty task breaks history.
    const auto before_reset = ids[0];
    history.reset();
    assert(history.assign(edge, std::span(ids).first(1)));
    assert(ids[0] != before_reset && ids[0] != 0); // Reset never recycles an ID.
    assert(history.assign(std::array{CpuActorRoot{next_epoch, 322, 0, 0}},
        std::span(ids).first(1)));
    assert(ids[0] != before_reset); // Epoch is part of identity.

    auto changed_source = next_epoch;
    changed_source.source_identity = 0x80200000U;
    const auto before_source_change = ids[0];
    assert(history.assign(std::array{CpuActorRoot{changed_source, 322, 0, 0}},
        std::span(ids).first(1)));
    assert(ids[0] != before_source_change); // A new attachment source is new history.

    history.reset();
    const std::array duplicate{CpuActorRoot{a, 0, 0, 0}, CpuActorRoot{a, 1, 0, 0}};
    assert(history.assign(duplicate, ids));
    assert(ids[0] == 0 && ids[1] == 0);
    assert(history.assign(one_a, std::span(ids).first(1)));
    assert(ids[0] == 0); // Ambiguous previous batch quarantines this key.
    assert(history.assign(one_a, std::span(ids).first(1)));
    assert(ids[0] != 0); // Previous ambiguity cannot donate an ID.

    const std::array mixed{CpuActorRoot{a, 0, 0, 0}, CpuActorRoot{b, 100, 0, 0}};
    assert(history.assign(mixed, ids));
    const auto mixed_a = ids[0], mixed_b = ids[1];
    assert(mixed_a != 0 && mixed_b != 0);
    assert(history.assign(duplicate, ids));
    assert(ids[0] == 0 && ids[1] == 0); // Both duplicate current roots get zero.
    assert(history.assign(mixed, ids));
    assert(ids[0] == 0 && ids[1] != mixed_b && ids[1] != 0);

    history.reset();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const std::array invalid{CpuActorRoot{a, nan, 0, 0}};
    ids[0] = 123;
    assert(!history.assign(invalid, std::span(ids).first(1)));
    assert(ids[0] == 0);
    assert(history.assign(one_a, std::span(ids).first(1)) && ids[0] != 0);
    const auto before_bad_size = ids[0];
    ids = {123, 456};
    assert(!history.assign(one_a, ids));
    assert(ids[0] == 0 && ids[1] == 0);
    assert(history.assign(one_a, std::span(ids).first(1)));
    assert(ids[0] != before_bad_size);
    std::array<CpuActorRoot, 129> too_many{};
    std::array<std::uint32_t, 129> too_many_ids{};
    too_many_ids.fill(123);
    assert(!history.assign(too_many, too_many_ids));
    for (auto id : too_many_ids) assert(id == 0);

    CpuActorRootHistory near_exhaustion{0x7FFFFFFEU};
    assert(near_exhaustion.assign(one_a, std::span(ids).first(1)));
    assert(ids[0] == 0x7FFFFFFEU);
    assert(near_exhaustion.assign(std::array{CpuActorRoot{c, 0, 0, 0}},
        std::span(ids).first(1)));
    assert(ids[0] == 0); // A new ID cannot exceed the reserved range.
    assert(near_exhaustion.assign(one_a, std::span(ids).first(1)));
    assert(ids[0] == 0); // Exhaustion never wraps or recycles the prior ID.
}
