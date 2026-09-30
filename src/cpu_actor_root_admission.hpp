#pragma once

#include "cpu_actor_root_history.hpp"
#include "model_actor_trace.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace tooie::model_interpolation {

// func_800DE498 saves its resolved model at sp+0x10c immediately before the
// dbanim calls. This is stable model provenance; D_8012C964 is only a
// double-buffered vertex-list base and must not be used for identity.
template <class ReadWord>
std::optional<std::uint32_t> read_cpu_model_identity(std::uint32_t stack,
    ReadWord read) noexcept {
    constexpr std::uint32_t model_offset = 0x10CU;
    if (stack > UINT32_MAX - model_offset ||
        !actor_trace_guest_range(stack + model_offset, 4U)) return std::nullopt;
    std::uint32_t model = 0;
    if (!read(stack + model_offset, model) || !actor_trace_guest_range(model, 4U))
        return std::nullopt;
    return model;
}

template <class ReadWord>
std::optional<CpuActorRoot> read_cpu_actor_root(std::uint32_t owner,
    std::uint32_t position, std::uint64_t epoch, ReadWord read) noexcept {
    // This is an Actor-shaped candidate, not an inference from model art or
    // update rate. Validate the full guest structure before any read callback.
    if (owner > UINT32_MAX - 4U || position != owner + 4U ||
        !actor_trace_guest_range(owner, 0x9CU)) return std::nullopt;

    std::uint32_t descriptor = 0;
    if (!read(owner + 0x10U, descriptor) ||
        !actor_trace_guest_range(descriptor, 8U)) return std::nullopt;
    std::uint32_t opcode = 0, delay_word = 0;
    if (!read(descriptor, opcode) || !read(descriptor + 4U, delay_word))
        return std::nullopt;
    // Tooie's dispatch table is 8-byte aligned in both cached and uncached
    // KSEG aliases. The loader retains the original ADDI/XORI delay word when
    // it replaces a resident overlay's syscall with a direct J instruction.
    constexpr std::uint32_t table_first = 0x80082540U;
    constexpr std::uint32_t table_end = 0x8008A988U;
    const auto canonical_stub = 0x80000000U | (descriptor & 0x1FFFFFFFU);
    if (canonical_stub < table_first || canonical_stub >= table_end ||
        (canonical_stub & 7U) != 0 ||
        ((delay_word & 0xFFFF0000U) != 0x20080000U &&
            (delay_word & 0xFFFF0000U) != 0x38080000U)) return std::nullopt;
    const auto entry_offset = delay_word & 0xFFFFU;
    const auto rewind = entry_offset * 2U;
    if (entry_offset >= 0x8000U || (entry_offset & 3U) != 0 ||
        canonical_stub < table_first + rewind) return std::nullopt;
    const auto table_start = canonical_stub - rewind;
    if ((opcode & 0xFC00003FU) != 0x0000000CU) {
        if ((opcode & 0xFC000000U) != 0x08000000U)
            return std::nullopt;
        const auto hook = ((opcode << 2U) & 0x0FFFFFFFU) |
            (descriptor & 0xF0000000U);
        const auto canonical_hook = 0x80000000U | (hook & 0x1FFFFFFFU);
        if (canonical_hook < 0x80000410U ||
            (hook & 0xFU) != 0 ||
            !actor_trace_guest_range(hook - 0x10U, 0x38U))
            return std::nullopt;
        const auto header = hook - 0x10U;
        std::uint32_t counts = 0, identity = 0;
        if (!read(header + 8U, counts) || !read(header + 0x2CU, identity))
            return std::nullopt;
        const auto entries = counts >> 16U;
        const auto overlay_id = identity >> 16U;
        const auto syscall_index = identity & 0xFFFFU;
        if (entries == 0 || entries >= 0x8000U || overlay_id == 0 ||
            entry_offset / 4U >= entries ||
            table_start != table_first + syscall_index * 8U)
            return std::nullopt;
    }

    std::uint32_t x_bits = 0, y_bits = 0, z_bits = 0;
    if (!read(position, x_bits) || !read(position + 4U, y_bits) ||
        !read(position + 8U, z_bits)) return std::nullopt;
    const float x = std::bit_cast<float>(x_bits);
    const float y = std::bit_cast<float>(y_bits);
    const float z = std::bit_cast<float>(z_bits);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return std::nullopt;
    return CpuActorRoot{{owner, descriptor, epoch}, x, y, z};
}

// This path is entered only by an exact source-bracketed attachment renderer.
// Its PlayerState owner and fixed role are supplied by that bracket; the
// resolved renderer model quarantines an attachment/model switch.
template <class ReadWord>
std::optional<CpuActorRoot> read_cpu_attachment_root(std::uint32_t owner,
    std::uint32_t role, std::uint32_t position, std::uint32_t model,
    std::uint64_t epoch, ReadWord read) noexcept {
    if (owner == 0 || role == 0 || position == 0 || model == 0 ||
        !actor_trace_guest_range(owner, 4U) ||
        !actor_trace_guest_range(position, 12U) ||
        !actor_trace_guest_range(model, 4U)) return std::nullopt;
    std::uint32_t x_bits = 0, y_bits = 0, z_bits = 0;
    if (!read(position, x_bits) || !read(position + 4U, y_bits) ||
        !read(position + 8U, z_bits)) return std::nullopt;
    const float x = std::bit_cast<float>(x_bits);
    const float y = std::bit_cast<float>(y_bits);
    const float z = std::bit_cast<float>(z_bits);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return std::nullopt;
    return CpuActorRoot{{owner, role, epoch, model}, x, y, z};
}

} // namespace tooie::model_interpolation
