#pragma once
#include "recomp.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Original gccubeDll stack array has 500 slots. A snapshot is a planned
// iteration, not proof every constructor completed or later records stayed fixed.
namespace tooie::map_actor_list {
inline constexpr uint32_t site=0x808001e0;
inline constexpr unsigned capacity=500,limit=8;
inline bool range(const uint8_t* ram,size_t size,uint32_t a,size_t n,unsigned alignment) noexcept {
 const auto segment=a&0xe0000000u;
 return ram && size==0x800000 && (segment==0x80000000u || segment==0xa0000000u) &&
        !(a&(alignment-1)) && uint64_t(a&0x1fffffffu)+n<=size;
}
inline uint32_t read(const uint8_t* ram,uint32_t a,unsigned n) noexcept {
 uint32_t out=0;for(unsigned i=0;i<n;++i)out=(out<<8)|ram[((a&0x1fffffffu)+i)^3];return out;
}
struct Row {uint32_t address=0;uint16_t marker=0;std::array<uint8_t,20> bytes{};};
struct Snapshot {
 uint64_t observation=0;uint32_t array_address=0,map=0;int32_t requested_count=0;
 unsigned count=0,error_index=capacity;bool complete=false;
 std::string_view error;std::array<Row,capacity> rows{};
};
struct Observer {
 std::atomic<uint64_t> seen{0},attempts{0},emitted{0},suppressed{0},failures{0},incomplete{0};
 template<class Sink> void observe(const uint8_t* ram,size_t size,const recomp_context* ctx,uint32_t pc,Sink&& sink) noexcept {
  const auto sequence=++seen;
  if(!ctx || pc!=site){++failures;return;}
  auto claimed=attempts.load();
  do {if(claimed>=limit){++suppressed;return;}}
  while(!attempts.compare_exchange_weak(claimed,claimed+1));
  try {
   Snapshot s;s.observation=sequence;s.array_address=uint32_t(ctx->r17);s.requested_count=int32_t(ctx->r20);
   if(!range(ram,size,0x80132dc2,2,2))s.error="invalid_memory";
   else {
    s.map=read(ram,0x80132dc2,2);
    if(uint32_t(ctx->r19)!=0)s.error="wrong_iteration_state";
    else if(s.requested_count<0 || s.requested_count>int(capacity))s.error="count_out_of_bounds";
    else if(s.requested_count && !range(ram,size,s.array_address,size_t(s.requested_count)*4,4))s.error="invalid_array";
    else {
     for(unsigned i=0;i<unsigned(s.requested_count);++i){
      auto& row=s.rows[i];row.address=read(ram,s.array_address+i*4,4);
      if(!range(ram,size,row.address,20,4)){s.error="invalid_record";s.error_index=i;break;}
      for(unsigned j=0;j<20;++j)row.bytes[j]=uint8_t(read(ram,row.address+j,1));
      row.marker=(uint16_t(row.bytes[8])<<8)|row.bytes[9];
     }
     if(s.error.empty()){s.complete=true;s.count=unsigned(s.requested_count);}
    }
   }
   if(!s.complete){s.rows={};++incomplete;}
   sink(s);++emitted;
  }catch(...){++failures;} // Observation cannot throw into guest execution.
 }
};
} // namespace tooie::map_actor_list
