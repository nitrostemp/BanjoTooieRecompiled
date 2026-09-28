// Disposable-folder coverage for default-profile selection, locking and the
// one-time legacy migration. The profile parent is injected, so no real
// application-data folder is read or written.
#include "profile_location.hpp"
#include "platform_support.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace tooie::profile;

namespace {
int skipped = 0;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    require(bool(output), "fixture write failed");
}

// Relative path -> SHA-256 for every file; directories map to "<dir>".
std::map<std::string, std::string> tree(const fs::path& root) {
    std::map<std::string, std::string> result;
    if (!fs::exists(root)) return result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto relative = entry.path().lexically_relative(root).generic_string();
        result[relative] = entry.is_directory() ? "<dir>" : tooie::platform::file_sha256(entry.path());
    }
    return result;
}

void legacy_fixture(const fs::path& legacy) {
    write(legacy / "config/tooie_imgui.json", "{\"tooie_ui_scale\": 1.1}\n");
    write(legacy / "config/bt.n64.us.1.0.z64", std::string(4096, '\x37'));
    write(legacy / "config/mods/.keep", "");
    write(legacy / "saves/bt.n64.us.1.0.bin", std::string(2048, '\x5a'));
    write(legacy / "saves/bt.n64.us.1.0.bin.bak", std::string(2048, '\x5b'));
    write(legacy / "logs/session.jsonl", "{\"event\":\"frontend_exit\"}\n");
    write(legacy / "private-practice.tooie-state", "TOOIEST1 fixture");
    fs::create_directories(legacy / "devices/rt64");
}

Environment environment(const fs::path& parent, const fs::path& executable_dir) {
    Environment result;
    result.default_parent = parent;
    result.executable_dir = executable_dir;
    result.other_instance_running = [] { return false; };
    return result;
}

template <class F> std::string expect_blocked(F call) {
    try {
        call();
    } catch (const StartupBlocked& blocked) {
        return blocked.what();
    }
    throw std::runtime_error("Expected startup to be blocked");
}

bool staging_left(const fs::path& parent) {
    for (const auto& entry : fs::directory_iterator(parent))
        if (entry.path().filename().string().find(".migrating-") != std::string::npos) return true;
    return false;
}

// Denies listing one disposable folder so enumeration fails part-way.
void set_listing_denied(const fs::path& folder, bool denied) {
#ifdef _WIN32
    const std::string command = std::string("icacls \"") + folder.string() + "\" " +
        (denied ? "/deny *S-1-1-0:(RD)" : "/remove:d *S-1-1-0") + " >NUL 2>&1";
    std::system(command.c_str());
#else
    std::error_code ignored;
    fs::permissions(folder, denied ? fs::perms::none : fs::perms::owner_all, ignored);
#endif
}

bool listing_denied(const fs::path& folder) {
    std::error_code error;
    fs::directory_iterator probe(folder, error);
    return bool(error);
}

