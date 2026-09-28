#include "continuous_host.hpp"
#include "continuous_internal.hpp"
#include "game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/addresses.hpp"
#include "hardware_access.hpp"
#include "graphics_observation.hpp"
#include "title_observation.hpp"
#include "map_actor_list.hpp"
#include "menu_observation.hpp"
#include "sfx_wait_observation.hpp"
#include "sfx_wait_checkpoint.hpp"
#include <bit>
#include <chrono>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

extern "C" void yield_self_1ms(uint8_t* rdram);

namespace {
std::atomic<uint64_t> sfx_checkpoint_calls{},sfx_checkpoint_returns{};
tooie::sfx_wait::Counters sfx_wait_counters;
std::atomic<uint64_t> sfx_wait_log_failures{};
tooie::graphics_observation::Observer graphics_observer;
tooie::title_observation::Observer title_observer;
tooie::map_actor_list::Observer map_list_observer;
tooie::menu_observation::Observer menu_observer;
}
namespace tooie {
Json sfx_wait_observation_summary() {
    return {{"attempts",sfx_wait_counters.attempts.load()},
        {"snapshot_reservations",sfx_wait_counters.emitted.load()},
        {"capped",sfx_wait_counters.capped.load()},
        {"invalid",sfx_wait_counters.invalid.load()},
        {"outer_clock_checks",sfx_wait_counters.clock_checks.load()},
        {"rate_skipped",sfx_wait_counters.rate_skipped.load()},
        {"observer_failures",sfx_wait_log_failures.load()},{"limit",tooie::sfx_wait::cap},
        {"repeat_checkpoint",{{"enabled_in_continuous",true},
            {"runtime_helper","yield_self_1ms"},{"calls",sfx_checkpoint_calls.load()},
            {"normal_returns",sfx_checkpoint_returns.load()},
            {"unwind_difference_is_not_delivery_failure",true},
            {"host_handle_clears",0},{"fabricated_messages",0}}},
        {"guest_memory_modified",false},{"guest_registers_modified",false}};
}

Json menu_observation_summary() {
    Json categories=Json::object();
    for(unsigned i=0;i<menu_observation::sites.size();++i)
        categories[menu_observation::names[i]]={{"seen",menu_observer.seen[i].load()},
            {"log_attempts",menu_observer.attempts[i].load()},{"emitted",menu_observer.emitted[i].load()},
            {"unchanged_filtered",menu_observer.filtered[i].load()},{"cap_suppressed",menu_observer.capped[i].load()},
            {"invalid_snapshots",menu_observer.invalid[i].load()},{"limit",menu_observation::limits[i]}};
    return {{"categories",std::move(categories)},{"observer_failures",menu_observer.failures.load()},
        {"guest_memory_modified",false},{"guest_registers_modified",false},{"input_acceptance_claim",false}};
}
Json map_actor_list_summary() {
    return {{"seen",map_list_observer.seen.load()},{"log_attempts",map_list_observer.attempts.load()},
        {"emitted",map_list_observer.emitted.load()},{"suppressed",map_list_observer.suppressed.load()},
        {"incomplete",map_list_observer.incomplete.load()},{"observer_failures",map_list_observer.failures.load()},
        {"limit",map_actor_list::limit},{"capacity",map_actor_list::capacity},
        {"guest_memory_modified",false},{"guest_registers_modified",false}};
}
Json graphics_observation_summary() {
    Json categories=Json::object();
    for(size_t i=0;i<graphics_observation::categories.size();++i)
        categories[graphics_observation::categories[i]]={
            {"seen",graphics_observer.seen[i].load()},
            {"emitted",graphics_observer.emitted[i].load()},
            {"suppressed",graphics_observer.suppressed[i].load()},
            {"limit",graphics_observation::limits[i]}};
    return {{"categories",categories},{"observer_failures",graphics_observer.failures.load()},
        {"guest_memory_modified",false},{"guest_registers_modified",false},
        {"renderer_acceptance_claim",false},{"presented_frame_claim",false}};
}
Json title_timing_snapshot() {
    using namespace title_observation;
    const auto as_number=[](uint32_t bits)->Json {
        const auto value=std::bit_cast<float>(bits);
        return std::isfinite(value)?Json(value):Json(nullptr);
    };
    const auto first_title=title_observer.title_first_host_ns.load(std::memory_order_acquire);
    const auto latest_title=title_observer.title_latest_host_ns.load(std::memory_order_acquire);
    const auto first_attract=title_observer.attract_first_host_ns.load(std::memory_order_acquire);
    const auto latest_attract=title_observer.attract_latest_host_ns.load(std::memory_order_acquire);
    return {{"title",{{"seen",title_observer.seen[7].load()},{"valid",title_observer.title_timing_valid.load(std::memory_order_acquire)},
        {"actor_address",hex32(title_observer.title_actor.load())},{"elapsed_bits",hex32(title_observer.title_elapsed_bits.load())},
        {"elapsed",as_number(title_observer.title_elapsed_bits.load())},{"scheduler_game_delta_bits",hex32(title_observer.title_scheduler_delta_bits.load())},
        {"scheduler_tick_count_bits",hex32(title_observer.title_scheduler_ticks_bits.load())},{"first_host_ns",first_title},
        {"latest_host_ns",latest_title},{"host_elapsed_ns",latest_title>=first_title?latest_title-first_title:0}}},
        {"attract",{{"seen",title_observer.seen[8].load()},{"valid",title_observer.attract_timing_valid.load(std::memory_order_acquire)},
        {"phase",title_observer.attract_phase.load()},{"clock_bits",hex32(title_observer.attract_clock_bits.load())},
        {"clock",as_number(title_observer.attract_clock_bits.load())},{"scheduler_game_delta_bits",hex32(title_observer.attract_scheduler_delta_bits.load())},
        {"scheduler_tick_count_bits",hex32(title_observer.attract_scheduler_ticks_bits.load())},{"first_host_ns",first_attract},
        {"latest_host_ns",latest_attract},{"host_elapsed_ns",latest_attract>=first_attract?latest_attract-first_attract:0}}},
        {"per_frame_trace_emitted",false}};
}
Json title_observation_summary() {
    Json categories=Json::object();
    for(size_t i=0;i<title_observation::names.size();++i)
        categories[title_observation::names[i]]={
            {"seen",title_observer.seen[i].load()},{"emitted",title_observer.emitted[i].load()},
            {"suppressed",title_observer.suppressed[i].load()},{"log_attempts",title_observer.log_attempts[i].load()},
            {"limit",title_observation::limits[i]}};
    return {{"categories",categories},{"observer_failures",title_observer.failures.load()},
        {"guest_memory_modified",false},{"guest_registers_modified",false},
        {"input_acceptance_claim",false},{"gameplay_acceptance_claim",false}};
}
}
extern "C" void tooie_observe_map_actor_list(const uint8_t* rdram,const recomp_context* ctx,uint32_t site) {
    if(!tooie::continuous_enabled())return;
    map_list_observer.observe(rdram,0x800000,ctx,site,[&](const tooie::map_actor_list::Snapshot& s){
        tooie::Json rows=tooie::Json::array();
        for(unsigned i=0;i<s.count;++i){
            const auto& r=s.rows[i];std::string bytes;bytes.reserve(40);
            for(auto b:r.bytes){bytes.push_back("0123456789abcdef"[b>>4]);bytes.push_back("0123456789abcdef"[b&15]);}
            rows.push_back({{"index",i},{"record_address",tooie::hex32(r.address)},{"marker_id",r.marker},{"record20_hex",bytes}});
        }
        tooie::trace("map_actor_list_planned","G4","observation",site,ctx,
            {{"overlay_id",727},{"original_function","gspropsDll_entrypoint_1"},{"observation_phase","after sort before descriptor iteration"},
             {"category_sequence",s.observation},{"complete",s.complete},{"reason",std::string(s.error)},
             {"error_index",s.error_index==tooie::map_actor_list::capacity?tooie::Json(nullptr):tooie::Json(s.error_index)},
             {"current_map",s.map},{"array_address",tooie::hex32(s.array_address)},{"requested_count",s.requested_count},
             {"record_count",s.count},{"records",std::move(rows)},{"capacity",tooie::map_actor_list::capacity},
             {"guest_memory_modified",false},{"guest_registers_modified",false},{"constructor_completion_claim",false},
             {"plan_condition","earlier constructors return normally and future list/marker records remain unchanged"}});
    });
}
extern "C" void tooie_observe_menu(const uint8_t* rdram,const recomp_context* ctx,uint32_t site) {
    if(!tooie::continuous_enabled())return;
    menu_observer.observe(rdram,0x800000,ctx,site,[&](const tooie::menu_observation::Record& r){
        using namespace tooie::menu_observation;
        std::ostringstream worker;worker<<std::this_thread::get_id();
        tooie::Json extra={{"overlay_id",205},{"original_function",functions[r.category]},
            {"observer_host_thread",worker.str()},{"category_sequence",r.category_sequence},
            {"snapshot_valid",r.valid()},{"globals_valid",r.globals_valid},{"current_map",r.map},
            {"pending_warp",r.pending},{"stack_valid",r.stack_valid},
            {"actor_kind",r.actor.present?(r.actor.child?"child":"root"):"none"},
            {"guest_memory_modified",false},{"guest_registers_modified",false},{"input_acceptance_claim",false}};
        const auto number=[](uint32_t bits,bool valid)->tooie::Json {
            return valid?tooie::Json(std::bit_cast<float>(bits)):tooie::Json(nullptr);
        };
        if(r.actor.present){
            const auto& a=r.actor;
            extra["actor_valid"]=a.valid;extra["actor_address"]=tooie::hex32(a.address);
            extra["actor_marker"]=tooie::hex32(a.marker);extra["menu_state"]=a.state;extra["selection"]=a.selection;
            if(a.child){
                extra["parent_marker"]=tooie::hex32(a.parent_marker);extra["actor_flags"]=tooie::hex32(a.flags);
                extra["direction_disabled"]=bool(a.flags&(1u<<19));extra["slot_only"]=bool(a.flags&(1u<<20));
                extra["timer_bits"]=tooie::hex32(a.timer_bits);extra["timer_finite"]=a.timer_finite;
                extra["timer_value"]=number(a.timer_bits,a.timer_finite);
            }
        }
        if(r.slots_present){
            tooie::Json slots=tooie::Json::array();
            for(unsigned i=0;i<3;++i){const auto& s=r.slots[i];slots.push_back({{"slot",i},
                {"address",tooie::hex32(s.address)},{"valid",s.valid},{"occupancy_byte",s.value},
                {"nonempty",s.valid?tooie::Json(s.value!=0):tooie::Json(nullptr)}});}
            extra["slots_valid"]=r.slots_valid;extra["slots"]=std::move(slots);extra["occupancy_predicate_only"]=true;
        }
        if(r.stick_present){
            extra["stick_valid"]=r.stick_valid;extra["x_finite"]=r.x_finite;extra["y_finite"]=r.y_finite;
            extra["stick_x_bits"]=tooie::hex32(r.x_bits);extra["stick_y_bits"]=tooie::hex32(r.y_bits);
            extra["stick_x"]=number(r.x_bits,r.x_finite);extra["stick_y"]=number(r.y_bits,r.y_finite);
        }
        if((r.category>=3 && r.category<=5) || r.category==7)extra["getter_result"]=r.result;
        if(r.category==2){extra["old_selection"]=r.old_selection;extra["new_selection"]=r.actor.selection;}
        if(r.category==6 || r.category==9)extra["event_selection"]=r.event_selection;
        if(r.category==8){extra["message_index"]=r.message;extra["message_dispatched"]=r.stack_valid?tooie::Json(r.message!=-1):tooie::Json(nullptr);extra["visible_message_claim"]=false;}
        if(r.category==10){extra["camera_request_returned"]=true;extra["camera_motion_claim"]=false;extra["title_return_claim"]=false;}
        tooie::trace(names[r.category],"G6/G7","observed",site,ctx,std::move(extra));
    });
}
extern "C" void tooie_observe_title(const uint8_t* rdram,const recomp_context* ctx,uint32_t site) {
    if(!tooie::continuous_enabled())return;
    title_observer.observe(rdram,0x800000,ctx,site,[&](const tooie::title_observation::Record& r){
        using namespace tooie::title_observation;
        std::ostringstream worker;worker<<std::this_thread::get_id();
        tooie::Json extra={{"category_sequence",r.category_sequence},{"observer_host_thread",worker.str()},
            {"state_valid",r.state_valid},{"current_map",r.map},{"pending_warp",r.pending},
            {"start_counter",r.start},{"b_counter",r.b},{"input_ready",r.ready},{"input_blocked",r.blocked},
            {"guest_memory_modified",false},{"guest_registers_modified",false},{"input_acceptance_claim",false}};
        if(r.category<=4){
            extra["overlay_id"]=419;extra["original_function"]="func_808005AC_chintroticker";
            extra["actor_address"]=tooie::hex32(r.actor);extra["actor_valid"]=r.actor_valid;
            extra["ticker_mode"]=r.mode;extra["context_map"]=r.context_map;
            if(r.category>=1 && r.category<=3)extra["getter_result"]=r.result;
            if(r.category==3)extra["compare_rhs"]=r.compare_rhs;
            if(r.category==4)extra["transition_call"]="_gcfrontend_entrypoint_10";
        }else if(r.category==5){
            extra["overlay_id"]=673;extra["original_function"]="func_80800738_gcfrontend";
            extra["requested_map"]=r.requested_map;extra["requested_exit"]=r.requested_exit;
            extra["requested_transition"]=r.requested_transition;extra["request_only"]=true;
        }else if(r.category==6){
            extra["overlay_id"]=726;extra["original_function"]="gsworldDll_entrypoint_2";
            extra["map_state_address"]=tooie::hex32(r.map_state_address);extra["store_valid"]=r.store_valid;
            extra["global_map_target"]=r.global_map_target;extra["previous_map"]=r.previous_map;
            extra["stored_map"]=r.stored_map;extra["requested_map"]=r.requested_map;
            extra["observation_phase"]="after original current-map halfword store";
            extra["map_load_complete_claim"]=false;
        }
        tooie::trace(names[r.category],"G7","observed",site,ctx,std::move(extra));
    });
}
extern "C" void tooie_observe_graphics(const uint8_t* rdram,const recomp_context* ctx,uint32_t site) {
    if(!tooie::continuous_enabled())return;
    graphics_observer.observe(rdram,ctx,site,[&](const tooie::graphics_observation::Record& r){
        using namespace tooie::graphics_observation;
        tooie::Json state=tooie::Json::object();
        if(r.state_valid)for(size_t i=0;i<state_addresses.size();++i)
            state[tooie::hex32(state_addresses[i])]=tooie::hex32(r.state[i]);
        std::ostringstream worker;worker<<std::this_thread::get_id();
        tooie::Json extra={{"category",categories[r.category]},{"category_sequence",r.category_sequence},
            {"observer_host_thread",worker.str()},{"scheduler_state_valid",r.state_valid},{"scheduler_state",state},
            {"guest_memory_modified",false},{"guest_registers_modified",false},
            {"renderer_acceptance_claim",false},{"presented_frame_claim",false}};
        if(r.receive_return) {
            extra["queue"]=tooie::hex32(0x800783d0);extra["api_result"]=r.result;
            extra["guest_receive_observed"]=r.receive_success;
            extra["message_valid"]=r.message_valid;
            if(r.message_valid)extra["message"]=tooie::hex32(r.message);
        }else{
            extra["task_address"]=tooie::hex32(r.task_address);extra["task_descriptor_valid"]=r.task_valid;
            extra["submission_stage"]="before osSpTaskStartGo after original delay slot";
            extra["queue_acceptance_claim"]=false;
            if(r.task_valid){
                extra["task_words"]=r.task;extra["task_type"]=r.task[0];extra["task_flags"]=r.task[1];
                extra["ucode"]=tooie::hex32(r.task[4]);extra["ucode_size"]=r.task[5];
                extra["ucode_data"]=tooie::hex32(r.task[6]);extra["ucode_data_size"]=r.task[7];
                extra["display_list"]=tooie::hex32(r.task[12]);extra["display_list_size"]=r.task[13];
            }
        }
        tooie::trace(r.receive_return?"scheduler_message_receive_returned":"original_task_submit_attempt",
            "G3/G5","observed",site,ctx,std::move(extra));
    });
}

