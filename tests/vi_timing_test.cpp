#include "vi_timing.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
}

int main() {
    using tooie::vi::line_for_elapsed;
    constexpr uint64_t frame=1'000'000'000ull/60;
    try {
        require(line_for_elapsed(0,1)==0,"Frame start is not line zero");
        require(line_for_elapsed(frame-1,1)==524,"Frame end is not line 524");
        require(line_for_elapsed(frame,1)==0,"60 Hz frame does not wrap");
        require(line_for_elapsed(frame+frame/2,1)==262,"Mid-frame line is wrong");
        require(line_for_elapsed(frame/2,2)==0,"Speed multiplier does not shorten the frame");
        require(line_for_elapsed(frame/2-1,2)==524,"Accelerated frame end is wrong");
        require(line_for_elapsed(123,0)==0,"Zero speed must stay safe");
        std::cout<<"PASS VI timing: 60 Hz range/wrap and speed phase boundaries\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<'\n'; return 1; }
}
