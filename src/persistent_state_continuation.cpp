#include "persistent_state_continuation.hpp"
#include "persistent_state_hooks.h"
#include "platform_support.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using Bytes=std::vector<std::uint8_t>;
using tooie::continuation::Frame;
using tooie::continuation::Identity;
thread_local tooie::continuation::Machine* bound=nullptr;
constexpr std::array<std::uint8_t,8> magic={'T','P','C','O','N','T','0','1'};
constexpr std::uint32_t schema=3, proof_only_kind=1, no_save_medium_policy=1;
constexpr std::size_t max_file=16*1024*1024, max_frames=1024;
void require(bool ok,const char* what) {
    if(!ok) throw std::runtime_error(std::string("Continuation proof: ")+what);
}
void u32(Bytes& b,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) b.push_back(static_cast<std::uint8_t>(value>>(i*8)));
}
void u64(Bytes& b,std::uint64_t value) {
    for(unsigned i=0;i<8;++i) b.push_back(static_cast<std::uint8_t>(value>>(i*8)));
}
void text(Bytes& b,const std::string& value) {
    require(!value.empty()&&value.size()<=256,"invalid identity string");
    u32(b,static_cast<std::uint32_t>(value.size()));
    b.insert(b.end(),value.begin(),value.end());
}
struct Reader {
    std::span<const std::uint8_t> b;
    std::span<const std::uint8_t> take(std::size_t n) {
        require(n<=b.size(),"truncated state");
        auto result=b.first(n);b=b.subspan(n);return result;
    }
    std::uint32_t u32() {
        auto v=take(4);std::uint32_t value=0;
        for(unsigned i=0;i<4;++i)value|=std::uint32_t(v[i])<<(i*8);
        return value;
    }
    std::uint64_t u64() {
        auto v=take(8);std::uint64_t value=0;
        for(unsigned i=0;i<8;++i)value|=std::uint64_t(v[i])<<(i*8);
        return value;
    }
    std::string text() {
        auto n=u32();require(n&&n<=256,"identity length outside bounds");
        auto v=take(n);return {reinterpret_cast<const char*>(v.data()),v.size()};
    }
};
#define EACH_REG(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) \
 X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
 X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
 X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
void context_out(Bytes& b,const recomp_context& c) {
    require(c.mips3_float_mode==0&&c.f_odd==&c.f0.u32h,"unsupported FPR alias/mode");
#define GPR_OUT(N) u64(b,c.r##N);
    EACH_REG(GPR_OUT)
#undef GPR_OUT
#define FPR_OUT(N) u64(b,c.f##N.u64);
    EACH_REG(FPR_OUT)
#undef FPR_OUT
    u64(b,c.hi);u64(b,c.lo);u32(b,c.status_reg);u32(b,c.mips3_float_mode);
}
void context_in(Reader& r,recomp_context& c) {
#define GPR_IN(N) c.r##N=r.u64();
    EACH_REG(GPR_IN)
#undef GPR_IN
#define FPR_IN(N) c.f##N.u64=r.u64();
    EACH_REG(FPR_IN)
#undef FPR_IN
    c.hi=r.u64();c.lo=r.u64();c.status_reg=r.u32();
    require(r.u32()==0,"unsupported serialized FPR mode");
    c.mips3_float_mode=0;c.f_odd=&c.f0.u32h;
}
#undef EACH_REG
void proof_path(const std::filesystem::path& path) {
    require(path.extension()==".tstate-probe","only proof-specific .tstate-probe files allowed");
    require(std::filesystem::is_directory(path.parent_path()),"state parent must already exist");
}
void atomic_write(const std::filesystem::path& path,std::span<const std::uint8_t> bytes) {
    proof_path(path);
    static std::atomic_uint64_t sequence{0};
#ifdef _WIN32
    auto temporary=path;
    temporary+=L".part-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence);
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Create state temporary");
    try {
        DWORD written=0;
        if(!WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size())
            throw std::system_error(GetLastError(),std::system_category(),"Write state temporary");
        if(!FlushFileBuffers(file))throw std::system_error(GetLastError(),std::system_category(),"Flush state temporary");
        if(!CloseHandle(file))throw std::system_error(GetLastError(),std::system_category(),"Close state temporary");
        file=INVALID_HANDLE_VALUE;
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(),std::system_category(),"Publish state atomically");
    } catch(...) {
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        DeleteFileW(temporary.c_str()); // Only the exact temporary created above.
        throw;
    }
