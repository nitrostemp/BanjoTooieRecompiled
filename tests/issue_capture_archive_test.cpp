#include "issue_capture_archive.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    using tooie::capture_archive::Writer;
    using tooie::capture_archive::create_session_directory;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::current_path() /
        ("issue-capture-archive-test-" + std::to_string(stamp));
    assert(fs::create_directory(root));
    const auto logs = root / "logs";
    assert(fs::create_directory(logs));

    const auto first = create_session_directory(logs);
    assert(!first.empty() && fs::is_directory(first));
    assert(first.parent_path() == logs / "issue-captures");
    {
        Writer writer(first);
        for (int i = 0; i < 24; ++i)
            assert(writer.append("{\"event\":\"first\",\"n\":" + std::to_string(i) + "}"));
        assert(!writer.append(""));
        assert(!writer.append("{\"event\":1}\n{\"event\":2}"));
    }
    std::ifstream prior(first / "events.jsonl");
    std::string line;
    std::vector<std::string> first_lines;
    while (std::getline(prior, line)) first_lines.push_back(line);
    assert(first_lines.size() == 24);
    assert(first_lines[0] == "{\"event\":\"first\",\"n\":0}");
    assert(first_lines[23] == "{\"event\":\"first\",\"n\":23}");

    const auto second = create_session_directory(logs);
    assert(!second.empty() && second != first);
    {
        Writer writer(second);
        for (int i = 0; i < 25; ++i)
            assert(writer.append("{\"event\":\"second\",\"n\":" + std::to_string(i) + "}"));
    }
    assert(fs::file_size(first / "events.jsonl") > 0);
    std::ifstream preserved(first / "events.jsonl");
    std::vector<std::string> preserved_lines;
    while (std::getline(preserved, line)) preserved_lines.push_back(line);
    assert(preserved_lines == first_lines);
    std::ifstream later(second / "events.jsonl");
    std::size_t later_count = 0;
    while (std::getline(later, line)) ++later_count;
    assert(later_count == 25);

    // Simulate a process restart with the same time and counter: the claim
    // must skip a preexisting directory without modifying its old record.
    const auto collision = tooie::capture_archive::detail::claim_session_directory(
        logs, 123456789U, 0U);
    assert(!collision.empty());
    {
        Writer writer(collision);
        assert(writer.append("{\"event\":\"old\"}"));
    }
    const auto after_restart = tooie::capture_archive::detail::claim_session_directory(
        logs, 123456789U, 0U);
    assert(!after_restart.empty() && after_restart != collision);
    std::ifstream old(collision / "events.jsonl");
    assert(std::getline(old, line) && line == "{\"event\":\"old\"}");
    assert(!std::getline(old, line));

    // Cleanup only directories this test created; no profile paths are used.
    prior.close();
    preserved.close();
    later.close();
    old.close();
    fs::remove(first / "events.jsonl");
    fs::remove(second / "events.jsonl");
    fs::remove(collision / "events.jsonl");
    fs::remove(first);
    fs::remove(second);
    fs::remove(collision);
    fs::remove(after_restart);
    fs::remove(logs / "issue-captures");
    fs::remove(logs);
    fs::remove(root);
}
