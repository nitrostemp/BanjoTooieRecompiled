#pragma once

#include <filesystem>

namespace tooie {
void register_save_root(std::filesystem::path root);
std::filesystem::path runtime_save_folder(const std::filesystem::path& config_root);
}