#else
    auto temporary=path;
    temporary+=".part-"+std::to_string(getpid())+"-"+std::to_string(++sequence);
    int file=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
    if(file<0)throw std::system_error(errno,std::generic_category(),"Create state temporary");
    try {
        auto remaining=bytes;
        while(!remaining.empty()) {
            auto n=write(file,remaining.data(),remaining.size());
            if(n<0&&errno==EINTR)continue;
            if(n<=0)throw std::system_error(errno,std::generic_category(),"Write state temporary");
            remaining=remaining.subspan(static_cast<std::size_t>(n));
        }
        if(fsync(file))throw std::system_error(errno,std::generic_category(),"Flush state temporary");
        if(close(file))throw std::system_error(errno,std::generic_category(),"Close state temporary");
        file=-1;
        if(rename(temporary.c_str(),path.c_str()))throw std::system_error(errno,std::generic_category(),"Publish state atomically");
        int directory=open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY);
        if(directory<0)throw std::system_error(errno,std::generic_category(),"Open state directory for flush");
        int result=fsync(directory);int error=errno;close(directory);
        if(result)throw std::system_error(error,std::generic_category(),"Flush state directory");
    } catch(...) {
        if(file>=0)close(file);
        unlink(temporary.c_str());
        throw;
    }
#endif
}
Bytes read_file(const std::filesystem::path& path) {
    proof_path(path);
    const auto size=std::filesystem::file_size(path);
    require(size>=80&&size<=max_file,"state size outside bounds");
    Bytes data(static_cast<std::size_t>(size));
    std::ifstream in(path,std::ios::binary);
    require(bool(in),"cannot open state");
    in.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size()));
    require(in.gcount()==static_cast<std::streamsize>(data.size()),"short state read");
    require(in.peek()==std::char_traits<char>::eof(),"state changed during read");
    return data;
}
}

