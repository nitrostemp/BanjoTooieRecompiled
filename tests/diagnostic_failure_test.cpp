#include "game.hpp"
#include <thread>
#include <chrono>
#include <cstring>
extern "C" {
void boot_osInitialize(uint8_t*,recomp_context*);
void boot___osPiRawStartDma(uint8_t*,recomp_context*);
void boot___osDisableInt(uint8_t*,recomp_context*);
void boot___osRestoreInt(uint8_t*,recomp_context*);
void __osSiGetAccess_recomp(uint8_t*,recomp_context*);
void __osSiRawStartDma_recomp(uint8_t*,recomp_context*);
void __osSiRelAccess_recomp(uint8_t*,recomp_context*);
void osPfsInit_recomp(uint8_t*,recomp_context*);
void osViGetCurrentLine_recomp(uint8_t*,recomp_context*);
}
int main(int argc,char**argv) {
    if(argc<3)return 2;tooie::start_trace(argv[2]);recomp_context ctx{};
    struct Binding{const char*name;recomp_func_t*fn;};
    Binding bindings[]={
#define ENTRY(name) {#name,name}
        ENTRY(__osSiGetAccess_recomp),ENTRY(__osSiRawStartDma_recomp),ENTRY(__osSiRelAccess_recomp),ENTRY(osPfsInit_recomp),ENTRY(osViGetCurrentLine_recomp)
    };
    for(auto&b:bindings)if(!std::strcmp(argv[1],b.name)){b.fn(nullptr,&ctx);return 9;}
    if(!std::strcmp(argv[1],"recomp_syscall_handler")){recomp_syscall_handler(nullptr,&ctx,(int32_t)0x800ABCDE);return 9;}
    if(!std::strcmp(argv[1],"watchdog")&&argc==4) {
        tooie::validate_and_install_rom(argv[3]);tooie::run_boot_diagnostic();
        tooie::start_watchdog(1);std::this_thread::sleep_for(std::chrono::seconds(5));tooie::stop_watchdog();return 9;
    }
    return 2;
}
