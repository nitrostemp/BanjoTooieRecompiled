#include "sdl_audio_observation.hpp"
#include "platform_support.hpp"
#include "tooie_sdl_observer_abi.h"
#include "json/json.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace tooie::sdl_observation {
namespace {
constexpr uint64_t record_capacity=262144;
void require(bool value,const std::string& why){if(!value)throw std::runtime_error("SDL observation: "+why);}
#ifdef _WIN32
using Json=nlohmann::json;
struct Calibration{int64_t steady_before_ns,qpc,steady_after_ns,frequency;};
Calibration calibrate(){
    LARGE_INTEGER tick,frequency;require(QueryPerformanceFrequency(&frequency)!=0,"QPC frequency unavailable");
    auto before=std::chrono::steady_clock::now();require(QueryPerformanceCounter(&tick)!=0,"QPC unavailable");auto after=std::chrono::steady_clock::now();
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(before.time_since_epoch()).count(),tick.QuadPart,
        std::chrono::duration_cast<std::chrono::nanoseconds>(after.time_since_epoch()).count(),frequency.QuadPart};
}
Json json(const Calibration& c){return {{"steady_before_ns",c.steady_before_ns},{"qpc",c.qpc},{"steady_after_ns",c.steady_after_ns},{"frequency",c.frequency}};}
std::filesystem::path module_path(HMODULE module){
    std::wstring path(32768,L'\0');auto n=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    require(n>0&&n<path.size(),"Cannot resolve loaded SDL module path");path.resize(n);return std::filesystem::canonical(path);
}
template<class F>F resolve(HMODULE module,const char* name){auto address=GetProcAddress(module,name);require(address!=nullptr,std::string("Missing private export ")+name);return reinterpret_cast<F>(address);}
void control(int result,const char* operation){require(result==TOOIE_SDL_OK,std::string(operation)+" failed with status "+std::to_string(result));}
#endif
}
struct Session::Impl {
#ifdef _WIN32
    HMODULE module=nullptr;
    decltype(&TooieSDL_AudioObserveBegin) begin=nullptr;
    decltype(&TooieSDL_AudioObserveSnapshotAfterClose) snapshot=nullptr;
    decltype(&TooieSDL_AudioObserveRelease) release=nullptr;
    std::filesystem::path directory,module_file;
    std::string module_sha;
    std::thread::id owner;
    Calibration before{};
    bool began=false;
    bool finalization_attempted=false;
    ~Impl(){if(module&&!began)FreeLibrary(module);}
#endif
};
Session::Session()=default;
Session::~Session() noexcept {
    if(impl_)try{finish();}catch(const std::exception& e){std::fprintf(stderr,"SDL observation cleanup incomplete; retaining any live module/ring: %s\n",e.what());}
}
void Session::begin(const std::filesystem::path& directory,const void* anchor,const std::filesystem::path& expected_module,const std::string& expected_sha256){
    if(directory.empty())return;
    require(!impl_,"Session already configured");
#ifndef _WIN32
    (void)anchor;(void)expected_module;(void)expected_sha256;
    throw std::runtime_error("SDL observation currently requires Windows");
#else
    static_assert(sizeof(TooieSDL_Record)==128&&sizeof(TooieSDL_Snapshot)==56);
    auto next=std::make_unique<Impl>();next->owner=std::this_thread::get_id();
    require(anchor!=nullptr,"Null imported SDL anchor");
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(anchor),&next->module)!=0,"Cannot bind imported SDL module");
    next->module_file=module_path(next->module);
    require(_wcsicmp(next->module_file.c_str(),std::filesystem::canonical(expected_module).c_str())==0,"Loaded SDL path differs from expected sibling module");
    next->module_sha=platform::file_sha256(next->module_file);
    require(next->module_sha==expected_sha256,"Loaded SDL SHA256 differs from configured build identity");
    next->begin=resolve<decltype(next->begin)>(next->module,"TooieSDL_AudioObserveBegin");
    next->snapshot=resolve<decltype(next->snapshot)>(next->module,"TooieSDL_AudioObserveSnapshotAfterClose");
    next->release=resolve<decltype(next->release)>(next->module,"TooieSDL_AudioObserveRelease");
    next->directory=std::filesystem::absolute(directory);
    require(!std::filesystem::exists(next->directory),"Output directory must be new");
    std::filesystem::create_directories(next->directory.parent_path());
    require(std::filesystem::create_directory(next->directory),"Cannot create exclusive output directory");
    next->before=calibrate();
    control(next->begin(1,sizeof(TooieSDL_Record),record_capacity),"Begin");
    next->began=true;impl_=std::move(next);