namespace tooie::continuation {
Machine::Machine(std::span<const Function> functions,bool tracking_only):memory(tracking_only?0:memory_bytes),functions_(functions),tracking_only_(tracking_only) {
    context.f_odd=&context.f0.u32h;
    require(std::endian::native==std::endian::little,"guest word-swapped memory requires little-endian host");
    require(!functions.empty(),"empty function registry");
    functions_by_id_.reserve(functions.size());function_pointers_.reserve(functions.size());
    for(std::size_t i=0;i<functions.size();++i) {
        require(functions[i].id&&functions[i].entry,"invalid function registry");
        require(functions[i].local_words>=4&&functions[i].local_words<=128,"invalid function local schema");
        require(functions_by_id_.emplace(functions[i].id,&functions[i]).second,"duplicate function identity");
        function_pointers_.insert(functions[i].entry);
    }
}
const Function& Machine::function(std::uint32_t id) const {
    auto it=functions_by_id_.find(id);
    require(it!=functions_by_id_.end(),"unknown function identity");return *it->second;
}
void Machine::validate_frames(std::span<const Frame> values) const {
    require(!values.empty()&&values.size()<=max_frames,"invalid continuation depth");
    for(const auto& frame:values) {
        const auto& f=function(frame.function);
        require(frame.pc&&std::find(f.allowed_pcs.begin(),f.allowed_pcs.end(),frame.pc)!=f.allowed_pcs.end(),
            "unknown continuation label");
        require(frame.extra_locals.size()<=124,"too many generated native locals");
        require(frame.extra_locals.size()==f.local_words-4,"serialized locals differ from function schema");
    }
}
Machine& Machine::current(){require(bound!=nullptr,"generated function entered outside bound machine");return *bound;}
Machine* Machine::current_if_bound() noexcept{return bound;}
recomp_func_t* Machine::validate_function_pointer(recomp_func_t* entry) const {
    if(tracking_only_) {
        if(!function_pointers_.contains(entry))const_cast<Machine*>(this)->top().pc=0;
        return entry;
    }
    require(function_pointers_.contains(entry),
        "indirect call target has no continuation contract");
    return entry;
}
Frame& Machine::top(){require(!frames.empty(),"empty continuation stack");return frames.back();}
Frame& Machine::enter(std::uint32_t id) {
    function(id);
    if(dispatching_) {
        dispatching_=false;
        require(top().function==id,"resume dispatch function mismatch");
        return top();
    }
    require(frames.size()<max_frames,"continuation stack limit");
    frames.push_back(Frame{id});++entries;
    maximum_depth=std::max(maximum_depth,frames.size());return top();
}
void Machine::bind_tracking(std::uint8_t* rdram,recomp_context* guest_context) {
    require(tracking_only_&&bound==nullptr&&rdram&&guest_context,"invalid live tracking binding");
    borrowed_rdram_=rdram;borrowed_context_=guest_context;bound=this;
}
void Machine::unbind_tracking() noexcept {
    if(bound==this)bound=nullptr;
    borrowed_rdram_=nullptr;borrowed_context_=nullptr;
}
void Machine::native_dependency(const char* symbol) {
    require(std::strcmp(symbol,"INVALID_CONTINUATION_PC")!=0,"invalid generated continuation PC");
    // Native calls are admitted for live execution, not automatic capture.
    // The lifter invalidates unknown native call boundaries; the whole-machine
    // coordinator separately requires every active frame to be resumable.
    if(tracking_only_) {++native_dependencies;return;}
    throw std::runtime_error(std::string("Continuation native contract missing: ")+symbol);
}
void Machine::leave(std::uint32_t id) {
    require(!yielded_&&top().function==id,"invalid continuation return");
    frames.pop_back();
}
void Machine::resume_borrowed(std::span<const Frame> values,const recomp_context& saved) {
    require(tracking_only_&&bound==this&&borrowed_rdram_&&borrowed_context_,"borrowed restore is not bound");
    validate_frames(values);
    frames.assign(values.begin(),values.end());
    *borrowed_context_=saved;borrowed_context_->f_odd=&borrowed_context_->f0.u32h;
    yielded_=false;yield_pc_=0;
    while(!frames.empty()) {
        const auto count=frames.size();const auto id=frames.back().function;
        dispatching_=true;
        try {function(id).entry(borrowed_rdram_,borrowed_context_);}
        catch(...) {dispatching_=false;throw;}
        require(!dispatching_&&frames.size()<count,"borrowed resume failed to retire frame");
    }
}
bool Machine::checkpoint(std::uint32_t pc) {
    if(tracking_only_)return false;
    if(pc!=yield_pc_||!yield_pc_)return false;
    require(top().pc==pc,"checkpoint was not staged");
    yielded_=true;yield_pc_=0;return true;
}
void Machine::dispatch(std::uint32_t id,bool resume) {
    require(!tracking_only_,"live tracking cannot dispatch isolated execution");
    require(bound==nullptr,"nested machine binding");
    require(std::fegetround()==FE_TONEAREST,"proof supports only round-to-nearest host mode");
    bound=this;dispatching_=resume;
    try {function(id).entry(memory.data(),&context);}
    catch(...) {bound=nullptr;dispatching_=false;throw;}
    bound=nullptr;
    require(!dispatching_,"function did not consume resume dispatch");
}
void Machine::start(std::uint32_t id,std::uint32_t yield_pc) {
    require(frames.empty()&&!yielded_,"start requires an empty machine");
    yield_pc_=yield_pc;dispatch(id,false);
    require(yielded_||frames.empty(),"function leaked a continuation");
    if(yielded_)validate_frames(frames);
}
void Machine::resume(std::uint32_t yield_pc) {
    require(yielded_,"resume requires a suspended machine");
    validate_frames(frames);yielded_=false;yield_pc_=yield_pc;
    while(!frames.empty()) {
        auto count=frames.size();dispatch(frames.back().function,true);
        if(yielded_) {validate_frames(frames);return;}
        require(frames.size()<count,"resumed function did not retire its frame");
    }
}
std::string Machine::completion_hash() const {
    require(!tracking_only_,"live tracking is not an isolated machine snapshot");
    Bytes bytes;context_out(bytes,context);
    bytes.insert(bytes.end(),memory.begin(),memory.end());
    return platform::digest(bytes,true);
}
void Machine::save(const std::filesystem::path& path) const {
    require(!tracking_only_,"live tracking cannot save: scheduler/device contracts incomplete");
    require(bound==nullptr&&yielded_,"checkpoint requires fully unwound isolated execution");
    require(std::fegetround()==FE_TONEAREST,"unsupported host rounding mode");
    require(memory.size()==memory_bytes,"RDRAM allocation size changed");
    validate_frames(frames);
    Bytes body;
    u32(body,schema);u32(body,proof_only_kind);u32(body,no_save_medium_policy);
    u32(body,0); // Symbolic round-to-nearest; never encode platform FE_* values.
    text(body,identity.program);text(body,identity.executable);text(body,identity.input);
    text(body,identity.expected_completion);
    context_out(body,context);
    u32(body,static_cast<std::uint32_t>(frames.size()));
    for(const auto& f:frames) {
        u32(body,f.function);u32(body,f.pc);u64(body,f.hi);u64(body,f.lo);
        u64(body,f.result);u32(body,std::bit_cast<std::uint32_t>(f.c1cs));
        for(auto operand:f.operands)u64(body,operand);
        u32(body,static_cast<std::uint32_t>(f.extra_locals.size()));
        for(auto local:f.extra_locals)u64(body,local);
    }
    u32(body,static_cast<std::uint32_t>(memory.size()));
    body.insert(body.end(),memory.begin(),memory.end());
    Bytes output(magic.begin(),magic.end());u64(output,body.size());
    auto hash=platform::digest(body,true);
    output.insert(output.end(),hash.begin(),hash.end());
    output.insert(output.end(),body.begin(),body.end());
    require(output.size()<=max_file,"state exceeds maximum size");
    atomic_write(path,output);
}
void Machine::load(const std::filesystem::path& path,const Identity& expected) {
    require(!tracking_only_,"live tracking cannot restore: scheduler/device contracts incomplete");
    require(bound==nullptr,"cannot load while generated execution is active");
    auto data=read_file(path);Reader envelope{data};
    auto file_magic=envelope.take(magic.size());
    require(std::equal(file_magic.begin(),file_magic.end(),magic.begin()),"invalid magic");
    const auto length=envelope.u64();auto hash_bytes=envelope.take(64);
    require(length==envelope.b.size(),"payload length mismatch");
    std::string hash(reinterpret_cast<const char*>(hash_bytes.data()),hash_bytes.size());
    require(platform::digest(envelope.b,true)==hash,"checksum mismatch");
    Reader body{envelope.b};
    require(body.u32()==schema,"incompatible schema");
    require(body.u32()==proof_only_kind,"not an isolated continuation proof state");
    require(body.u32()==no_save_medium_policy,"unsupported save-medium policy");
    require(body.u32()==0&&std::fegetround()==FE_TONEAREST,"unsupported host rounding mode");
    Identity next_identity{body.text(),body.text(),body.text(),body.text()};
    require(next_identity.program==expected.program,"incompatible generated program");
    require(next_identity.executable==expected.executable,"incompatible executable");
    require(next_identity.input==expected.input,"incompatible ROM/input identity");
    require(next_identity.expected_completion.size()==64,"invalid completion digest");
    recomp_context next_context{};context_in(body,next_context);
    auto count=body.u32();require(count&&count<=max_frames,"continuation count outside bounds");
    std::vector<Frame> next_frames;next_frames.reserve(count);
    for(std::uint32_t i=0;i<count;++i) {
        Frame f;f.function=body.u32();f.pc=body.u32();f.hi=body.u64();
        f.lo=body.u64();f.result=body.u64();f.c1cs=std::bit_cast<std::int32_t>(body.u32());
        for(auto& operand:f.operands)operand=body.u64();
        auto local_count=body.u32();require(local_count<=124,"generated native local count outside bounds");
        f.extra_locals.resize(local_count);for(auto& local:f.extra_locals)local=body.u64();
        next_frames.push_back(f);
    }
    validate_frames(next_frames);
    require(body.u32()==memory_bytes,"incompatible RDRAM size");
    auto next_bytes=body.take(memory_bytes);
    require(body.b.empty(),"trailing state data");
    std::vector<std::uint8_t> next_memory(next_bytes.begin(),next_bytes.end());
    // Validation and all allocations finish before replacing any machine state.
    context=next_context;context.f_odd=&context.f0.u32h;
    memory.swap(next_memory);frames.swap(next_frames);
    identity=std::move(next_identity);yielded_=true;yield_pc_=0;dispatching_=false;
}
}

