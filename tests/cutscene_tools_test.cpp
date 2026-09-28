#include "cutscene_tools.hpp"
#include <cassert>

int main() {
    assert(tooie_cutscene_skip_input(0) == 0);
    assert(tooie_cutscene_skip_input(1) == 1);
    tooie::cutscene_tools::request_skip();
    assert(tooie_cutscene_skip_input(0) == 1);
    assert(tooie_cutscene_skip_input(0) == 0);
    tooie::cutscene_tools::request_skip();
    tooie::cutscene_tools::reset();
    assert(tooie_cutscene_skip_input(0) == 0);
}
