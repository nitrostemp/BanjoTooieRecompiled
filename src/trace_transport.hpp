#pragma once
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <ostream>
#include <string_view>
#include <thread>
namespace tooie {
struct TraceTransportOptions {
    bool buffered=false;
    std::chrono::milliseconds period{16};
    size_t pending_bytes=65536;
};
// No row queue or asynchronous serialization. Caller snapshots rows synchronously.
// Sink must outlive stop(). Lifecycle owner must explicitly check stop() errors.
class TraceTransport {
public:
    TraceTransport()=default;
    ~TraceTransport() noexcept;
    TraceTransport(const TraceTransport&)=delete;
    TraceTransport& operator=(const TraceTransport&)=delete;
    void start(std::ostream& sink,TraceTransportOptions options={});
    void append(std::string_view row,bool barrier);
    void flush();
    void check_health();
    void stop();
private:
    void check_locked();
    void flush_locked();
    void fail_locked(std::exception_ptr error) noexcept;
    void worker() noexcept;
    std::mutex lifecycle_mutex_,mutex_;
    std::condition_variable cv_;
    std::thread flusher_;
    std::ostream* sink_=nullptr;
    TraceTransportOptions options_;
    size_t pending_=0;
    bool running_=false,stopping_=false;
    std::exception_ptr failure_;
};
}
