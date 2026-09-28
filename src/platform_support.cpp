#include "platform_support.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <openssl/evp.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <cerrno>
#include <openssl/evp.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
std::string hexadecimal(std::span<const uint8_t> bytes) {
    constexpr char digits[]="0123456789abcdef";
    std::string result(bytes.size()*2,'0');
    for (std::size_t i=0;i<bytes.size();++i) {
        result[i*2]=digits[bytes[i]>>4];
        result[i*2+1]=digits[bytes[i]&15];
    }
    return result;
}
#ifdef _WIN32
void crypto_check(NTSTATUS result,const char* action) {
    if (result<0) throw std::runtime_error(std::string(action)+" failed (NTSTATUS "+std::to_string(static_cast<uint32_t>(result))+")");
}
struct Algorithm {
    BCRYPT_ALG_HANDLE value=nullptr;
    explicit Algorithm(bool sha256) {
        crypto_check(BCryptOpenAlgorithmProvider(&value,sha256?BCRYPT_SHA256_ALGORITHM:BCRYPT_SHA1_ALGORITHM,nullptr,0),"BCryptOpenAlgorithmProvider");
    }
    ~Algorithm() { if(value) BCryptCloseAlgorithmProvider(value,0); }
    Algorithm(const Algorithm&)=delete;
    Algorithm& operator=(const Algorithm&)=delete;
};
struct Hasher {
    Algorithm algorithm;
    std::vector<uint8_t> object;
    BCRYPT_HASH_HANDLE handle=nullptr;
    ULONG output_size=0;
    explicit Hasher(bool sha256):algorithm(sha256) {
        ULONG object_size=0,received=0;
        crypto_check(BCryptGetProperty(algorithm.value,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&object_size),sizeof(object_size),&received,0),"BCryptGetProperty object length");
        if (received!=sizeof(object_size)) throw std::runtime_error("BCrypt object length has unexpected size");
        crypto_check(BCryptGetProperty(algorithm.value,BCRYPT_HASH_LENGTH,reinterpret_cast<PUCHAR>(&output_size),sizeof(output_size),&received,0),"BCryptGetProperty hash length");
        if (received!=sizeof(output_size) || output_size!=(sha256?32u:20u)) throw std::runtime_error("BCrypt digest length mismatch");
        object.resize(object_size);
        crypto_check(BCryptCreateHash(algorithm.value,&handle,object.data(),object_size,nullptr,0,0),"BCryptCreateHash");
    }
    ~Hasher() { if(handle) BCryptDestroyHash(handle); }
    Hasher(const Hasher&)=delete;
    Hasher& operator=(const Hasher&)=delete;
    void update(std::span<const uint8_t> bytes) {
        while (!bytes.empty()) {
            ULONG count=static_cast<ULONG>(std::min<std::size_t>(bytes.size(),std::numeric_limits<ULONG>::max()));
            crypto_check(BCryptHashData(handle,const_cast<PUCHAR>(bytes.data()),count,0),"BCryptHashData");
            bytes=bytes.subspan(count);
        }
    }
    std::string finish() {
        std::array<uint8_t,32> output{};
        crypto_check(BCryptFinishHash(handle,output.data(),output_size,0),"BCryptFinishHash");
        return hexadecimal(std::span(output).first(output_size));
    }
};
#else
struct Hasher {
    EVP_MD_CTX* context=nullptr;
    explicit Hasher(bool sha256):context(EVP_MD_CTX_new()) {
        if (!context) throw std::runtime_error("Digest allocation failed");
        if (EVP_DigestInit_ex(context,sha256?EVP_sha256():EVP_sha1(),nullptr)!=1) {
            EVP_MD_CTX_free(context);
            context=nullptr;
            throw std::runtime_error("Digest initialization failed");
        }
    }
    ~Hasher() { EVP_MD_CTX_free(context); }
    Hasher(const Hasher&)=delete;
    Hasher& operator=(const Hasher&)=delete;
    void update(std::span<const uint8_t> bytes) {
        if (!bytes.empty() && EVP_DigestUpdate(context,bytes.data(),bytes.size())!=1)
            throw std::runtime_error("Digest update failed");
    }
    std::string finish() {
        std::array<uint8_t,EVP_MAX_MD_SIZE> output{};
        unsigned int count=0;
        if (EVP_DigestFinal_ex(context,output.data(),&count)!=1) throw std::runtime_error("Digest finalization failed");
        return hexadecimal(std::span(output).first(count));
    }
};
#endif
}

