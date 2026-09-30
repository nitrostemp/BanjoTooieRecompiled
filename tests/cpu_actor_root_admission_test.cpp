#include "cpu_actor_root_admission.hpp"

#include <cassert>
#include <bit>
#include <cstdint>
#include <limits>
#include <unordered_map>

int main() {
    using tooie::model_interpolation::read_cpu_actor_root;
    using tooie::model_interpolation::read_cpu_attachment_root;
    using tooie::model_interpolation::read_cpu_model_identity;
    constexpr std::uint32_t owner = 0x80100000U;
    constexpr std::uint64_t epoch = 42;
    std::unordered_map<std::uint32_t, std::uint32_t> words;
    words[owner + 4U] = std::bit_cast<std::uint32_t>(1.25f);
    words[owner + 8U] = std::bit_cast<std::uint32_t>(-2.5f);
    words[owner + 12U] = std::bit_cast<std::uint32_t>(3.75f);
    unsigned reads = 0;
    auto read = [&](std::uint32_t address, std::uint32_t &word) {
        ++reads;
        const auto found = words.find(address);
        if (found == words.end()) return false;
        word = found->second;
        return true;
    };

    constexpr std::uint32_t stack = 0x80120000U;
    constexpr std::uint32_t model = 0x80200000U;
    words[stack + 0x10CU] = model;
    assert(read_cpu_model_identity(stack, read) == model);
    words.erase(stack + 0x10CU);
    assert(!read_cpu_model_identity(stack, read));
    assert(!read_cpu_model_identity(0xFFFFFF00U, read));
    words[stack + 0x10CU] = 0x807FFFFFU;
    assert(!read_cpu_model_identity(stack, read));
    words[stack + 0x10CU] = model;

    const auto attachment = read_cpu_attachment_root(owner, 0x4241434BU,
        owner + 4U, model, epoch, read);
    assert(attachment);
    assert(attachment->key.owner == owner);
    assert(attachment->key.descriptor == 0x4241434BU);
    assert(attachment->key.source_identity == model);
    assert(!read_cpu_attachment_root(0, 0x4241434BU, owner + 4U, model,
        epoch, read));
    assert(!read_cpu_attachment_root(owner, 0, owner + 4U, model,
        epoch, read));
    assert(!read_cpu_attachment_root(owner, 0x4241434BU, owner + 4U, 0,
        epoch, read));
    words[owner + 12U] = std::bit_cast<std::uint32_t>(
        std::numeric_limits<float>::quiet_NaN());
    assert(!read_cpu_attachment_root(owner, 0x4241434BU, owner + 4U, model,
        epoch, read));
    words[owner + 12U] = std::bit_cast<std::uint32_t>(3.75f);
    std::uint32_t syscall_id = 215U;
    for (const std::uint32_t descriptor : {0x80085CA8U, 0x80085C70U,
            0x80086028U, 0x80086100U, 0xA0086100U}) {
        words[owner + 0x10U] = descriptor;
        words[descriptor] = (syscall_id++ << 6U) | 0xCU;
        words[descriptor + 4U] = (descriptor == 0x80086028U ? 0x38080000U :
            0x20080000U) | (descriptor == 0x80085C70U ? 8U : 0U);
        const auto candidate = read_cpu_actor_root(owner, owner + 4U, epoch, read);
        assert(candidate.has_value());
        assert(candidate->key.owner == owner);
        assert(candidate->key.descriptor == descriptor);
        assert(candidate->key.epoch == epoch);
        assert(candidate->x == 1.25f && candidate->y == -2.5f &&
            candidate->z == 3.75f);
    }

    // A resident overlay rewrites the syscall instruction to J, while its
    // original ADDI/XORI delay word still encodes this table entry offset.
    constexpr std::uint32_t patched_stub = 0x80085C70U; // entrypoint_2
    constexpr std::uint32_t table_start = 0x80085C60U;
    constexpr std::uint32_t hook = 0x80110010U;
    constexpr std::uint32_t header = hook - 0x10U;
    words[owner + 0x10U] = patched_stub;
    words[patched_stub] = 0x08000000U | ((hook & 0x0FFFFFFFU) >> 2U);
    words[patched_stub + 4U] = 0x20080008U;
    words[header + 8U] = 4U << 16U;
    words[header + 0x2CU] = (215U << 16U) |
        ((table_start - 0x80082540U) / 8U);
    assert(read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[header + 0x2CU] += 1U; // Header names another dispatch group.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[header + 0x2CU] -= 1U;
    words[header + 8U] = 2U << 16U; // Entrypoint 2 exceeds count 2.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[header + 8U] = 4U << 16U;
    words[patched_stub + 4U] = 0x30080008U; // ANDI is not original delay.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[patched_stub + 4U] = 0x38080008U; // XORI is valid too.
    assert(read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words.erase(header + 8U); // Partial header cannot admit a patched J.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[header + 8U] = 4U << 16U;
    words[patched_stub] = 0x08000000U; // J target below live guest RAM.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[patched_stub] = 0x08000000U | ((hook & 0x0FFFFFFFU) >> 2U);

    // Both guest aliases name the same original dispatch table and live hook.
    constexpr std::uint32_t uncached_stub = patched_stub | 0x20000000U;
    constexpr std::uint32_t uncached_header = header | 0x20000000U;
    words[owner + 0x10U] = uncached_stub;
    words[uncached_stub] = words[patched_stub];
    words[uncached_stub + 4U] = words[patched_stub + 4U];
    words[uncached_header + 8U] = words[header + 8U];
    words[uncached_header + 0x2CU] = words[header + 0x2CU];
    assert(read_cpu_actor_root(owner, owner + 4U, epoch, read));

    auto reject_without_read = [&](std::uint32_t base, std::uint32_t position) {
        reads = 0;
        assert(!read_cpu_actor_root(base, position, epoch, read));
        assert(reads == 0);
    };
    reject_without_read(0, 4);
    reject_without_read(owner, owner + 8U);
    reject_without_read(0xFFFFFFFCU, 0U);
    reject_without_read(owner + 1U, owner + 5U);
    reject_without_read(0x807FFF68U, 0x807FFF6CU); // Actor crosses RDRAM.
    reject_without_read(0xC0100000U, 0xC0100004U);

    words[owner + 0x10U] = 0;
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[owner + 0x10U] = 0x80085CA9U; // Unaligned descriptor.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[owner + 0x10U] = 0x807FFFFCU; // Eight-byte stub crosses RDRAM.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[owner + 0x10U] = 0xC0085CA8U;
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));

    words[owner + 0x10U] = 0x80085CA8U;
    words[0x80085CA8U] = 0x08000000U; // Jump is not a syscall stub.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[0x80085CA8U] = 0x0400000CU; // Syscall funct with wrong op field.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[0x80085CA8U] = 0x0000000CU;
    words.erase(0x80085CACU); // Partial descriptor read.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[0x80085CACU] = 0x20080000U;
    words.erase(owner + 8U); // Partial position read cannot fabricate zero.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[owner + 8U] = std::bit_cast<std::uint32_t>(-2.5f);
    words[owner + 12U] = std::bit_cast<std::uint32_t>(
        std::numeric_limits<float>::quiet_NaN());
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[owner + 12U] = std::bit_cast<std::uint32_t>(
        std::numeric_limits<float>::infinity());
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));
    words[owner + 12U] = std::bit_cast<std::uint32_t>(3.75f);
    words.erase(owner + 0x10U); // Missing descriptor is invalid.
    assert(!read_cpu_actor_root(owner, owner + 4U, epoch, read));

}
