// Synthetic records/guest fixtures only; no SDL/device/game use.
#include "audio_pacing_observer.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace tooie::audio_pacing;
static std::atomic<bool> count_allocations{false};
static std::atomic<unsigned> allocation_count{0};
void* operator new(std::size_t n){if(count_allocations.load())++allocation_count;if(void*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void*p) noexcept{std::free(p);}
void operator delete(void*p,std::size_t) noexcept{std::free(p);}
static void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
static std::vector<std::vector<uint64_t>> read(const std::filesystem::path& p){
    std::ifstream f(p);check(bool(f),"missing deferred record file");std::string s;std::getline(f,s);
    std::vector<std::vector<uint64_t>> rows;
    while(std::getline(f,s)){std::vector<uint64_t> row;std::istringstream in(s);std::string field;while(std::getline(in,field,','))row.push_back(std::stoull(field));rows.push_back(row);}return rows;
}
int main(int argc,char**argv){try{
    const auto root=(argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path()/"tooie-audio-pacing-test")/std::to_string(now_ns());
    initialize({});Record a;a.kind=Kind::query;a.times[0]=123;a.values[0]=789;append(a);
    auto s=finish();check(!enabled() && !s.attempted && !s.emitted,"disabled observer produced records");
    initialize(root/"bounded",3);for(int i=0;i<5;++i)append(a);
    check(!std::filesystem::exists(root/"bounded/records.csv"),"producer performed file I/O");
    s=finish();check(s.capacity==3 && s.attempted==5 && s.emitted==3 && s.overflow==2,"capacity/overflow accounting");
    auto rows=read(root/"bounded/records.csv");check(rows.size()==3 && rows[0].size()==23,"CSV schema/length");
    check(rows[0][0]==1 && rows[0][1]==1 && rows[0][3]==123 && rows[0][11]==789,"numeric record changed");
    check(finish().emitted==3,"repeat finish changed finalized receipt");
    append(a);check(finish().attempted==5,"postfinish writer accepted");
    bool refused=false;try{initialize(root/"bounded",3);}catch(const std::exception&){refused=true;}check(refused,"overwrote existing output");
    initialize(root/"parallel",4000);std::vector<std::thread> writers;
    for(unsigned j=0;j<4;++j)writers.emplace_back([j]{for(unsigned i=0;i<1000;++i){Record r;r.kind=Kind::queue;r.values[0]=j*1000+i;append(r);}});
    for(auto& t:writers)t.join();s=finish();check(s.attempted==4000 && s.emitted==4000 && !s.overflow,"parallel record loss");
    rows=read(root/"parallel/records.csv");std::set<uint64_t> ids;
    for(size_t i=0;i<rows.size();++i){check(rows[i][0]==i+1,"ordinal lost");ids.insert(rows[i][11]);}check(ids.size()==4000,"parallel slot collision");
    std::vector<uint8_t> ram(0x800000,0);recomp_context ctx{};ctx.r16=0x80001000;ctx.r2=1440;ctx.r3=360;ctx.r19=0x80045bb8;ctx.r29=0x80002000;
    auto put=[&](uint32_t address,uint32_t value){std::memcpy(ram.data()+(address&0x1fffffff),&value,4);};
    put(0x80076938,2);put(0x8006a148,12);put(0x80001000,0x80003000);
    uint16_t samples=552;std::memcpy(ram.data()+0x1006,&samples,2);
    const auto original=ram;const auto saved=ctx;
    initialize(root/"guest",16);
    count_allocations.store(true);
    for(auto site:{0x80012934u,0x80012998u,0x80012a2cu,0x80012af0u})observe_guest(ram.data(),ram.size(),&ctx,site);
    count_allocations.store(false);check(allocation_count.load()==0,"producer allocated heap memory");
    check(ram==original && std::memcmp(&ctx,&saved,sizeof(ctx))==0,"guest writes");
    ctx.r16=0x807ffffe;observe_guest(ram.data(),ram.size(),&ctx,0x80012af0);
    ctx.r16=0x1000;observe_guest(ram.data(),ram.size(),&ctx,0x80012af0);
    observe_guest(ram.data(),4,&ctx,0x80012af0);observe_guest(nullptr,0,nullptr,0x80012af0);observe_guest(ram.data(),ram.size(),&ctx,0xdeadbeef);
    s=finish();check(s.emitted==9 && s.invalid_guest==5,"invalid guest snapshot accounting");
    rows=read(root/"guest/records.csv");check(rows[2][13]==1440 && rows[3][21]==360 && rows[3][18]==552 && rows[3][19]==0x80003000,"query/decision extraction");
    check(rows[3][17]==2 && rows[3][20]==12,"cooldown/frame counter extraction");
    initialize(root/"output-error",1);append(a);
    {std::ofstream existing(root/"output-error/records.csv");existing<<"preserve";}
    refused=false;try{finish();}catch(const std::exception&){refused=true;}
    check(refused && !enabled(),"output error not visible or admission still active");
    std::ifstream kept(root/"output-error/records.csv");std::string contents;kept>>contents;check(contents=="preserve","output error overwrote file");
    refused=false;try{initialize(root/"bad-capacity",0);}catch(const std::exception&){refused=true;}check(refused,"zero capacity allowed");
    std::cout<<"PASS synthetic disabled/deferred/capacity/overflow/concurrent/const/bounds/finalization contracts\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