namespace tooie::platform {
std::string digest(std::span<const uint8_t> bytes,bool sha256) {
    Hasher hash(sha256);
    hash.update(bytes);
    return hash.finish();
}
std::string file_sha256(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open hash input: "+path.string());
    Hasher hash(true);
    std::array<uint8_t,65536> buffer{};
    while (file.read(reinterpret_cast<char*>(buffer.data()),buffer.size()) || file.gcount())
        hash.update(std::span(buffer).first(static_cast<std::size_t>(file.gcount())));
    if (!file.eof()) throw std::runtime_error("Cannot read hash input: "+path.string());
    return hash.finish();
}
std::filesystem::path executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> path(1024);
    for (;;) {
        DWORD count=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if (!count) throw std::system_error(GetLastError(),std::system_category(),"GetModuleFileNameW");
        if (count<path.size()) return std::filesystem::path(std::wstring(path.data(),count));
        if (path.size()>=1024*1024) throw std::runtime_error("Executable path exceeds supported length");
        path.resize(path.size()*2);
    }
#elif defined(__APPLE__)
    uint32_t size=0;
    if (_NSGetExecutablePath(nullptr,&size)!=-1 || !size)
        throw std::runtime_error("_NSGetExecutablePath size query failed");
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(),&size)!=0)
        throw std::runtime_error("_NSGetExecutablePath failed");
    return std::filesystem::absolute(std::filesystem::path(path.data()));
#else
    std::vector<char> path(1024);
    for (;;) {
        ssize_t count=readlink("/proc/self/exe",path.data(),path.size());
        if (count<0) throw std::system_error(errno,std::generic_category(),"readlink executable");
        if (static_cast<std::size_t>(count)<path.size()) return std::filesystem::path(std::string(path.data(),count));
        if (path.size()>=1024*1024) throw std::runtime_error("Executable path exceeds supported length");
        path.resize(path.size()*2);
    }
#endif
}
#ifdef _WIN32
std::wstring frontend_command_line(const std::filesystem::path& executable,
    const std::filesystem::path& profile, FrontendLaunch mode) {
    // Quote Windows argv, including trailing backslashes in directory paths.
    const auto quote=[](const std::wstring& argument) {
        std::wstring result=L"\"";
        std::size_t slashes=0;
        for (wchar_t c:argument) {
            if (c==L'\\') { ++slashes; continue; }
            result.append(c==L'"'?slashes*2+1:slashes,L'\\');
            result+=c;
            slashes=0;
        }
        result.append(slashes*2,L'\\');
        return result+L'"';
    };
    const wchar_t* arguments=mode==FrontendLaunch::RestartGame?L" --restart-game --profile-dir "
        :mode==FrontendLaunch::PracticeGame?L" --practice-game --profile-dir "
        :L" --profile-dir ";
    return quote(executable.wstring())+arguments+quote(profile.wstring());
}
#endif

#ifdef _WIN32
void wait_for_frontend_parent(std::uint32_t process_id) {
    if (process_id == 0 || process_id == GetCurrentProcessId())
        throw std::invalid_argument("Invalid frontend parent process");
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (!parent) {
        const DWORD error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) return; // Parent already exited.
        throw std::system_error(error, std::system_category(), "Open frontend parent");
    }
    const DWORD waited = WaitForSingleObject(parent, 30000);
    const DWORD error = waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    CloseHandle(parent);
    if (waited != WAIT_OBJECT_0)
        throw std::system_error(error, std::system_category(), "Wait for frontend parent shutdown");
}
#endif