bool make_directory_link(const fs::path& link, const fs::path& target) {
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    if (!error) return true;
#ifdef _WIN32
    // Junctions need no symbolic-link privilege.
    const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >NUL";
    return std::system(command.c_str()) == 0 && fs::exists(fs::symlink_status(link));
#else
    return false;
#endif
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Requires a disposable test directory");
        const fs::path base = fs::absolute(argv[1]);
        // Recover from an interrupted earlier run of the enumeration-failure case.
        if (fs::exists(base))
            for (const auto& entry : fs::recursive_directory_iterator(
                     base, fs::directory_options::skip_permission_denied))
                if (entry.is_directory() && entry.path().filename() == "unreadable")
                    set_listing_denied(entry.path(), false);
        fs::remove_all(base);
        fs::create_directories(base);
        const auto executable_dir = base / "exe";
        fs::create_directories(executable_dir);
        int case_number = 0;
        const auto fresh = [&] {
            const auto parent = base / ("case-" + std::to_string(++case_number));
            fs::create_directories(parent);
            return parent;
        };

        { // New installation.
            const auto parent = fresh();
            auto result = resolve({}, environment(parent, executable_dir));
            require(result.origin == Origin::Created && result.root == parent / product_folder, "fresh profile");
            require(fs::is_regular_file(result.root / marker_file) && result.lock, "fresh marker/lock");
            require(!fs::exists(parent / legacy_folder), "fresh install created a legacy folder");
        }
        { // Legacy-only profile: verified copy, source untouched, reopen is stable.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            const auto before = tree(parent / legacy_folder);
            {
                auto result = resolve({}, environment(parent, executable_dir));
                require(result.origin == Origin::Migrated, "legacy profile was not migrated");
                require(result.legacy == parent / legacy_folder && result.files == 7, "migration counts");
                auto copied = tree(result.root);
                require(copied.erase(marker_file) && copied.erase(receipt_file), "migration marker/receipt");
                require(copied == before, "migrated contents differ from the legacy profile");
                require(!staging_left(parent), "staging folder left after migration");
            }
            require(tree(parent / legacy_folder) == before, "legacy profile changed by migration");
            auto again = resolve({}, environment(parent, executable_dir));
            require(again.origin == Origin::Existing && again.legacy == parent / legacy_folder, "reopen migrated");
        }
        { // Both present: the marked product profile wins; nothing is merged.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            write(parent / product_folder / marker_file, "{}\n");
            write(parent / product_folder / "saves/bt.n64.us.1.0.bin", "newer");
            const auto legacy_before = tree(parent / legacy_folder);
            const auto target_before = tree(parent / product_folder);
            auto result = resolve({}, environment(parent, executable_dir));
            require(result.origin == Origin::Existing, "existing profile not selected");
            require(tree(parent / legacy_folder) == legacy_before && tree(parent / product_folder) == target_before,
                "existing profiles were modified");
        }
        { // Unmarked destination with profile content and a legacy profile: conflict.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            write(parent / product_folder / "saves/bt.n64.us.1.0.bin", "copied by hand");
            const auto legacy_before = tree(parent / legacy_folder);
            const auto target_before = tree(parent / product_folder);
            const auto message = expect_blocked([&] { resolve({}, environment(parent, executable_dir)); });
            require(message.find("not merged") != std::string::npos, "conflict message");
            require(tree(parent / legacy_folder) == legacy_before && tree(parent / product_folder) == target_before,
                "conflicting profiles were modified");
        }
        { // Unmarked destination with profile content and no legacy: adopted.
            const auto parent = fresh();
            write(parent / product_folder / "config/tooie_imgui.json", "{}\n");
            auto result = resolve({}, environment(parent, executable_dir));
            require(result.origin == Origin::Adopted && fs::exists(result.root / marker_file), "adopt profile");
        }
        { // Unknown non-profile content is never adopted or migrated into.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            write(parent / product_folder / "notes.txt", "not a profile");
            expect_blocked([&] { resolve({}, environment(parent, executable_dir)); });
            require(!fs::exists(parent / product_folder / marker_file), "unknown folder was marked");
        }
        { // Empty destination folder is treated as absent.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            fs::create_directories(parent / product_folder);
            require(resolve({}, environment(parent, executable_dir)).origin == Origin::Migrated, "empty destination");
        }
        { // Copy failure: nothing activated, staging discarded, retry succeeds.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            const auto before = tree(parent / legacy_folder);
            auto failing = environment(parent, executable_dir);
            int copies = 0;
            failing.copy_file = [&](const fs::path& from, const fs::path& to) {
                if (++copies == 3) throw std::runtime_error("injected disk-full failure");
                fs::copy_file(from, to);
            };
            const auto message = expect_blocked([&] { resolve({}, failing); });
            require(message.find("injected disk-full failure") != std::string::npos, "copy failure message");
            require(!fs::exists(parent / product_folder) && !staging_left(parent), "failed copy left data");
            require(tree(parent / legacy_folder) == before, "failed copy changed legacy profile");
            require(resolve({}, environment(parent, executable_dir)).origin == Origin::Migrated, "retry after failure");
        }
        { // Corrupt copy (verification mismatch) is rejected.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            auto corrupting = environment(parent, executable_dir);
            corrupting.copy_file = [](const fs::path& from, const fs::path& to) {
                fs::copy_file(from, to);
                if (from.filename() == "bt.n64.us.1.0.bin") write(to, std::string(2048, '\0'));
            };
            expect_blocked([&] { resolve({}, corrupting); });
            require(!fs::exists(parent / product_folder) && !staging_left(parent), "corrupt copy activated");
        }
        { // Interrupted earlier migration: marked staging is discarded, unmarked folders are kept.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            const auto stale = parent / (std::string(product_folder) + ".migrating-1-2-3");
            write(stale / staging_marker_file, "partial");
            write(stale / "saves/bt.n64.us.1.0.bin", "partial");
            const auto foreign = parent / (std::string(product_folder) + ".migrating-not-ours");
            write(foreign / "keep.txt", "user data");
            require(resolve({}, environment(parent, executable_dir)).origin == Origin::Migrated, "resume migration");
            require(!fs::exists(stale) && fs::exists(foreign / "keep.txt"), "staging cleanup scope");
        }
        { // Source changes while copying: migration is abandoned.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            auto changing = environment(parent, executable_dir);
            changing.after_file_copied = [&](const fs::path& relative) {
                if (relative.filename() == "tooie_imgui.json")
                    write(parent / legacy_folder / "saves/bt.n64.us.1.0.bin", "written by another copy");
            };
            const auto message = expect_blocked([&] { resolve({}, changing); });
            require(message.find("changed while") != std::string::npos, "source change message");
            require(!fs::exists(parent / product_folder) && !staging_left(parent), "changed source activated");
        }
        { // Another running copy defers migration without touching either folder.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            const auto before = tree(parent / legacy_folder);
            auto busy = environment(parent, executable_dir);
            busy.other_instance_running = [] { return true; };
            const auto message = expect_blocked([&] { resolve({}, busy); });
            require(message.find("Close every other copy") != std::string::npos, "busy message");
            require(!fs::exists(parent / product_folder) && tree(parent / legacy_folder) == before, "busy migration");
        }
        { // Profile locks: contention across threads, including the legacy folder.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            // The helper thread holds the lock until the check finishes. On
            // Windows, ownership would otherwise pass on as an abandoned mutex,
            // exactly as it does for a relaunched child after its parent exits.
            const auto contend = [](const fs::path& root, auto&& call) {
                std::atomic<int> state{0};
                bool held = false;
                std::thread holder([&] {
                    Lock lock = try_lock(root);
                    held = bool(lock);
                    state = 1;
                    while (state.load() != 2) std::this_thread::yield();
                });
                while (state.load() == 0) std::this_thread::yield();
                try {
                    require(held, "helper thread could not take lock");
                    call();
                } catch (...) {
                    state = 2;
                    holder.join();
                    throw;
                }
                state = 2;
                holder.join();
            };
            contend(parent / product_folder, [&] {
                expect_blocked([&] { resolve({}, environment(parent, executable_dir)); });
            });
            require(!fs::exists(parent / product_folder), "locked resolve created a profile");
            contend(parent / legacy_folder, [&] {
                expect_blocked([&] { resolve({}, environment(parent, executable_dir)); });
            });
            require(!fs::exists(parent / product_folder) && !staging_left(parent), "legacy lock migration");
            const auto explicit_root = base / "explicit-locked";
            Lock first = try_lock(explicit_root);
            require(bool(first), "explicit lock");
            bool second = true;
            std::thread([&] { second = bool(try_lock(explicit_root / ".")); }).join();
            require(!second, "normalized path was not treated as the same profile");
            first = Lock{};
            require(bool(try_lock(explicit_root)), "released lock could not be reacquired");
#ifdef _WIN32
            const auto handoff = base / "relaunch-handoff";
            std::thread([&] { Lock parent_lock = try_lock(handoff); require(bool(parent_lock), "parent lock");
                parent_lock = Lock{}; Lock abandoned = try_lock(handoff); require(bool(abandoned), "parent relock");
                // Simulate process exit: the owning thread ends without releasing.
                new Lock(std::move(abandoned)); }).join();
            require(bool(try_lock(handoff)), "lock was not handed over after the owner exited");
#endif
            require(lock_key(base / "A") != lock_key(base / "B"), "distinct profiles share a lock");
#ifdef _WIN32
            require(lock_key(base / "Case") == lock_key(base / "CASE"), "Windows lock is case-sensitive");
#endif
        }
        { // Portable marker beside the executable, not the working directory.
            const auto parent = fresh();
            const auto portable_exe = base / "portable-exe";
            write(portable_exe / "portable.txt", "");
            auto portable = resolve({}, environment(parent, portable_exe));
            require(portable.origin == Origin::Portable && portable.root == portable_exe, "portable beside exe");
            portable = Resolution{};
            const auto working = base / "working-dir";
            write(working / "portable.txt", "");
            const auto previous = fs::current_path();
            fs::current_path(working);
            auto result = resolve({}, environment(parent, executable_dir));
            fs::current_path(previous);
            require(result.origin == Origin::Created && result.root == parent / product_folder,
                "working-directory portable.txt selected a portable profile");
        }
        { // Explicit --profile-dir: absolute, never migrated or marked.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            const auto previous = fs::current_path();
            fs::current_path(base);
            auto result = resolve("explicit-profile", environment(parent, executable_dir));
            fs::current_path(previous);
            require(result.origin == Origin::Explicit && result.root == base / "explicit-profile", "explicit root");
            require(!fs::exists(parent / product_folder) && !fs::exists(result.root / marker_file), "explicit migrated");
        }
        { // Linked items are reported, never followed or silently skipped.
            const auto parent = fresh();
            legacy_fixture(parent / legacy_folder);
            const auto outside = base / "outside-link-target";
            write(outside / "secret.txt", "outside");
            if (make_directory_link(parent / legacy_folder / "saves/linked", outside)) {
                const auto message = expect_blocked([&] { resolve({}, environment(parent, executable_dir)); });
                require(message.find("linked item") != std::string::npos, "link message");
                require(!fs::exists(parent / product_folder) && !staging_left(parent), "linked profile migrated");
                require(fs::exists(outside / "secret.txt"), "link target changed");
                fs::remove(parent / legacy_folder / "saves/linked");
            } else {
                ++skipped;
                std::cout << "SKIPPED: linked-item case (could not create a directory link here)\n";
            }
            const auto linked_parent = fresh();
            if (make_directory_link(linked_parent / legacy_folder, parent / legacy_folder)) {
                expect_blocked([&] { resolve({}, environment(linked_parent, executable_dir)); });
                require(!fs::exists(linked_parent / product_folder), "linked legacy folder migrated");
                fs::remove(linked_parent / legacy_folder);
            } else {
                ++skipped;
                std::cout << "SKIPPED: linked legacy-folder case (could not create a directory link here)\n";
            }
        }
        { // A folder that cannot be listed stops migration; no partial copy is activated.
            const auto parent = base / "case-enumeration";
            fs::create_directories(parent);
            legacy_fixture(parent / legacy_folder);
            const auto unreadable = parent / legacy_folder / "saves" / "unreadable";
            write(unreadable / "bt.n64.us.1.0.bin.cheat-reset-backup-1", "progress");
            set_listing_denied(unreadable, true);
            if (listing_denied(unreadable)) {
                std::string message;
                try {
                    message = expect_blocked([&] { resolve({}, environment(parent, executable_dir)); });
                } catch (...) {
                    set_listing_denied(unreadable, false);
                    throw;
                }
                const bool clean = !fs::exists(parent / product_folder) && !staging_left(parent);
                set_listing_denied(unreadable, false);
                require(message.find("could not be read completely") != std::string::npos, "enumeration message");
                require(clean, "partial enumeration activated or left a copy");
                auto recovered = resolve({}, environment(parent, executable_dir));
                require(recovered.origin == Origin::Migrated &&
                    fs::exists(recovered.root / "saves/unreadable/bt.n64.us.1.0.bin.cheat-reset-backup-1"),
                    "migration after restoring access");
            } else {
                set_listing_denied(unreadable, false);
                ++skipped;
                std::cout << "SKIPPED: enumeration-failure case (listing could not be denied here)\n";
            }
        }
        std::cout << "profile_location: all cases passed";
        if (skipped) std::cout << " (" << skipped << " skipped)";
        std::cout << '\n';
        return skipped ? 77 : 0;
    } catch (const std::exception& error) {
        std::cerr << "profile_location failed: " << error.what() << '\n';
        return 1;
    }
}