extern "C" void tooie_observe_global_settings(const recomp_context* ctx, uint32_t site) {
    if (!tooie::continuous_enabled()) return;
    const char* event = nullptr;
    const char* outcome = "observed";
    const char* function = "glglobalsettings_entrypoint_1";
    tooie::Json extra = {
        {"overlay_id", 699}, {"overlay_name", "glglobalsettings"},
        {"original_pc", tooie::hex32(site)},
        {"pc_kind", "original overlay virtual PC; not live relocated PC"},
        {"guest_memory_modified", false}, {"guest_registers_modified", false},
        {"populated_game_slot_claim", false}
    };
    switch (site) {
    case 0x808001B0:
        event = "global_settings_record_read";
        function = "func_80800168_glglobalsettings";
        extra["global_record_index"] = int32_t(ctx->r16);
        extra["original_reader_status"] = int32_t(ctx->r2);
        extra["original_reader_accepted"] = int32_t(ctx->r2) == 0;
        break;
    case 0x80800234:
        event = "global_settings_selection";
        extra["selected_global_record"] = int32_t(ctx->r2);
        break;
    case 0x80800240:
        event = "global_settings_defaults";
        outcome = "entered";
        break;
    case 0x80800250:
        event = "global_settings_import";
        outcome = "entered";
        extra["original_data_pointer"] = tooie::hex32(ctx->r4);
        break;
    case 0x80800258:
        event = "global_settings_import_return";
        outcome = "returned"; // Original void deserializer: V0 is not status.
        break;
    default:
        throw std::runtime_error("Unexpected source-scoped global settings observation site");
    }
    std::ostringstream worker;
    worker << std::this_thread::get_id();
    extra["observer_host_thread"] = worker.str();
    extra["original_function"] = function;
    // Executed on the original worker, reading only its live const context.
    // No guest RAM reads, retained pointers, shared counters or acceptance state.
    // trace() serializes its own output; failures remain visible to the worker.
    tooie::trace(event, "G6/G7", outcome, site, ctx, std::move(extra));
}

