// CIC algorithm adapted from X-Scale (2011), BSD-style license retained below.
// Upstream: mupen64plus/mupen64plus-core b20b27ebf9e5b099a978e86dba609111dc98c837
// src/device/pif/n64_cic_nus_6105.c. Native SI transaction code is Tooie-specific.
/*
 * Copyright 2011 X-Scale. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY X-Scale ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL X-Scale OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of X-Scale.
 */
#include "si_adapter.hpp"
#include "ultramodern/ultramodern.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace tooie::si {
Challenge cic6105_response(const Challenge& challenge) {
    static constexpr uint8_t tables[2][16] = {
        {4, 7, 10, 7, 14, 5, 14, 1, 12, 15, 8, 15, 6, 3, 6, 9},
        {4, 1, 10, 7, 14, 5, 14, 1, 12, 9, 8, 5, 6, 3, 12, 9}};
    uint8_t key = 11;
    unsigned table = 0;
    Challenge response{};
    for (unsigned i = 0; i < 30; ++i) {
        const unsigned shift = (i % 2 == 0) ? 4 : 0;
        const uint8_t nibble = (challenge[i / 2] >> shift) & 15;
        const uint8_t result = (key + 5 * nibble) & 15;
        response[i / 2] |= result << shift;
        key = tables[table][result];
        const unsigned sign = result >> 3;
        const unsigned magnitude = (sign ? ~result : result) & 7;
        unsigned next = (magnitude % 3 == 1) ? sign : 1 - sign;
        if (table == 1 && (result == 1 || result == 9)) next = 1;
        if (table == 1 && (result == 11 || result == 14)) next = 0;
        table = next;
    }
    return response;
}

void Device::reset() { pif_.fill(0); response_available_ = false; }
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
PersistentState Device::persistent_export() const {
    PersistentState state;state.pif=pif_;state.response_available=response_available_;return state;
}
void Device::persistent_import(const PersistentState& state) {
    if(state.version!=1)throw std::runtime_error("SI snapshot version mismatch");
    pif_=state.pif;response_available_=state.response_available;
}
#endif

void Device::transfer(std::span<uint8_t> rdram, uint32_t direction, uint32_t address) {
    if (direction > 1) throw std::runtime_error("SI: unsupported DMA direction");
    const uint32_t segment = address & 0xE0000000u;
    const size_t offset = address & 0x1FFFFFFFu;
    if ((segment != 0x80000000u && segment != 0xA0000000u) || (address & 3u)
        || offset > rdram.size() || rdram.size() - offset < 64)
        throw std::runtime_error("SI: DMA address is not aligned, in-range cached/uncached RDRAM");
    if (direction == 1) {
        std::array<uint8_t, 64> incoming;
        for (size_t i = 0; i < incoming.size(); ++i) incoming[i] = rdram[(offset + i) ^ 3];
        if (incoming[63] != 2)
            throw std::runtime_error("SI: unsupported raw PIF command (only CIC-6105 command 0x02 implemented)");
        if (incoming[46] != 15 || incoming[47] != 15
            || !std::all_of(incoming.begin(), incoming.begin() + 46, [](uint8_t v) { return v == 0xFF; }))
            throw std::runtime_error("SI: unsupported CIC transaction shape or challenge length");
        Challenge challenge;
        std::copy_n(incoming.begin() + 48, challenge.size(), challenge.begin());
        const auto response = cic6105_response(challenge);
        std::copy(response.begin(), response.end(), incoming.begin() + 48);
        incoming[46] = incoming[47] = incoming[63] = 0;
        pif_ = incoming;
        response_available_ = true;
    } else {
        if (!response_available_) throw std::runtime_error("SI: read before a supported PIF transaction");
        for (size_t i = 0; i < pif_.size(); ++i) rdram[(offset + i) ^ 3] = pif_[i];
    }
}