#endif
}
Report Session::finish(){
    if(!impl_)return {};
#ifndef _WIN32
    return {};
#else
    auto& state=*impl_;require(state.owner==std::this_thread::get_id(),"Controls called from a different owner thread");
    require(!state.finalization_attempted,"Previous finalization failed; retained output/module must not be overwritten");
    state.finalization_attempted=true;
    TooieSDL_Snapshot summary{};const TooieSDL_Record* rows=nullptr;
    // Snapshot failure does not authorize freeing/unloading a potentially live ring.
    control(state.snapshot(1,sizeof(summary),&summary,&rows),"SnapshotAfterClose");
    std::exception_ptr error;Json final;Report report{true,summary.count,summary.attempted,summary.overflow};
    try{
        auto after=calibrate();
        require(summary.version==1&&summary.record_size==sizeof(TooieSDL_Record),"Snapshot ABI mismatch");
        require(summary.capacity==record_capacity&&summary.count<=record_capacity,"Snapshot capacity/count mismatch");
        require(summary.count==(summary.attempted<record_capacity?summary.attempted:record_capacity),"Snapshot retained count inconsistent");
        require(summary.overflow==(summary.attempted>record_capacity?summary.attempted-record_capacity:0),"Snapshot overflow inconsistent");
        require(summary.open_devices==0&&summary.qpc_frequency==uint64_t(after.frequency)&&after.frequency==state.before.frequency,"Snapshot device/clock contract mismatch");
        require(summary.count==0||rows!=nullptr,"Null snapshot records");
        std::ofstream file;file.exceptions(std::ios::badbit|std::ios::failbit);file.open(state.directory/"records.csv",std::ios::binary);
        file<<"ordinal,generation,begin_qpc,end_qpc,kind,device_id,thread_id,flags";for(int i=0;i<10;++i)file<<",v"<<i;file<<'\n';
        for(uint64_t i=0;i<summary.count;++i){const auto& r=rows[i];
            require(r.ordinal==i+1&&r.generation>0&&r.begin_qpc>0&&r.end_qpc>=r.begin_qpc,"Record ordinal/generation/timestamp invalid");
            require(r.kind>=1&&r.kind<=12&&r.flags==0,"Record schema invalid");
            if(r.kind==TOOIE_SDL_SOURCE_DRAIN)require(r.values[1]<=r.values[0]&&r.values[2]==r.values[0]-r.values[1],"Source drain conservation mismatch");
            file<<r.ordinal<<','<<r.generation<<','<<r.begin_qpc<<','<<r.end_qpc<<','<<r.kind<<','<<r.device_id<<','<<r.thread_id<<','<<r.flags;
            for(auto value:r.values)file<<','<<value;file<<'\n';
        }
        file.close();
        final={{"schema",1},{"private_abi",1},{"record_size",sizeof(TooieSDL_Record)},{"capacity",record_capacity},
            {"count",summary.count},{"attempted",summary.attempted},{"overflow",summary.overflow},{"open_devices",summary.open_devices},
            {"qpc_frequency",summary.qpc_frequency},{"module_path",state.module_file.string()},{"module_sha256",state.module_sha},
            {"process_id",GetCurrentProcessId()},{"executable_path",platform::executable_path().string()},
            {"calibration_before",json(state.before)},{"calibration_after",json(after)},
            {"records_sha256",platform::file_sha256(state.directory/"records.csv")},{"records_bytes",std::filesystem::file_size(state.directory/"records.csv")},
            {"status",summary.overflow?"complete_with_overflow":"complete"},{"all_attempted_records_retained",summary.overflow==0},
            {"caller_devices_closed_joined",true},{"physical_audio_acceptance",false}};
    }catch(...){error=std::current_exception();}
    int released=state.release();
    if(released!=TOOIE_SDL_OK){
        // Keep the module reference/ring alive; do not claim output finalized.
        throw std::runtime_error("SDL observation: Release failed with status "+std::to_string(released)+(error?" after serialization/validation failure":""));
    }
    state.began=false;
    auto directory=state.directory;impl_.reset(); // Release module only after successful private Release.
    if(error)std::rethrow_exception(error);
    final["observer_released"]=true;
    std::ofstream file;file.exceptions(std::ios::badbit|std::ios::failbit);file.open(directory/"summary.json",std::ios::binary);file<<final.dump(2)<<'\n';file.close();
    return report;
#endif
}
}
