#include "game.hpp"
#include "core1_bridge.hpp"
#include "coverage_metadata.hpp"
#include "librecomp/overlays.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <crtdbg.h>
#else
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
extern "C" void recomp_entrypoint(uint8_t*,recomp_context*);
extern "C" void chweldarbossdoors_entrypoint_0(uint8_t*,recomp_context*);
extern "C" void chweldarbossfireball_entrypoint_0(uint8_t*,recomp_context*);
#ifdef _WIN32
static int child_check=-1;
static int next_check=0;
static void missing(uint32_t address) {
    const int check=next_check++;
    if(child_check>=0) {
        if(check!=child_check)return;
        std::fprintf(stderr,"MISSING_CHECK %d %08X\n",check,address);
        std::fflush(stderr);
        get_function((int32_t)address);
        // A lookup that unexpectedly succeeds is always a failed death test.
        std::_Exit(0);
    }
    // Windows has no fork: replay this executable through the exact checkpoint.
    // Earlier negative checks are skipped in the child; all intervening registry
    // operations and assertions run, including boot-retirement state changes.
    wchar_t path[32768];
    DWORD length=GetModuleFileNameW(nullptr,path,32768);
    assert(length>0 && length<32768);
    std::wstring command=L"\""+std::wstring(path,length)+L"\" --missing-check "+std::to_wstring(check);
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE read_pipe=nullptr,write_pipe=nullptr;
    assert(CreatePipe(&read_pipe,&write_pipe,&security,4096));
    assert(SetHandleInformation(read_pipe,HANDLE_FLAG_INHERIT,0));
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    assert(input!=INVALID_HANDLE_VALUE);
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=input;
    startup.hStdOutput=write_pipe;
    startup.hStdError=write_pipe;
    PROCESS_INFORMATION process{};
    const BOOL created=CreateProcessW(path,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);
    CloseHandle(write_pipe);
    CloseHandle(input);
    assert(created);
    const DWORD wait=WaitForSingleObject(process.hProcess,10000);
    if(wait!=WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess,90);
        WaitForSingleObject(process.hProcess,1000);
    }
    DWORD status=0;
    assert(GetExitCodeProcess(process.hProcess,&status));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::string output;
    char buffer[1024];DWORD count=0;
    while(ReadFile(read_pipe,buffer,sizeof(buffer),&count,nullptr)&&count)output.append(buffer,count);
    CloseHandle(read_pipe);
    char marker[96],fault[96];
    std::snprintf(marker,sizeof(marker),"MISSING_CHECK %d %08X",check,address);
    std::snprintf(fault,sizeof(fault),"Failed to find function at 0x%08X",address);
    // Release runtime exits 1; Windows CRT abort is 3 or STATUS_FATAL_APP_EXIT.
    // Neither an arbitrary abnormal exit nor a failure before this lookup counts.
    const bool expected=(status==1 || status==3 || status==0x40000015UL);
    if(wait!=WAIT_OBJECT_0 || !expected || output.find(marker)==std::string::npos || output.find(fault)==std::string::npos)
        std::cerr<<"Missing lookup subprocess mismatch: check="<<check<<" status="<<status<<" wait="<<wait<<"\n"<<output;
    assert(wait==WAIT_OBJECT_0 && expected);
    assert(output.find(marker)!=std::string::npos && output.find(fault)!=std::string::npos);
}
#else
static void missing(uint32_t address) {
    pid_t child=fork(); assert(child>=0);
    if(child==0) { rlimit limit{0,0};setrlimit(RLIMIT_CORE,&limit);get_function((int32_t)address);_exit(0); }
    int status=0;assert(waitpid(child,&status,0)==child);
    assert((WIFSIGNALED(status)&&WTERMSIG(status)==SIGABRT)||(WIFEXITED(status)&&WEXITSTATUS(status)==1));
}
#endif
int main(int argc,char**argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    if(argc==3 && !std::strcmp(argv[1],"--missing-check")) {
        char* end=nullptr;
        long value=std::strtol(argv[2],&end,10);
        if(!end || *end || value<0 || value>7)return 2;
        child_check=(int)value;
    } else if(argc!=1)return 2;
#else
    if(argc!=1)return 2;
    (void)argv;
#endif
    tooie::register_overlays();recomp::overlays::init_overlays();
    // The selected corpus supplies this count, including the core1 adapter;
    // generated direct-call-only helpers are not registry table entries.
    assert(tooie::registered_function_count()==tooie::coverage_meta::expected_registry_entries);
    const auto fixture=tooie::registered_section_index(350);
    load_overlays(0x1000,(int32_t)0x80000400,0x100000);
    assert(get_function((int32_t)0x80000400)==recomp_entrypoint);
    assert(section_addresses[0]==(int32_t)0x80000400);
    load_overlays(0x01E29B60,(int32_t)0x80012030,0x31350);
    assert(get_function((int32_t)0x8002D7B0)==tooie_core1_osInitialize);
    // 1 and 170 are original empty slots; 0 and 885 are out of range.
    // 349 was the old uncompiled-ID negative case and is now compiled.
    for(uint32_t id:{0u,1u,885u,170u}) {
        bool rejected=false;try{tooie::checked_load_overlay(id,0x80307000);}catch(const std::exception&){rejected=true;}assert(rejected);
    }
    const auto added_fixture=tooie::registered_section_index(349);
    tooie::checked_load_overlay(349,0x80320000);
    assert(section_addresses[added_fixture]==(int32_t)0x80320000);
    assert(get_function((int32_t)0x80320000)==chweldarbossdoors_entrypoint_0);
    tooie::checked_unload_overlay(349);
    assert(section_addresses[added_fixture]==(int32_t)0x80800000);
    // Allocation begins at 0x80306FB0, but the API must receive text at +0x50.
    missing(0x80306FB0);
    tooie::checked_load_overlay(350,0x80307000);
    assert(section_addresses[fixture]==(int32_t)0x80307000);
    assert(get_function((int32_t)0x80307000)==chweldarbossfireball_entrypoint_0);
    missing(0x80306FB0);
    // Pinned API: absolute first load, signed delta on subsequent load.
    tooie::checked_move_overlay(350,0x8FF0);
    assert(section_addresses[fixture]==(int32_t)0x8030FFF0);
    assert(get_function((int32_t)0x8030FFF0)==chweldarbossfireball_entrypoint_0);
    missing(0x80307000);
    tooie::checked_move_overlay(350,-0xE000);
    assert(section_addresses[fixture]==(int32_t)0x80301FF0);
    assert(get_function((int32_t)0x80301FF0)==chweldarbossfireball_entrypoint_0);
    missing(0x8030FFF0);
    tooie::checked_unload_overlay(350);
    assert(section_addresses[fixture]==(int32_t)0x80800000);missing(0x80301FF0);
    tooie::checked_load_overlay(350,0x8022FFF0);
    assert(get_function((int32_t)0x8022FFF0)==chweldarbossfireball_entrypoint_0);
    tooie::checked_unload_overlay(350);missing(0x8022FFF0);
    assert(get_function((int32_t)0x80000400)==recomp_entrypoint);
    std::cout<<"PASS actual runtime: selected table entry count; boot; invalid IDs; text base; absolute/delta moves; old lookup invalidation; unload/reload\n";
    tooie::retire_boot_mappings();
    missing(0x80000400);missing(0x80000450);
    assert(get_function((int32_t)0x8002D7B0)==tooie_core1_osInitialize);
    bool duplicate=false;try{tooie::retire_boot_mappings();}catch(const std::logic_error&){duplicate=true;}
    assert(duplicate);
#ifdef _WIN32
    // Eight negative lookups, including both retired boot entry addresses.
    assert(next_check==8);
    if(child_check>=0)return 2; // Requested checkpoint must have terminated above.
#endif
}
