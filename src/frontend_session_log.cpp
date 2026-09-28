#include "frontend_session_log.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
std::string escape(std::string_view input) {
    std::string result;
    result.reserve(input.size() + 8);
    for (const auto c : input) {
        const auto value = static_cast<unsigned char>(c);
        if (c == '\\' || c == '\"') { result.push_back('\\'); result.push_back(c); continue; }
        if (c == '\n') { result += "\\n"; continue; }
        if (c == '\r') { result += "\\r"; continue; }
        if (c == '\t') { result += "\\t"; continue; }
        if (value < 0x20) {
            static constexpr char digits[] = "0123456789abcdef";
            result += "\\u00";
            result += digits[(value >> 4) & 0x0f];
            result += digits[value & 0x0f];
            continue;
        }
        result.push_back(c);
    }
    return result;
}
}

namespace tooie::session_log {

namespace {
bool plain_filename(std::string_view name) {
    if (name.empty() || name.front() == '.' || name.back() == '.') return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}
} // namespace

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &stamp);
#else
    gmtime_r(&stamp, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
        << milliseconds << 'Z';
    return out.str();
}

Writer::Writer(std::filesystem::path directory, std::size_t max_bytes, unsigned retained_files,
               std::string filename)
    : directory_(std::move(directory)), path_(directory_ / filename), max_bytes_(max_bytes), retained_files_(retained_files) {
    if (!plain_filename(filename)) throw std::invalid_argument("Session-log filename must be plain");
    if (max_bytes_ < 4096 || retained_files_ == 0) throw std::invalid_argument("Invalid bounded session-log configuration");
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) { failed_ = true; return; }
    const auto status = std::filesystem::symlink_status(path_, error);
    // Windows reports ERROR_FILE_NOT_FOUND for an absent first-run file.
    // That is the only path-query error that means there is no active log yet.
    const bool missing = error == std::errc::no_such_file_or_directory;
    if (missing) error.clear();
    if (error) { failed_ = true; return; }
    if (!missing && status.type() != std::filesystem::file_type::not_found) {
        if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
            failed_ = true;
            return;
        }
        const auto size = std::filesystem::file_size(path_, error);
        if (error) { failed_ = true; return; }
        bytes_ = static_cast<std::size_t>(size);
    }
}

Writer::~Writer() { try { flush(); } catch (...) {} }

void Writer::write(std::string_view event, std::string_view fields) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
    const auto row = "{\"timestamp\":\"" + utc_timestamp() + "\",\"elapsed_ms\":" + std::to_string(elapsed) +
        ",\"event\":\"" + escape(event) + "\",\"fields\":" + std::string(fields) + "}\n";
    std::lock_guard lock(mutex_);
    if (failed_) return;
    if (row.size() > max_bytes_) return;
    if (bytes_ + row.size() > max_bytes_) rotate_locked();
    if (failed_) return;
    bytes_ += row.size();
    pending_.push_back(row);
    const auto now = std::chrono::steady_clock::now();
    if (pending_.size() >= 32 || now - last_flush_ >= std::chrono::seconds(2)) flush_locked();
}

void Writer::mark_issue(std::string_view summary) {
    write("issue_marker", "{\"summary\":\"" + escape(summary) + "\"}");
}

void Writer::flush() { std::lock_guard lock(mutex_); flush_locked(); }

bool Writer::healthy() const {
    std::lock_guard lock(mutex_);
    return !failed_;
}

void Writer::rotate_locked() {
    flush_locked();
    if (failed_) return;
    for (unsigned index = retained_files_; index > 0; --index) {
        const auto source = index == 1 ? path_ : path_.string() + "." + std::to_string(index - 1);
        const auto target = path_.string() + "." + std::to_string(index);
        std::error_code error;
        const bool source_exists = std::filesystem::exists(source, error);
        if (error) { failed_ = true; pending_.clear(); return; }
        if (!source_exists) continue;
        const auto source_status = std::filesystem::symlink_status(source, error);
        if (error || !std::filesystem::is_regular_file(source_status) || std::filesystem::is_symlink(source_status)) {
            // Never repurpose an unexpected path (for example a directory or
            // symlink) merely because its name resembles a retained log.
            failed_ = true;
            pending_.clear();
            return;
        }
        const bool target_exists = std::filesystem::exists(target, error);
        if (error) { failed_ = true; pending_.clear(); return; }
        const auto target_status = target_exists ? std::filesystem::symlink_status(target, error) : std::filesystem::file_status{};
        if (error || (target_exists && (!std::filesystem::is_regular_file(target_status) || std::filesystem::is_symlink(target_status)))) {
            failed_ = true;
            pending_.clear();
            return;
        }
        std::filesystem::remove(target, error);
        if (error) { failed_ = true; pending_.clear(); return; }
        std::filesystem::rename(source, target, error);
        if (error) { failed_ = true; pending_.clear(); return; }
    }
    bytes_ = 0;
}

void Writer::flush_locked() {
    if (pending_.empty() || failed_) return;
    std::ofstream out(path_, std::ios::app | std::ios::binary);
    if (!out) { failed_ = true; pending_.clear(); return; }
    for (const auto& row : pending_) out << row;
    out.close();
    if (!out) { failed_ = true; pending_.clear(); return; }
    pending_.clear();
    last_flush_ = std::chrono::steady_clock::now();
}

} // namespace tooie::session_log