extern "C" std::uint32_t tooie_continuation_enter(std::uint32_t function,
    std::uint64_t* hi,std::uint64_t* lo,std::uint64_t* result,int* c1cs) {
    auto& f=tooie::continuation::Machine::current().enter(function);
    *hi=f.hi;*lo=f.lo;*result=f.result;*c1cs=f.c1cs;return f.pc;
}
extern "C" void tooie_continuation_store(std::uint32_t pc,
    std::uint64_t hi,std::uint64_t lo,std::uint64_t result,int c1cs) {
    auto& f=tooie::continuation::Machine::current().top();
    f.pc=pc;f.hi=hi;f.lo=lo;f.result=result;f.c1cs=c1cs;
}
extern "C" void tooie_continuation_leave(std::uint32_t function) {
    tooie::continuation::Machine::current().leave(function);
}
extern "C" int tooie_continuation_checkpoint(std::uint32_t pc) {
    return tooie::continuation::Machine::current().checkpoint(pc);
}
extern "C" int tooie_continuation_yielded() {
    return tooie::continuation::Machine::current().suspended();
}

extern "C" std::uint32_t tooie_persist_enter(std::uint32_t function,std::uint64_t* locals,std::uint32_t count) {
    auto* machine=tooie::continuation::Machine::current_if_bound();
    if(!machine)return 0;
    require(count>=4&&count<=128,"invalid generated local layout");
    auto& f=machine->enter(function);
    if(f.pc==0)f.extra_locals.resize(count-4);
    require(f.extra_locals.size()==count-4,"generated local layout differs from checkpoint");
    locals[0]=f.hi;locals[1]=f.lo;locals[2]=f.result;locals[3]=static_cast<std::uint64_t>(f.c1cs);
    std::copy(f.extra_locals.begin(),f.extra_locals.end(),locals+4);
    return f.pc;
}
extern "C" void tooie_persist_store(std::uint32_t pc,const std::uint64_t* locals,std::uint32_t count) {
    auto* machine=tooie::continuation::Machine::current_if_bound();
    if(!machine)return;
    require(count>=4&&count<=128,"invalid generated local layout");
    auto& f=machine->top();
    require(f.extra_locals.size()==count-4,"generated local layout changed during execution");
    f.pc=pc;f.hi=locals[0];f.lo=locals[1];f.result=locals[2];f.c1cs=static_cast<std::int32_t>(locals[3]);
    std::copy(locals+4,locals+count,f.extra_locals.begin());
}
extern "C" void tooie_persist_leave(std::uint32_t function) {
    if(auto* machine=tooie::continuation::Machine::current_if_bound())machine->leave(function);
}
extern "C" int tooie_persist_checkpoint(std::uint32_t pc) {
    auto* machine=tooie::continuation::Machine::current_if_bound();return machine&&machine->checkpoint(pc);
}
extern "C" int tooie_persist_yielded() {
    auto* machine=tooie::continuation::Machine::current_if_bound();return machine&&machine->suspended();
}
extern "C" void tooie_persist_native_guard(const char* symbol) {
    if(auto* machine=tooie::continuation::Machine::current_if_bound())machine->native_dependency(symbol);
}
extern "C" void tooie_persist_invalidate() {
    if(auto* machine=tooie::continuation::Machine::current_if_bound())machine->top().pc=0;
}
extern "C" recomp_func_t* tooie_persist_resolve(recomp_func_t* entry) {
    if(auto* machine=tooie::continuation::Machine::current_if_bound())return machine->validate_function_pointer(entry);
    return entry;
}