extern "C" void tooie_continuous_poll(uint8_t* rdram,recomp_context* ctx,uint32_t pc) {
    if (!tooie::continuous_enabled()) return;
    tooie::continuous_poll();
if(pc==tooie::sfx_wait::entry_pc || pc==tooie::sfx_wait::outer_pc || pc==tooie::sfx_wait::after_pc) {
    thread_local tooie::sfx_wait::Worker sfx_worker;
    try {
        auto snapshot=tooie::sfx_wait::observe(true,pc,rdram,0x800000,*ctx,sfx_worker,sfx_wait_counters,[]{
            return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        });
        if(snapshot) {
            const auto& s=*snapshot;
            tooie::Json slots=tooie::Json::array();
            if(s.complete) for(const auto& slot:s.slots) slots.push_back({
                {"index",slot.index},{"address",tooie::hex32(slot.address)},
                {"flags",tooie::hex32(slot.flags)},{"first_byte",slot.first_byte},
                {"state",slot.state},{"handle",slot.handle},
                {"target_address",tooie::hex32(slot.target_address)},
                {"target_word",tooie::hex32(slot.target_word)},{"unresolved",slot.unresolved}});
            std::ostringstream host_thread;host_thread<<std::this_thread::get_id();
            tooie::trace("sfx_wait_snapshot","G7",s.complete?"observed":"incomplete",pc,ctx,
                {{"phase",pc==tooie::sfx_wait::entry_pc?"entry":pc==tooie::sfx_wait::outer_pc?"outer":"after_return"},
                 {"observer_host_thread",host_thread.str()},{"complete",s.complete},
                 {"outer_pass",s.outer_pass},{"unresolved",s.unresolved},{"slots",std::move(slots)},
                 {"s1",tooie::hex32(s.s1)},{"s2",s.s2},{"s3",s.s3},
                 {"guest_memory_modified",false},{"guest_registers_modified",false},
                 {"scheduler_action_performed",false},{"causal_starvation_claim",false}});
        }
    } catch (...) { sfx_wait_log_failures.fetch_add(1,std::memory_order_relaxed); }
}
    // Only the source-verified unresolved SFX repeat edge needs this native
    // scheduling checkpoint. Keep it outside the observer catch: stop unwinds.
    // The runtime delivers a real queued message and checks higher priorities;
    // original guest code alone decides completion and clears owned handles.
    thread_local tooie::sfx_checkpoint::State sfx_checkpoint;
    if (tooie::sfx_checkpoint::should_service(true,pc,*ctx,sfx_checkpoint)) {
        sfx_checkpoint_calls.fetch_add(1,std::memory_order_relaxed);
        yield_self_1ms(rdram);
        sfx_checkpoint_returns.fetch_add(1,std::memory_order_relaxed);
    }
    // Per-worker first visits record the causal startup chain without tracing
    // every instruction or reading memory concurrently from the controller.
    thread_local std::set<uint32_t> observed;
    if(pc==0x8001DFB0 || pc==0x8001DFC8) {
        tooie::trace("cic_worker_si_receive","G2","observed",pc,ctx,
            {{"phase",pc==0x8001DFB0?"write":"read"},
             {"queue",tooie::hex32(ctx->r16)},{"result",int32_t(ctx->r2)},
             {"guest_receive_observed",int32_t(ctx->r2)==0},{"guest_memory_modified",false}});
    }
    if(pc==0x8001E1B0) {
        const auto destination=tooie::canonical_rdram(uint32_t(ctx->r4),16,1);
        static constexpr char digits[]="0123456789ABCDEF";
        std::string source_bytes,destination_bytes;
        for(unsigned i=0;i<16;++i) {
            const uint8_t source=MEM_BU(i,(gpr)(int32_t)0x8007D880);
            const uint8_t copied=MEM_BU(i,(gpr)(int32_t)destination);
            source_bytes.push_back(digits[source>>4]);source_bytes.push_back(digits[source&15]);
            destination_bytes.push_back(digits[copied>>4]);destination_bytes.push_back(digits[copied&15]);
        }
        tooie::trace("cic_original_writeback","G2","observed",pc,ctx,
            {{"source",tooie::hex32(0x8007D880)},{"destination",tooie::hex32(destination)},
             {"source_hex",source_bytes},{"destination_hex",destination_bytes},
             {"bytes",16},{"copy_matches",source_bytes==destination_bytes},{"guest_memory_modified",false}});
    }
    // Original worker has completed both uncached loads and its comparison at
    // this entry. Observe its own live stack; never supply expected IPL3 words.
    if (pc==0x8001DEE8 && observed.insert(0x8001DED4).second) {
        tooie::trace("integrity_boot_residue","G2","observed",pc,ctx,
            {{"source_802fb1f4",tooie::hex32(MEM_W(0,(gpr)(int32_t)0x802FB1F4))},
             {"source_802fe1c0",tooie::hex32(MEM_W(0,(gpr)(int32_t)0x802FE1C0))},
             {"stack_8007db54",tooie::hex32(MEM_W(0,(gpr)(int32_t)0x8007DB54))},
             {"stack_8007db50",tooie::hex32(MEM_W(0,(gpr)(int32_t)0x8007DB50))},
             {"comparison_byte_8007db79",uint32_t(MEM_BU(0,(gpr)(int32_t)0x8007DB79))},
             {"guest_memory_modified",false}});
    }
    // First flag store called by gldbDll entry2: preserve the original mask and
    // per-bit operation, recording only register values at the callee boundary.
    if (pc==0x800DA3B8 && uint32_t(ctx->r4)==0x648 && observed.insert(pc).second) {
        tooie::trace("integrity_flag648_context","G2","observed",pc,ctx,
            {{"s1_mask",tooie::hex32(ctx->r17)},{"s2",tooie::hex32(ctx->r18)},
             {"v0",tooie::hex32(ctx->r2)},{"a1_bit",uint32_t(ctx->r5)},
             {"guest_memory_modified",false}});
    }
    switch (pc) {
        case 0x80012030: case 0x80013678: case 0x800124EC: case 0x80012214:
        case 0x8001DDF0: case 0x8001DE64: case 0x8001DEE8: case 0x8001E840:
        case 0x80014FE8: case 0x8001E7E8: case 0x8001C1C0: case 0x80019EC0:
        case 0x8001A8B4: case 0x801168F0: case 0x800815CC:
            if (observed.insert(pc).second) {
                tooie::capture_guest_state(rdram,ctx);
                tooie::trace(pc==0x800124EC?"continuous_main_entered":"continuous_function_entered",
                    "mission-01","entered",pc,ctx,{{"guest_thread_id",ultramodern::this_thread()?tooie::Json(osGetThreadId(rdram,0)):tooie::Json(nullptr)},
                    {"original_generated_body",true}});
            }
            break;
    }
}
