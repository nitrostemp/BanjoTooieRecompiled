#include "runtime_save_root.hpp"

#include <mutex>

namespace {
std::mutex save_root_mutex;
std::filesystem::path save_root;
}

void tooie::register_save_root(std::filesystem::path root) {
    std::lock_guard lock(save_root_mutex);
    save_root = std::move(root);
}

std::filesystem::path tooie::runtime_save_folder(const std::filesystem::path& config_root) {
    std::lock_guard lock(save_root_mutex);
    return save_root.empty() ? config_root / "saves" : save_root;
}
