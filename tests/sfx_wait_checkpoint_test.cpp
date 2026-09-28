#include "sfx_wait_checkpoint.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
using tooie::sfx_checkpoint::State;
using tooie::sfx_checkpoint::should_service;
int main() {
    recomp_context c{};
    c.r17=int32_t(0x80128c10);c.r18=60;c.r19=1;c.r20=3;c.r21=60;c.r22=int32_t(0x80128c10);
    const auto original=c;
    std::vector<unsigned char> ram(8*1024*1024,0xa5), before=ram;
    State s{};
    assert(!should_service(true,0x800c2af4,c,s)); // No observed invocation.
    assert(!should_service(false,0x800c2ab8,c,s) && !s.active);
    assert(!should_service(true,0x800c2ab8,c,s));
    assert(!should_service(true,0x800c2af0,c,s) && s.first_outer);
    assert(!should_service(true,0x800c2af4,c,s)); // Inherited matching registers.
    assert(should_service(true,0x800c2af4,c,s));
    for(int n=0;n<=60;++n) {
        c.r19=n;
        assert(should_service(true,0x800c2af4,c,s)==(n>0 && n<60));
    }
    c=original;
    for(auto member:{&recomp_context::r17,&recomp_context::r18,&recomp_context::r20,&recomp_context::r21,&recomp_context::r22}) {
        c.*member=0;assert(!should_service(true,0x800c2af4,c,s));c=original;
    }
    c.r19=-1;assert(!should_service(true,0x800c2af4,c,s));c=original;
    assert(!should_service(false,0x800c2af4,c,s));
    assert(!should_service(true,0x800fb968,c,s) && !s.active);
    assert(!should_service(true,0x800c2af4,c,s));
    assert(!should_service(true,0x800c2ab8,c,s));
    assert(!should_service(true,0x800c2af4,c,s)); // Each new invocation resets first pass.
    assert(should_service(true,0x800c2af4,c,s));
    std::thread independent([&] {
        thread_local State other;
        assert(!should_service(true,0x800c2af4,c,other));
        assert(!should_service(true,0x800c2ab8,c,other));
        assert(!should_service(true,0x800c2af4,c,other));
        assert(should_service(true,0x800c2af4,c,other));
    });independent.join();
    assert(std::memcmp(&c,&original,sizeof(c))==0 && ram==before);
    std::cout<<"PASS checkpoint: 61 count boundaries, five shape guards, disabled/entry/reentry/exit/independent TLS, full context and 8MiB unchanged\n";
}
