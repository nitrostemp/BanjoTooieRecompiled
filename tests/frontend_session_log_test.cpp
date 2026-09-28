#include "frontend_session_log.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string payload(char fill) {
    return "{\"payload\":\"" + std::string(1500, fill) + "\"}";
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("tooie-session-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        {
            tooie::session_log::Writer writer(root, 4096, 2);
            writer.write("line\t\x01", payload('a'));
            writer.write("second", payload('b'));
            writer.write("third", payload('c'));
            writer.flush();
        }
        const auto active = root / "session.jsonl";
        const auto rotated = root / "session.jsonl.1";
        require(std::filesystem::exists(active) && std::filesystem::exists(rotated),
            "byte-bounded writer did not rotate its own session file");
        require(std::filesystem::file_size(active) <= 4096 && std::filesystem::file_size(rotated) <= 4096,
            "rotation left an oversized session file");
        const auto old_rows = read(rotated);
        require(old_rows.find("\"event\":\"line\\t\\u0001\"") != std::string::npos,
            "event JSON did not escape tab/control bytes");
        require(old_rows.find("\"timestamp\":\"") != std::string::npos && old_rows.find("\"elapsed_ms\":") != std::string::npos,
            "row lacks wall and monotonic timestamps");

        const auto original_session = read(active);
        {
            tooie::session_log::Writer captures(root, 4096, 2, "graphics-captures.jsonl");
            require(captures.path() == root / "graphics-captures.jsonl",
                "named writer did not select its own active file");
            captures.write("artifact_draw_capture", payload('x'));
            captures.write("artifact_draw_capture", payload('y'));
            captures.write("artifact_draw_capture_failure", payload('z'));
            captures.flush();
        }
        require(read(active) == original_session && read(rotated) == old_rows,
            "named writer changed a session log file");
        require(std::filesystem::exists(root / "graphics-captures.jsonl.1") &&
            std::filesystem::file_size(root / "graphics-captures.jsonl") <= 4096,
            "named writer did not rotate independently within its bound");
        for (const std::string name : {"", ".", "..", "...", "capture.", "../escape.jsonl", "sub/log.jsonl",
                                       "sub\\log.jsonl", "C:escape.jsonl"}) {
            bool rejected = false;
            try {
                tooie::session_log::Writer invalid(root, 4096, 2, name);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "writer accepted a non-plain filename");
        }
        require(!std::filesystem::exists(root.parent_path() / "escape.jsonl"),
            "invalid filename escaped the log directory");

        const auto blocked = root / "blocked";
        std::filesystem::create_directories(blocked / "session.jsonl.1");
        std::ofstream(blocked / "session.jsonl.1" / "keep") << "x";
        {
            tooie::session_log::Writer writer(blocked, 4096, 2);
            writer.write("one", payload('d'));
            writer.write("two", payload('e'));
            writer.write("three", payload('f'));
            writer.flush();
        }
        require(std::filesystem::file_size(blocked / "session.jsonl") <= 4096,
            "failed rotation appended beyond the configured bound");
        require(!std::filesystem::exists(blocked / "session.jsonl.2"),
            "failed rotation continued through later retention names");

        const auto active_blocked = root / "active-blocked";
        std::filesystem::create_directories(active_blocked / "session.jsonl");
        {
            tooie::session_log::Writer writer(active_blocked, 4096, 2);
            require(!writer.healthy(), "writer accepted a non-regular active session path");
            writer.write("ignored", payload('e'));
        }
        require(std::filesystem::is_directory(active_blocked / "session.jsonl"),
            "writer altered an unexpected active session path");
        std::filesystem::remove_all(root);
        std::cout << "PASS frontend session log JSON escaping, isolated bounded rotation, filename validation and failure containment\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
