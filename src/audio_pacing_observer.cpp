#include "audio_pacing_observer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <locale>
#include <memory>
#include <stdexcept>
#include <vector>

namespace tooie::audio_pacing {
namespace {
constexpr uint32_t invalid = 0x80000000u;
struct Session {
    std::filesystem::path directory;
    std::vector<Record> records;
    std::atomic<uint64_t> attempted{0}, invalid_guest{0};
    bool finished=false;
    Statistics final{};
};
std::unique_ptr<Session> session;
std::atomic<bool> active{false};
std::atomic<uint64_t> writers{0};
bool range(const uint8_t* ram,size_t bytes,uint64_t address,size_t width,size_t alignment,size_t& offset) noexcept {
    if(!ram || address>UINT32_MAX)return false;
    const auto a=uint32_t(address), segment=a&0xe0000000u;
    offset=a&0x1fffffffu;
    return (segment==0x80000000u || segment==0xa0000000u) && !(a&(alignment-1)) &&
        offset<=std::min<size_t>(bytes,0x800000) && width<=std::min<size_t>(bytes,0x800000)-offset;
}
bool word(const uint8_t* ram,size_t bytes,uint64_t address,uint64_t& value) noexcept {
    size_t offset;uint32_t v;
    if(!range(ram,bytes,address,4,4,offset))return false;
    std::memcpy(&v,ram+offset,4);value=v;return true;
}
bool half(const uint8_t* ram,size_t bytes,uint64_t address,uint64_t& value) noexcept {
    size_t offset;uint16_t v;
    if(!range(ram,bytes,address,2,2,offset))return false;
    // Halfwords in word-swapped native RDRAM have address xor2. Verify the
    // actual host span too, including deliberately tiny test memory views.
    offset^=2;
    if(offset>bytes || bytes-offset<2)return false;
    std::memcpy(&v,ram+offset,2);value=v;return true;
}
}
bool enabled() noexcept{return active.load(std::memory_order_acquire);}
uint64_t now_ns() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void initialize(const std::filesystem::path& directory,size_t capacity) {
    if(enabled() || writers.load()!=0)throw std::runtime_error("Audio pacing producers must be stopped before initialize");
    if(directory.empty()){session.reset();return;}
    if(capacity==0 || capacity>262144)throw std::runtime_error("Audio pacing capacity must be within1..262144");
    const auto path=std::filesystem::absolute(directory);
    if(std::filesystem::exists(path) && !std::filesystem::is_empty(path))throw std::runtime_error("Audio pacing directory must be new or empty");
    auto next=std::make_unique<Session>();next->directory=path;next->records.resize(capacity);
    std::filesystem::create_directories(path);session=std::move(next);
    active.store(true,std::memory_order_release);
}
void append(const Record& record) noexcept {
    if(!enabled())return;
    writers.fetch_add(1,std::memory_order_acq_rel);
    if(enabled()) {
        // Every writer has its own slot. No allocations, mutex, I/O or reused
        // slots in this bounded append path; readers wait until all joins.
        auto& s=*session;
        const uint64_t index=s.attempted.fetch_add(1,std::memory_order_relaxed);
        if(record.kind==Kind::guest && (record.flags&invalid))s.invalid_guest.fetch_add(1,std::memory_order_relaxed);
        if(index<s.records.size())s.records[size_t(index)]=record;
    }
    writers.fetch_sub(1,std::memory_order_release);
}
void observe_guest(const uint8_t* ram,size_t bytes,const recomp_context* ctx,uint32_t site) noexcept {
    if(!enabled())return;
    Record r;r.kind=Kind::guest;r.times[0]=now_ns();r.values[0]=site;
    bool valid=ctx && (site==0x80012934 || site==0x80012998 || site==0x80012a2c || site==0x80012af0);
    if(ctx) {
        r.flags|=1;r.values[2]=uint32_t(ctx->r2);r.values[3]=uint32_t(ctx->r16);
        r.values[4]=uint32_t(ctx->r19);r.values[5]=uint32_t(ctx->r29);r.values[10]=uint32_t(ctx->r3);
        if(word(ram,bytes,0x80076938,r.values[6]))r.flags|=2;else valid=false;
        if(word(ram,bytes,0x8006a148,r.values[9]))r.flags|=16;else valid=false;
        if(site==0x80012af0) {
            if(half(ram,bytes,uint64_t(uint32_t(ctx->r16))+4,r.values[7]))r.flags|=4;else valid=false;
            if(word(ram,bytes,uint32_t(ctx->r16),r.values[8]))r.flags|=8;else valid=false;
        }
    }
    r.values[11]=site==0x80012934?0x80045bb8:site==0x80012998?0x80045bf0:0;
    if(!valid)r.flags|=invalid;
    r.values[1]=r.flags;r.times[1]=now_ns();append(r);
}
Statistics finish() {
    active.store(false,std::memory_order_release);
    if(writers.load(std::memory_order_acquire)!=0)throw std::runtime_error("Audio pacing finish requires joined producers");
    if(!session)return {};
    auto& s=*session;if(s.finished)return s.final;
    Statistics stats;stats.capacity=s.records.size();stats.attempted=s.attempted.load();
    stats.emitted=std::min(stats.capacity,stats.attempted);stats.overflow=stats.attempted-stats.emitted;
    stats.invalid_guest=s.invalid_guest.load();
    const auto csv=s.directory/"records.csv", summary=s.directory/"summary.json";
    if(std::filesystem::exists(csv) || std::filesystem::exists(summary))throw std::runtime_error("Refusing to overwrite audio pacing output");
    std::ofstream out(csv);out.imbue(std::locale::classic());out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"ordinal,kind,flags";for(unsigned i=0;i<8;++i)out<<",t"<<i;for(unsigned i=0;i<12;++i)out<<",v"<<i;out<<'\n';
    for(size_t i=0;i<stats.emitted;++i) {
        const auto&r=s.records[i];out<<i+1<<','<<uint32_t(r.kind)<<','<<r.flags;
        for(auto t:r.times)out<<','<<t;for(auto v:r.values)out<<','<<v;out<<'\n';
    }
    out.close();
    std::ofstream meta(summary);meta.imbue(std::locale::classic());meta.exceptions(std::ios::badbit|std::ios::failbit);
    meta<<"{\n  \"schema\": 1,\n  \"clock\": \"std::chrono::steady_clock nanoseconds; same process clock as trace\",\n"
        <<"  \"capacity\": "<<stats.capacity<<",\n  \"attempted\": "<<stats.attempted<<",\n  \"emitted\": "<<stats.emitted
        <<",\n  \"overflow\": "<<stats.overflow<<",\n  \"invalid_guest\": "<<stats.invalid_guest
        <<",\n  \"record_bytes\": "<<sizeof(Record)<<",\n  \"producer_io\": false,\n  \"sample_or_pacing_changes\": false,\n"
        <<"  \"finalized_after_caller_join_contract\": true,\n  \"device_underrun_claim\": false\n}\n";
    meta.close();s.final=stats;s.finished=true;return stats;
}
}
extern "C" void tooie_observe_audio_pacing(const uint8_t* ram,const recomp_context* ctx,uint32_t site) noexcept {
    tooie::audio_pacing::observe_guest(ram,0x800000,ctx,site);
}
