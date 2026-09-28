// Synthetic observation contract test, not evidence of a live guest challenge.
#include "si_adapter.hpp"
#include "ultramodern/ultramodern.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

static std::vector<tooie::si::Observation> observations;
static unsigned completion_calls=0;
static bool fail_completion=false, fail_observer=false;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
static void observe(const tooie::si::Observation& observation){
    if(fail_observer)throw std::runtime_error("synthetic observer failure");
    observations.push_back(observation);
}
namespace ultramodern {
void send_si_message(){++completion_calls;if(fail_completion)throw std::runtime_error("synthetic completion failure");}
}
extern "C" void osCreateMesgQueue(uint8_t*,PTR(OSMesgQueue),PTR(OSMesg),s32){throw std::runtime_error("unexpected queue use");}
extern "C" s32 osSendMesg(uint8_t*,PTR(OSMesgQueue),OSMesg,s32){throw std::runtime_error("unexpected queue use");}
extern "C" s32 osRecvMesg(uint8_t*,PTR(OSMesgQueue),PTR(OSMesg),s32){throw std::runtime_error("unexpected queue use");}
int main(){try{
    using namespace tooie::si;
    std::vector<uint8_t> ram(512,0xa5);
    constexpr uint32_t address=0xA0000100u;
    std::array<uint8_t,64> payload{};payload.fill(0xff);payload[46]=payload[47]=15;payload[63]=2;
    Challenge challenge{};for(unsigned i=0;i<15;++i)payload[48+i]=challenge[i]=uint8_t(i*13+7);
    for(unsigned i=0;i<64;++i)ram[(0x100+i)^3]=payload[i];
    const auto source=ram;
    recomp_context ctx{};ctx.r4=1;ctx.r5=address;ctx.r31=0x8001e100;ctx.r29=0x8007ff00;ctx.r2=99;
    reset(ram.size());configure_observer(observe);
    tooie_si_raw_start_dma(ram.data(),&ctx);
    require(ram==source && ctx.r2==0,"write semantics changed");
    require(completion_calls==1 && observations.size()==3,"write completion/events count");
    for(unsigned i=0;i<3;++i){
        const auto& o=observations[i];
        require(o.transfer_id==1 && o.challenge_id==1 && o.direction==1 && o.guest_address==address && o.physical_address==0x100,"write identity mismatch");
        require(o.challenge==challenge && o.payload==payload && o.ra==0x8001e100 && o.sp==0x8007ff00,"original challenge/payload/context lost");
    }
    require(observations[0].phase==Phase::DmaTransferred && observations[1].phase==Phase::CompletionCall && observations[2].phase==Phase::CompletionReturned,"phase ordering");
    ctx.r4=0;std::fill(ram.begin()+0x100,ram.begin()+0x140,0xcc);
    tooie_si_raw_start_dma(ram.data(),&ctx);
    const auto expected=cic6105_response(challenge);
    for(unsigned i=0;i<15;++i)payload[48+i]=expected[i];payload[46]=payload[47]=payload[63]=0;
    for(unsigned i=0;i<64;++i)require(ram[(0x100+i)^3]==payload[i],"read response mismatch");
    require(observations.size()==6 && completion_calls==2,"read event count");
    require(observations[3].transfer_id==2 && observations[3].challenge_id==1 && observations[3].direction==0 && observations[3].payload==payload && observations[3].challenge==challenge,"read lost original challenge or actual payload");
    tooie_si_raw_start_dma(ram.data(),&ctx);
    require(observations[6].challenge_id==1 && observations[6].payload==payload,"repeat read observation changed device state");
    // Rejection must produce neither committed-transfer events nor completion.
    auto count=observations.size();auto calls=completion_calls;ctx.r4=7;ctx.r2=99;
    bool rejected=false;try{tooie_si_raw_start_dma(ram.data(),&ctx);}catch(const std::runtime_error&){rejected=true;}
    require(rejected && observations.size()==count && completion_calls==calls && ctx.r2==99,"invalid transfer fabricated completion");
    ctx.r4=0;fail_completion=true;rejected=false;
    try{tooie_si_raw_start_dma(ram.data(),&ctx);}catch(const std::runtime_error&){rejected=true;}
    require(rejected && observations.back().phase==Phase::CompletionThrew && observations.size()==count+3,"failed completion reported returned");
    fail_completion=false;fail_observer=true;calls=completion_calls;
    tooie_si_raw_start_dma(ram.data(),&ctx);
    require(completion_calls==calls+1 && observer_failures()==3,"observer failure changed completion or was not counted");
    fail_observer=false;configure_observer(nullptr);count=observations.size();calls=completion_calls;
    tooie_si_raw_start_dma(ram.data(),&ctx);
    require(observations.size()==count && completion_calls==calls+1,"disabled observer changed completion");
    reset(ram.size());require(observer_failures()==0,"reset did not clear observation failure count");
    std::puts("PASS synthetic SI observations: exact original challenge/write/read payloads, correlated phases, repeated read, rejected transfer, completion failure, observer failure, default-off behavior");
    return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
