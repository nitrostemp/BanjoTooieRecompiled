#include "context_observer.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace {
struct Observation {
    std::atomic<void*> pointer{nullptr};
    std::atomic<unsigned> deletes{0};
};
std::array<Observation, 16> observations{};
std::atomic<std::size_t> next_observation{0};

void observe_delete(void* pointer) noexcept {
    if (!pointer) return;
    for (auto& observation : observations) {
        void* expected = pointer;
        if (observation.pointer.compare_exchange_strong(expected, nullptr)) {
            // Retire the address before free; allocator address reuse cannot
            // be mistaken for a second deletion of this context.
            observation.deletes.fetch_add(1);
            observation.deletes.notify_all();
        }
    }
}
}

tooie::context_observer::Token tooie::context_observer::watch_context(void* context) {
    if (!context) throw std::runtime_error("Cannot observe a null context");
    auto token = next_observation.fetch_add(1);
    if (token >= observations.size()) throw std::runtime_error("Context observation capacity exceeded");
    for (const auto& existing : observations)
        if (existing.pointer.load() == context) throw std::runtime_error("Context already observed");
    observations[token].pointer.store(context);
    return token;
}

unsigned tooie::context_observer::deletion_count(Token token) {
    return observations.at(token).deletes.load();
}

void tooie::context_observer::wait_for_deletion(Token token) {
    auto& count = observations.at(token).deletes;
    while (count.load() == 0) count.wait(0);
}

// Pair replacement allocation/deallocation functions. No tracing or allocation
// in the deletion observer; it only updates fixed atomics for explicitly watched
// pointers. Context destruction has completed before operator delete is entered:
// an unjoined std::thread would already have terminated the process.
void* operator new(std::size_t size) {
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { observe_delete(p); std::free(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
