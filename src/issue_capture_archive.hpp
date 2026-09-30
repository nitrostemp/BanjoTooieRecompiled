#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

namespace tooie::capture_archive {

namespace detail {

// create_directory is the exclusive claim. A process restarted with the same
// clock value and counter must skip an existing child, never reopen it.
inline std::filesystem::path claim_session_directory(
    const std::filesystem::path &logs, std::uint64_t time,
    std::uint64_t counter) noexcept {
    namespace fs = std::filesystem;
    try {
        if (logs.empty()) return {};
        const auto parent = logs / "issue-captures";
        std::error_code error;
        fs::create_directories(parent, error);
        if (error || !fs::is_directory(parent, error) || error) return {};
        for (std::uint64_t attempt = 0; attempt < 65536; ++attempt) {
            if (counter > UINT64_MAX - attempt) return {};
            const auto child = parent /
                ("session-" + std::to_string(time) + "-" +
                    std::to_string(counter + attempt));
            error.clear();
            if (fs::create_directory(child, error)) return child;
            if (error) return {};
            if (!fs::exists(child, error) || error) return {};
        }
    } catch (...) {}
    return {};
}

} // namespace detail

inline std::filesystem::path create_session_directory(
    const std::filesystem::path &logs) noexcept {
    static std::atomic_uint64_t counter{0};
    const auto time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return detail::claim_session_directory(logs, std::uint64_t(time),
        counter.fetch_add(1, std::memory_order_relaxed));
}

// The caller supplies a compact serialized JSON record. This writer preserves
// each record in its claimed session and never rotates or truncates the file.
class Writer {
public:
    explicit Writer(const std::filesystem::path &session_directory) noexcept {
        try {
            if (!session_directory.empty()) {
                file_.open(session_directory / "events.jsonl",
                    std::ios::out | std::ios::app | std::ios::binary);
            }
        } catch (...) {}
    }

    bool append(std::string_view serialized_json_line) noexcept {
        if (serialized_json_line.empty() ||
            serialized_json_line.find_first_of("\r\n") != std::string_view::npos ||
            serialized_json_line.size() >
                std::size_t(std::numeric_limits<std::streamsize>::max())) return false;
        std::lock_guard lock(mutex_);
        if (!file_.is_open() || !file_) return false;
        file_.write(serialized_json_line.data(),
            std::streamsize(serialized_json_line.size()));
        file_.put('\n');
        file_.flush();
        return bool(file_);
    }

private:
    std::mutex mutex_;
    std::ofstream file_;
};

} // namespace tooie::capture_archive
