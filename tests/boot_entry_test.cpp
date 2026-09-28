#include "game.hpp"
#include <cassert>
int main(int argc,char** argv) {
    assert(argc==3);tooie::start_trace(argv[2]);
    auto identity=tooie::validate_and_install_rom(argv[1]);
    assert(identity.size==32u*1024*1024);
    assert(identity.sha1=="af1a89e12b638b8d82cc4c085c8e01d4cba03fb3");
    assert(tooie::run_boot_diagnostic());
}