namespace {
Device device;
size_t memory_size = 8 * 1024 * 1024;
std::mutex device_mutex; // Held only during synchronous transfer; never across guest scheduling.
Observer observer = nullptr; // Configured only outside worker lifetime.
std::atomic<uint64_t> observation_failures{0};
uint64_t transfer_sequence = 0, challenge_sequence = 0;
Challenge original_challenge{};
void publish(Observer callback, Observation& observation, Phase phase) noexcept {
    if (!callback) return;
    observation.phase = phase;
    try { callback(observation); }
    catch (...) { observation_failures.fetch_add(1, std::memory_order_relaxed); }
}
}
void configure_observer(Observer callback) noexcept { observer = callback; }
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
PersistentState persistent_export() {
    std::lock_guard lock(device_mutex);
    auto state=device.persistent_export();
    state.transfer_sequence=transfer_sequence;state.challenge_sequence=challenge_sequence;
    state.original_challenge=original_challenge;return state;
}
void persistent_import(const PersistentState& state) {
    std::lock_guard lock(device_mutex);
    device.persistent_import(state);transfer_sequence=state.transfer_sequence;
    challenge_sequence=state.challenge_sequence;original_challenge=state.original_challenge;
}
#endif
uint64_t observer_failures() noexcept { return observation_failures.load(std::memory_order_relaxed); }
const char* phase_name(Phase phase) noexcept {
    switch (phase) {
        case Phase::DmaTransferred: return "dma_transferred";
        case Phase::CompletionCall: return "completion_call";
        case Phase::CompletionReturned: return "completion_returned";
        case Phase::CompletionThrew: return "completion_threw";
    }
    return "unknown";
}
void reset(size_t rdram_size) {
    if (rdram_size < 64 || (rdram_size & 3)) throw std::runtime_error("SI: invalid RDRAM capacity");
    std::lock_guard lock(device_mutex);
    memory_size = rdram_size;
    device.reset();
    transfer_sequence = challenge_sequence = 0;
    original_challenge.fill(0);
    observation_failures.store(0, std::memory_order_relaxed);
}
}

namespace {
// Original linked core1 ELF __osSiCreateAccessQueue at 0x800319A0.
constexpr int32_t access_enabled = static_cast<int32_t>(0x800414A0u);
constexpr int32_t access_buffer = static_cast<int32_t>(0x80081210u);
constexpr int32_t access_queue = static_cast<int32_t>(0x80081218u);
}

extern "C" void tooie_si_get_access(uint8_t* rdram, recomp_context*) {
    if (!MEM_W(0, access_enabled)) {
        MEM_W(0, access_enabled) = 1;
        osCreateMesgQueue(rdram, access_queue, access_buffer, 1);
        if (osSendMesg(rdram, access_queue, 0, OS_MESG_NOBLOCK) != 0)
            throw std::runtime_error("SI: access queue initialization failed");
    }
    // The dummy received message has no observable use; NULL avoids a fabricated guest stack.
    if (osRecvMesg(rdram, access_queue, NULLPTR, OS_MESG_BLOCK) != 0)
        throw std::runtime_error("SI: access queue acquisition failed");
}

extern "C" void tooie_si_rel_access(uint8_t* rdram, recomp_context*) {
    if (osSendMesg(rdram, access_queue, 0, OS_MESG_NOBLOCK) != 0)
        throw std::runtime_error("SI: access queue release failed");
}

extern "C" void tooie_si_raw_start_dma(uint8_t* rdram, recomp_context* ctx) {
    tooie::si::Observation observation;
    const auto callback = tooie::si::observer;
    {
        std::lock_guard lock(tooie::si::device_mutex);
        tooie::si::device.transfer({rdram, tooie::si::memory_size}, uint32_t(ctx->r4), uint32_t(ctx->r5));
        // Device has validated the span/direction and committed the transfer.
        // OS_WRITE leaves its source untouched; OS_READ has written actual PIF
        // output. Read canonical bytes only after that successful operation.
        if (callback) {
            observation.transfer_id = ++tooie::si::transfer_sequence;
            observation.direction = uint32_t(ctx->r4);
            observation.guest_address = uint32_t(ctx->r5);
            observation.physical_address = observation.guest_address & 0x1FFFFFFFu;
            observation.ra = uint32_t(ctx->r31); observation.sp = uint32_t(ctx->r29);
            for (size_t i = 0; i < observation.payload.size(); ++i)
                observation.payload[i] = rdram[(observation.physical_address + i) ^ 3];
            if (observation.direction == 1) {
                ++tooie::si::challenge_sequence;
                std::copy_n(observation.payload.begin() + 48, 15, tooie::si::original_challenge.begin());
            }
            observation.challenge_id = tooie::si::challenge_sequence;
            observation.challenge = tooie::si::original_challenge;
        }
    }
    tooie::si::publish(callback, observation, tooie::si::Phase::DmaTransferred);
    ctx->r2 = 0;
    // Completion uses the existing SI event registration and external-message delivery.
    // No new producer thread or timing oracle: payload is ready before completion is queued.
    tooie::si::publish(callback, observation, tooie::si::Phase::CompletionCall);
    try { ultramodern::send_si_message(); }
    catch (...) {
        tooie::si::publish(callback, observation, tooie::si::Phase::CompletionThrew);
        throw;
    }
    // The runtime API is void; this observes only return from its enqueue call,
    // not an allocation result, delivery to a guest queue, or osRecvMesg success.
    tooie::si::publish(callback, observation, tooie::si::Phase::CompletionReturned);
}
