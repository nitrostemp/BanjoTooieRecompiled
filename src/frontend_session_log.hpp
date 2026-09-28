#pragma once

#include <filesystem>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tooie::session_log {

// Buffered JSONL writer for human-retrievable diagnostic events. It retains
// a small fixed number of rotated files and never accepts frame-by-frame data.
class Writer {
public:
    explicit Writer(std::filesystem::path directory, std::size_t max_bytes = 1U << 20,
                    unsigned retained_files = 3, std::string filename = "session.jsonl");
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // json_fields must be one complete JSON value. Callers should serialize
    // structured values before passing them here.
    void write(std::string_view event, std::string_view json_fields = "{}");
    void mark_issue(std::string_view summary);
    void flush();
    bool healthy() const;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    void rotate_locked();
    void flush_locked();

    std::filesystem::path directory_, path_;
    std::size_t max_bytes_;
    unsigned retained_files_;
    std::size_t bytes_ = 0;
    std::vector<std::string> pending_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_flush_ = started_;
    bool failed_ = false;
    mutable std::mutex mutex_;
};

std::string utc_timestamp();

} // namespace tooie::session_log
