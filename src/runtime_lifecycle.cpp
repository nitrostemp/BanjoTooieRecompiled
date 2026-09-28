#include "runtime_lifecycle.hpp"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace {
std::atomic_bool active{false}, stopping_flag{false};
bool reject_tasks_flag=true;
std::mutex owner_mutex;
std::condition_variable owners_changed;
std::unordered_map<UltraThreadContext*,bool> producers;
std::exception_ptr first_failure;
std::size_t created_count=0, enqueued_count=0, deleted_count=0;
}
namespace tooie::lifecycle {
void enable(bool reject_tasks) {
    std::lock_guard lock(owner_mutex);
    if (active.exchange(true)) throw std::runtime_error("Continuous lifecycle is single-use per process");
    reject_tasks_flag=reject_tasks;
}
bool enabled() noexcept { return active.load(std::memory_order_acquire); }
bool stopping() noexcept { return stopping_flag.load(std::memory_order_acquire); }
void poll() { if (stopping()) throw ultramodern::thread_terminated{}; }
void request_stop() noexcept {
    std::lock_guard lock(owner_mutex);
    stopping_flag.store(true,std::memory_order_release);
    // Only touch native semaphores. Guest queues and OSThread memory belong to
    // the scheduler, and must never be modified by the controller concurrently.
    for (auto [context,launched]:producers) if (launched) context->running.signal();
    owners_changed.notify_all();
}
void fail(std::exception_ptr error) noexcept {
    { std::lock_guard lock(owner_mutex); if (!first_failure) first_failure=error; }
    request_stop();
}
std::exception_ptr failure() { std::lock_guard lock(owner_mutex); return first_failure; }
void created(UltraThreadContext* context) {
    std::lock_guard lock(owner_mutex);
    if (stopping_flag.load()) throw ultramodern::thread_terminated{};
    if (!producers.emplace(context,false).second) throw std::runtime_error("Duplicate continuous worker ownership");
    ++created_count;
}
void launched(UltraThreadContext* context) {
    std::lock_guard lock(owner_mutex);
    producers.at(context)=true;
    if (stopping_flag.load()) context->running.signal();
}
void creation_failed(UltraThreadContext* context) noexcept {
    std::lock_guard lock(owner_mutex);
    if (producers.erase(context)) --created_count;
    owners_changed.notify_all();
}
void enqueue_completed(UltraThreadContext* context,const std::function<void()>& scheduler_exit) {
    std::lock_guard lock(owner_mutex);
    // Serialize the last ordinary scheduler access with broadcast shutdown.
    // Otherwise a late run_next_thread could signal an already cleaned context.
    if (!stopping_flag.load()) {
        try { scheduler_exit(); }
        catch (...) {
            if (!first_failure) first_failure=std::current_exception();
            stopping_flag.store(true,std::memory_order_release);
            for (auto [other,ready]:producers) if (ready) other->running.signal();
        }
    }
    // Remove ownership and enqueue under one lock. Stop cannot dereference a
    // context after the cleaner sees it. Quiescence follows the final enqueue.
    if (producers.erase(context)!=1) std::terminate();
    ultramodern::cleanup_thread(context);
    ++enqueued_count;
    owners_changed.notify_all();
}
void deleted(UltraThreadContext*) noexcept {
    if (!enabled()) return;
    std::lock_guard lock(owner_mutex);
    ++deleted_count;
    owners_changed.notify_all();
}
bool wait_for_producers(std::chrono::milliseconds timeout) {
    std::unique_lock lock(owner_mutex);
    return owners_changed.wait_for(lock,timeout,[]{return producers.empty();});
}
bool wait_for_shutdown_quiescence(std::chrono::milliseconds timeout) {
    std::unique_lock lock(owner_mutex);
    return owners_changed.wait_for(lock,timeout,[]{
        return stopping_flag.load(std::memory_order_acquire) && producers.empty();
    });
}
Counts counts() { std::lock_guard lock(owner_mutex); return {created_count,producers.size(),enqueued_count,deleted_count}; }
void reject_task(uint32_t type) {
    if (enabled() && reject_tasks_flag)
        throw std::runtime_error("Headless diagnostic cannot execute actual OSTask type "+std::to_string(type));
}
}
