#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>

struct UltraThreadContext;
namespace tooie::lifecycle {
// App-owned opt-in extension to pinned ultramodern. Diagnostic paths stay off.
struct Counts { std::size_t created, producers, enqueued, deleted; };
void enable(bool reject_tasks = true);
bool enabled() noexcept;
bool stopping() noexcept;
void poll();
void request_stop() noexcept;
void fail(std::exception_ptr error) noexcept;
std::exception_ptr failure();
void created(UltraThreadContext* context);
void launched(UltraThreadContext* context);
void creation_failed(UltraThreadContext* context) noexcept;
void enqueue_completed(UltraThreadContext* context,const std::function<void()>& scheduler_exit);
void deleted(UltraThreadContext* context) noexcept;
bool wait_for_producers(std::chrono::milliseconds timeout);
// A saver can observe ultramodern::exited before the continuous host has
// admitted shutdown. Wait for that admission boundary and producer drain.
bool wait_for_shutdown_quiescence(std::chrono::milliseconds timeout);
Counts counts();
void reject_task(uint32_t type);
// Implemented in the materialized timer.cpp; interrupts a host wait, never
// manufactures a guest OS message or task completion.
void stop_and_join_timer();
}
