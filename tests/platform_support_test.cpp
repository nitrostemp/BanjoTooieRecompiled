#include "platform_support.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F call) {
    bool rejected=false;
    try { call(); } catch(const std::exception&) { rejected=true; }
    require(rejected,"Invalid operation unexpectedly succeeded");
}
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::runtime_error("Requires a test output directory");
        namespace platform=tooie::platform;
        require(platform::digest({},false)=="da39a3ee5e6b4b0d3255bfef95601890afd80709","Empty SHA1 mismatch");
        require(platform::digest({},true)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","Empty SHA256 mismatch");
        const std::vector<uint8_t> abc={'a','b','c'};
        require(platform::digest(abc,false)=="a9993e364706816aba3e25717850c26c9cd0d89d","abc SHA1 mismatch");
        require(platform::digest(abc,true)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","abc SHA256 mismatch");
        std::filesystem::path directory=argv[1];
        std::filesystem::create_directories(directory);
        std::vector<uint8_t> content(3*65536+17);
        for(std::size_t i=0;i<content.size();++i) content[i]=static_cast<uint8_t>(i*37+11);
        auto file_path=directory/"streaming-hash.bin";
        { std::ofstream file(file_path,std::ios::binary); file.write(reinterpret_cast<const char*>(content.data()),content.size()); require(bool(file),"Fixture write failed"); }
        require(platform::file_sha256(file_path)==platform::digest(content,true),"Streaming file SHA256 mismatch");
        rejects([&]{platform::file_sha256(directory/"missing-input.bin");});
        auto executable=platform::executable_path();
        require(executable.is_absolute() && std::filesystem::is_regular_file(executable),"Executable path is not a real absolute file");
        require(platform::file_sha256(executable).size()==64,"Executable file could not be hashed");
        // A large guard reservation must not commit the entire address space.
        constexpr std::size_t accessible=8*1024*1024,reserved=std::size_t(4)*1024*1024*1024;
        auto* memory=platform::reserve_rdram(reserved,accessible);
        for(std::size_t i=0;i<accessible;++i) require(memory[i]==0,"RDRAM was not initially zero");
        memory[0]=0x39;memory[accessible-1]=0xa7;
        require(memory[0]==0x39 && memory[accessible-1]==0xa7,"RDRAM accessible interval not writable");
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION committed{},guard{};
        require(VirtualQuery(memory,&committed,sizeof(committed))==sizeof(committed),"VirtualQuery committed failed");
        require(VirtualQuery(memory+accessible,&guard,sizeof(guard))==sizeof(guard),"VirtualQuery guard failed");
        require(committed.State==MEM_COMMIT && committed.RegionSize==accessible,"Wrong Windows committed extent");
        require(guard.State==MEM_RESERVE,"Guard reservation was committed");
        const auto executable_path=std::filesystem::path(L"C:\\Games\\Banjo Tooie\\TooieRecompiled.exe");
        const auto unicode_profile=std::filesystem::path(L"C:\\Players\\Pok\u00e9mon \"Tooie\"\\");
        require(platform::frontend_command_line(executable_path,unicode_profile,
            platform::FrontendLaunch::RestartGame)==
            L"\"C:\\Games\\Banjo Tooie\\TooieRecompiled.exe\" --restart-game --profile-dir \"C:\\Players\\Pok\u00e9mon \\\"Tooie\\\"\\\\\"",
            "Restart command line lost Unicode or Windows quoting");
        require(platform::frontend_command_line(executable_path,unicode_profile,
            platform::FrontendLaunch::Launcher)==
            L"\"C:\\Games\\Banjo Tooie\\TooieRecompiled.exe\" --profile-dir \"C:\\Players\\Pok\u00e9mon \\\"Tooie\\\"\\\\\"",
            "Launcher command line unexpectedly auto-starts or loses profile quoting");
#else
        pid_t child=fork();
        require(child>=0,"Guard probe fork failed");
        if(child==0) {
            rlimit no_core{0,0}; setrlimit(RLIMIT_CORE,&no_core);
            volatile uint8_t byte=memory[accessible]; (void)byte; _exit(1);
        }
        int status=0;
        require(waitpid(child,&status,0)==child,"Guard probe wait failed");
        require(WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV,"Guard page unexpectedly readable");
#endif
        platform::release_rdram(memory,reserved);
        rejects([&]{platform::reserve_rdram(0,4096);});
        rejects([&]{platform::reserve_rdram(4096,0);});
        rejects([&]{platform::reserve_rdram(4096,8192);});
        rejects([&]{platform::release_rdram(nullptr,4096);});
        rejects([&]{platform::release_rdram(reinterpret_cast<uint8_t*>(1),0);});
        std::cout<<"PASS platform support: SHA1/SHA256 vectors, streaming files, executable identity, 4GiB reserve/8MiB accessible, zero initialization, guards, release, rejection edges\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<'\n';return 1; }
}