void launch_frontend(const std::filesystem::path& profile, FrontendLaunch mode) {
    if (profile.empty()) throw std::invalid_argument("Frontend relaunch requires a profile");
    const auto executable=executable_path();
    const auto absolute_profile=std::filesystem::absolute(profile);
#ifdef _WIN32
    auto command=frontend_command_line(executable,absolute_profile,mode);
    command += L" --wait-for-parent " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,0,
        nullptr,nullptr,&startup,&process))
        throw std::system_error(GetLastError(),std::system_category(),
            mode==FrontendLaunch::PracticeGame?"Start Private Practice":
                mode==FrontendLaunch::RestartGame?"Restart Game":"Return to Launcher");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    if (mode==FrontendLaunch::PracticeGame)
        execl(executable.c_str(),executable.c_str(),"--practice-game","--profile-dir",absolute_profile.c_str(),nullptr);
    else if (mode==FrontendLaunch::RestartGame)
        execl(executable.c_str(),executable.c_str(),"--restart-game","--profile-dir",absolute_profile.c_str(),nullptr);
    else
        execl(executable.c_str(),executable.c_str(),"--profile-dir",absolute_profile.c_str(),nullptr);
    throw std::system_error(errno,std::generic_category(),
        mode==FrontendLaunch::PracticeGame?"Start Private Practice":
            mode==FrontendLaunch::RestartGame?"Restart Game":"Return to Launcher");
#endif
}

uint8_t* reserve_rdram(std::size_t reservation_size,std::size_t accessible_size) {
    if (!reservation_size || !accessible_size || accessible_size>reservation_size)
        throw std::invalid_argument("Invalid RDRAM reservation/access sizes");
#ifdef _WIN32
    void* reservation=VirtualAlloc(nullptr,reservation_size,MEM_RESERVE,PAGE_NOACCESS);
    if (!reservation) throw std::system_error(GetLastError(),std::system_category(),"RDRAM reserve");
    if (!VirtualAlloc(reservation,accessible_size,MEM_COMMIT,PAGE_READWRITE)) {
        DWORD error=GetLastError();
        VirtualFree(reservation,0,MEM_RELEASE);
        throw std::system_error(error,std::system_category(),"RDRAM commit accessible region");
    }
#else
    void* reservation=mmap(nullptr,reservation_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (reservation==MAP_FAILED) throw std::system_error(errno,std::generic_category(),"RDRAM reserve");
    if (mprotect(reservation,accessible_size,PROT_READ|PROT_WRITE)) {
        int error=errno;
        munmap(reservation,reservation_size);
        throw std::system_error(error,std::generic_category(),"RDRAM protect accessible region");
    }
#endif
    return static_cast<uint8_t*>(reservation);
}
void release_rdram(uint8_t* address,std::size_t reservation_size) {
    if (!address || !reservation_size) throw std::invalid_argument("Invalid RDRAM release arguments");
#ifdef _WIN32
    if (!VirtualFree(address,0,MEM_RELEASE)) throw std::system_error(GetLastError(),std::system_category(),"RDRAM release");
#else
    if (munmap(address,reservation_size)) throw std::system_error(errno,std::generic_category(),"RDRAM release");
#endif
}

ProcessMemory process_memory() noexcept {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return {static_cast<std::uint64_t>(counters.PrivateUsage), ProcessMemoryKind::PrivateBytes};
    }
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return {static_cast<std::uint64_t>(info.phys_footprint), ProcessMemoryKind::PhysicalFootprint};
    }
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total_pages = 0, resident_pages = 0;
    if (statm >> total_pages >> resident_pages) {
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size > 0 && resident_pages <= std::numeric_limits<std::uint64_t>::max() /
                static_cast<std::uint64_t>(page_size)) {
            return {resident_pages * static_cast<std::uint64_t>(page_size), ProcessMemoryKind::ResidentSet};
        }
    }
#endif
    return {};
}
}
