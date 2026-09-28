#include "runtime_save_root.hpp"

#include <cassert>

int main() {
    using std::filesystem::path;
    assert(tooie::runtime_save_folder(path{"config"}) == path{"config"} / "saves");
    tooie::register_save_root(path{"profile"} / "saves");
    assert(tooie::runtime_save_folder(path{"config"}) == path{"profile"} / "saves");
}
