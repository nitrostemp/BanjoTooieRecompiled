#include "continuous_host.hpp"
#include "game_features.hpp"
#include "pacing_render_work.hpp"
#include "scene_observer.hpp"
#include "virtual_clock.hpp"
#include "vi_timing.hpp"
#include "widescreen.hpp"
#include "hud_layout.hpp"
#include "visibility.hpp"
#include "draw_distance.hpp"
#include "overlay_call_trace.hpp"
#include "build_metadata.hpp"
#include "runtime_lifecycle.hpp"
#include "runtime_dp.hpp"
#include "platform_support.hpp"
#include "overlay_validation.hpp"
#include "si_adapter.hpp"
#include "ipl3_boot_state.hpp"
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
#include "persistent_state_runtime.hpp"
#include "persistent_state_devices.hpp"
#include "persistent_state_controller.hpp"
#endif
#ifdef TOOIE_NATIVE_HOST
#include "native_host_devices.hpp"
#include "tooie_audio_rsp.hpp"
#endif
#include "game.hpp"
#include "core1_metadata.hpp"
#include "librecomp/addresses.hpp"
#include "ultramodern/ultramodern.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>

// Exact pinned runtime startup implementation, used after callback/preinit.
// App owns the lifetime around it because recomp::start does not join guest
// workers or its detached timer before releasing RDRAM at this revision.
bool wait_for_game_started(uint8_t*,recomp_context*);
extern std::atomic_bool exited;
extern moodycamel::LightweightSemaphore graphics_shutdown_ready;
namespace tooie { void prepare_core1_continuous(uint8_t*); }

namespace {
std::atomic_bool boot_launcher_returned{false};
std::atomic_bool frontend_player_mode{false};
std::atomic_bool overlay_references_ready{false};
#ifdef TOOIE_NATIVE_HOST
struct NativeDevicesLifetime {
    bool active=false;
    ~NativeDevicesLifetime() {
        if(active) try { tooie::native_host::shutdown(); }
        catch(const std::exception& e){std::cerr<<"Native device cleanup retained resources: "<<e.what()<<'\n';}
    }
};
#endif
class HeadlessDiagnosticRenderer final: public ultramodern::renderer::RendererContext {
public:
    HeadlessDiagnosticRenderer() {
        setup_result=ultramodern::renderer::SetupResult::Success;
        chosen_api=ultramodern::renderer::GraphicsApi::Auto;
    }
    bool valid() override { return true; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&,const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    void send_dl(const OSTask*) override { throw std::runtime_error("Graphics task escaped headless submission rejection"); }
    void send_dummy_workload(uint32_t) override {}
    void update_screen() override {} // No presentation or successful-frame counter.
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1; }
};
std::unique_ptr<ultramodern::renderer::RendererContext> create_renderer(uint8_t*,ultramodern::renderer::WindowHandle,bool) {
    return std::make_unique<HeadlessDiagnosticRenderer>();
}
void no_rsp_init() {}
bool reject_rsp(uint8_t*,const OSTask*) { throw std::runtime_error("RSP task escaped headless submission rejection"); }
void reject_samples(int16_t*,size_t) { throw std::runtime_error("Audio samples require an actual audio host"); }
size_t no_samples() { return 0; }
void set_frequency(uint32_t) {}
void poll_input() {}
bool disconnected_input(int,uint16_t*,float*,float*) { return false; }
ultramodern::input::connected_device_info_t no_controller(int) {
    return {ultramodern::input::Device::None,ultramodern::input::Pak::None};
}
struct GuestMemory {
    uint8_t* data=nullptr;
    bool release_allowed=true;
    GuestMemory() {
        data=tooie::platform::reserve_rdram(recomp::allocation_size,recomp::mem_size);
    }
    ~GuestMemory() {
        if (!data) return;
        if (!release_allowed) {
            std::cerr<<"Continuous RDRAM retained: shutdown did not establish all resource joins\n";
            return;
        }
        tooie::platform::release_rdram(data,recomp::allocation_size);
    }
    void release() {
        if (!release_allowed) throw std::runtime_error("Unsafe continuous RDRAM release attempt");
        tooie::platform::release_rdram(data,recomp::allocation_size);
        data=nullptr;
    }
};
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
}

namespace tooie {
bool continuous_enabled() noexcept { return lifecycle::enabled(); }
bool continuous_frontend_mode() noexcept { return frontend_player_mode.load(std::memory_order_acquire); }
void continuous_poll() { lifecycle::poll(); }
void continuous_thread_create_callback(uint8_t* rdram,recomp_context* context) {
    continuous_poll();
    // The runtime starts the first worker without unwinding the boot launcher.
    // Hold its body until all boot native frames return and lookup mappings are
    // retired. This also removes concurrent launcher/idle guest-RAM accesses.
    while(!boot_launcher_returned.load(std::memory_order_acquire)) {
        continuous_poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!context->mips3_float_mode && context->f_odd==&context->f0.u32h,"Continuous worker lost native FR=0 mapping");
    // libultra's initial RA/SR apply to every created thread. Never copy Rare's
    // saved-register image across the native pointer in OSThread::context.
    context->r31=(gpr)(int32_t)core1_meta::first_ra;
    context->status_reg=core1_meta::first_sr;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    persistent_state::runtime::attach_worker(rdram,context,ultramodern::this_thread());
#endif
    trace("continuous_worker_entered","G1","entered",0,context,
        {{"guest_thread",hex32(ultramodern::this_thread())},{"guest_id",osGetThreadId(rdram,0)},
         {"priority",osGetThreadPri(rdram,0)},{"fr",0},{"native_context_preserved",true}});
}
void continuous_core1_ready(uint8_t*,recomp_context* context) {
    require(continuous_enabled(),"Continuous core1 callback outside continuous host");
    trace("continuous_core1_ready","G1","pass",core1_meta::core1_entry,context,
          {{"original_core1_will_execute",true},{"diagnostic_thread_gates",false}});
}
void continuous_on_init(uint8_t* rdram,recomp_context* context) {
    scene::reset();
    pacing::reset_render_work();
    // The launcher profile becomes immutable for this player session here, before
    // original startup reaches the global-settings setter. Diagnostics retain the
    // game's saved setting because frontend mode is false.
    if (continuous_frontend_mode()) {
        widescreen::latch_for_game_start();
        hud_layout::latch_for_game_start();
        visibility::latch_for_game_start();
        draw_distance::latch_for_game_start();
        features::latch_for_game_start();
#ifdef TOOIE_NATIVE_HOST
        // This queues the existing graphics configuration onto its RT64 owner
        // boundary, where the host reads the same immutable session latch.
        ultramodern::renderer::set_graphics_config(native_host::vanilla_graphics_config());
#endif
    }
    const auto rom=recomp::get_rom();
    for (size_t i=0;i<0x100000;i++)
        require(rdram[(0x400+i)^3]==rom[0x1000+i],"Continuous original initial DMA mismatch");
    require(context->f_odd==&context->f0.u32h && !context->mips3_float_mode,"Continuous boot float setup mismatch");
    const auto scatter=boot::apply_ipl3_scatter(rom,{rdram,0x800000});
    trace("ipl3_scatter_dma_applied","G2","pass",0,context,
        {{"source","original ROM IPL3"},{"rom_source",hex32(scatter.rom_source)},
         {"rdram_destination",hex32(scatter.rdram_destination)},
         {"blocks",scatter.blocks},{"block_bytes",scatter.block_bytes},
         {"rdram_stride",scatter.rdram_stride},{"bytes",scatter.blocks*scatter.block_bytes},
         {"before_guest_workers",true},{"expected_reference_copied",false}});
    capture_guest_state(rdram,context);
    trace("continuous_runtime_initialized","G1","pass",0x80000400,context,
        {{"original_initial_dma_bytes",0x100000},{"guest_reported_ram_bytes",0x800000},
         {"runtime_init","original wait_for_game_started/init"},{"bss_poisoning",false}});
    if (continuous_frontend_mode() && !overlay_references_ready.exchange(true)) {
        const auto reference=build_metadata::current().text(build_metadata::File::OverlayValidation);
        overlay_validation::load_reference_bytes(reference,platform::digest(rom,true));
    }
    prepare_core1_continuous(rdram);
}
bool run_continuous_host(const ContinuousOptions& options) {
    vi::reset_epoch();
#ifndef TOOIE_NATIVE_HOST
    require(!options.native_window,"This executable was built without native SDL/RT64 devices");
#endif
    require(!options.runtime_directory.empty(),"Continuous host requires an isolated runtime/save directory");
    require(options.frontend || options.observation_window.count()>0,"Continuous observation window must be positive");
    overlay_references_ready.store(false,std::memory_order_release);
    std::filesystem::create_directories(options.runtime_directory);
    if (!options.frontend) recomp::register_config_path(std::filesystem::absolute(options.runtime_directory));
    // wait_for_game_started deliberately uses the ordinary stored-ROM startup.
    // Copy only verified installed input, and reject an existing different file.
    if (!options.frontend) {
        auto rom_path=options.runtime_directory/"bt.n64.us.1.0.z64";
        const auto rom=recomp::get_rom();
        if (std::filesystem::exists(rom_path)) {
            require(std::filesystem::file_size(rom_path)==rom.size(),"Existing continuous runtime ROM has wrong size");
            std::ifstream in(rom_path,std::ios::binary);
            for (uint8_t expected:rom) {
                char value;
                require(bool(in.get(value)) && uint8_t(value)==expected,"Existing continuous runtime ROM differs");
            }
        } else {
            std::ofstream out(rom_path,std::ios::binary);
            out.write(reinterpret_cast<const char*>(rom.data()),rom.size());
            require(bool(out),"Cannot write isolated runtime ROM");
        }
    }
    register_overlays();
    if (!options.frontend) {
      const auto rom=recomp::get_rom();
      const auto reference=build_metadata::current().text(build_metadata::File::OverlayValidation);
      const auto rom_hash=platform::digest(rom,true);
      overlay_validation::load_reference_bytes(reference,rom_hash,[](const overlay_validation::Report& report){
            trace("overlay_image_validated","G4","pass",report.text,nullptr,
                {{"overlay_id",report.id},{"name",report.name},{"header",hex32(report.header)},{"text",hex32(report.text)},
                 {"initialized_bytes",report.initialized_bytes},{"bss_zero_bytes",report.bss_bytes},{"metadata_bytes",report.metadata_bytes},
                 {"relocation_counts_32_26_hi16_lo16",report.relocation_counts},{"original_rom_key",report.original_key},
                 {"reference_copied_to_guest",false}});
        });
    }
    si::reset();
    si::configure_observer([](const si::Observation& observation) {
        const auto bytes_hex=[](const auto& bytes) {
            static constexpr char digits[]="0123456789ABCDEF";
            std::string result;result.reserve(bytes.size()*2);
            for(uint8_t byte:bytes){result.push_back(digits[byte>>4]);result.push_back(digits[byte&15]);}
            return result;
        };
        trace("si_cic_transaction","G2","observed",0,nullptr,
            {{"phase",si::phase_name(observation.phase)},
             {"transfer_id",observation.transfer_id},{"challenge_id",observation.challenge_id},
             {"direction",observation.direction},{"guest_address",hex32(observation.guest_address)},
             {"physical_address",hex32(observation.physical_address)},
             {"raw_ra",hex32(observation.ra)},{"guest_sp",hex32(observation.sp)},
             {"challenge_hex",bytes_hex(observation.challenge)},{"dma_payload_hex",bytes_hex(observation.payload)},
             {"guest_receive_observed",false},{"queue_delivery_verified",false}});
    });
    frontend_player_mode.store(options.frontend, std::memory_order_release);
    lifecycle::enable(!options.native_window);
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
    // Fix the session medium before runtime preinit/save-file reads. Merely
    // compiling continuation support does not change normal progress saving.
    persistent_state::devices::enable_experiment(
        persistent_state::controller::practice_requested(options.persistent_practice,options.frontend));
    persistent_state::controller::initialize(options.runtime_directory,options.frontend);
#endif
    ultramodern::renderer::WindowHandle window{};
#ifdef TOOIE_NATIVE_HOST
    NativeDevicesLifetime device_lifetime;
    if(options.native_window) {
        native_host::Options devices;
        devices.data_directory=options.runtime_directory/"devices";
        devices.frontend=options.frontend;
        if (options.frontend) {
            devices.title="Banjo-Tooie: Recompiled";
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
            if(persistent_state::devices::experiment_enabled())
                devices.title="Banjo-Tooie: Recompiled - Private Practice";
#endif
            devices.keyboard_controller=false;
        }
        if(options.sdl_audio_observation)devices.sdl_observation_directory=options.runtime_directory/"audio-sdl-observation";
        if(options.audio_pacing_observation) {
            devices.audio_pacing_directory=options.runtime_directory/"audio-pacing";
            devices.audio_pacing_capacity=262144; // Bounded capacity for a fully observed ten-minute run.
        }
        if(options.audio_capture_seconds) {
            devices.audio_capture_directory=options.runtime_directory/"audio-capture";
            devices.max_capture_seconds=options.audio_capture_seconds;
        }
        devices.log=options.frontend_log ? options.frontend_log
            : [](const char* event,const Json& fields){trace(event,"G5/G6","observation",0,nullptr,fields);};
        device_lifetime.active=true;
        native_host::initialize(devices);
        window=native_host::create_window();
        if (!options.frontend) ultramodern::renderer::set_graphics_config(native_host::vanilla_graphics_config());
        audio_rsp::configure(devices.log,
            [](const std::string& why){lifecycle::fail(std::make_exception_ptr(std::runtime_error(why)));},
            !options.frontend);
        ultramodern::set_callbacks(audio_rsp::callbacks(),native_host::renderer_callbacks(),
            native_host::audio_callbacks(),options.frontend ? native_host::frontend_input_callbacks() : native_host::input_callbacks(),
            native_host::gfx_callbacks(),
            ultramodern::events::callbacks_t{},
            {},{});
    } else
#endif
    {
        ultramodern::set_callbacks({no_rsp_init,reject_rsp},{create_renderer},
            {reject_samples,no_samples,set_frequency},
            {poll_input,disconnected_input,nullptr,no_controller},{},{},{},{});
    }
    GuestMemory memory;
    exited.store(false);
    boot_launcher_returned.store(false, std::memory_order_release);
    std::atomic_bool launcher_done{false};
    trace("continuous_host_start","G1","entered",0,nullptr,
        {{"mode",options.native_window?"continuous native devices":"continuous headless diagnostic"},
         {"reject_actual_tasks_before_completion",!options.native_window},{"observation_ms",options.observation_window.count()},
         {"save_directory",std::filesystem::absolute(options.runtime_directory).string()}});
    memory.release_allowed=false;
    std::thread launcher;
    try { launcher=std::thread([&] {
        try {
            ultramodern::preinit(memory.data,window);
            if (!options.frontend) recomp::start_game(u8"bt.n64.us.1.0","");
            recomp_context context{};
            if (options.frontend) {
                while (!lifecycle::stopping() && !exited.load() && !wait_for_game_started(memory.data,&context))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!lifecycle::stopping() && !exited.load()) {
                    native_host::request_gameplay_profile();
                    while (!lifecycle::stopping() && !exited.load() && !native_host::gameplay_profile_applied())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            } else {
                wait_for_game_started(memory.data,&context);
            }
            if(!lifecycle::stopping()) {
                retire_boot_mappings();
                boot_launcher_returned.store(true,std::memory_order_release);
            }
        } catch (const ultramodern::thread_terminated&) {
        } catch (...) { lifecycle::fail(std::current_exception()); }
        launcher_done.store(true,std::memory_order_release);
    }); } catch(...) {
        // A failed std::thread constructor starts no worker; RDRAM has no users.
        memory.release_allowed=true;
        throw;
    }
    auto deadline=std::chrono::steady_clock::now()+options.observation_window;
#ifdef TOOIE_NATIVE_HOST
    auto next_capture=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    unsigned capture_index=0;
#endif
    try {
      while (!lifecycle::stopping() && !exited.load() &&
             (options.frontend || std::chrono::steady_clock::now()<deadline)) {
        check_trace_health(); // Existing catch requests stop; cleanup trace never rethrows sink failure.
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
        persistent_state::controller::poll();
#endif
#ifdef TOOIE_NATIVE_HOST
        if(options.native_window) {
            if (options.frontend) native_host::poll_frontend_events();
            else native_host::poll_events();
            if(native_host::close_requested()) lifecycle::request_stop();
            const auto now=std::chrono::steady_clock::now();
            const auto counters=native_host::counters();
            if(!options.frontend && counters.graphics_tasks_parsed && counters.vi_updates_submitted && now>=next_capture) {
                const auto directory=options.runtime_directory/"captures";
                std::filesystem::create_directories(directory);
                auto image=directory/("candidate-"+std::to_string(++capture_index)+".bmp");
                require(!std::filesystem::exists(image),"Refusing to overwrite a prior frame capture");
                bool captured=native_host::capture_visible_client_bmp(image);
                trace("presented_frame_candidate","G5","unreviewed",0,nullptr,
                    {{"path",image.string()},{"captured",captured},{"graphics_tasks_parsed",counters.graphics_tasks_parsed},
                     {"vi_updates_submitted",counters.vi_updates_submitted},{"visually_verified",false}});
                next_capture=now+(capture_index<4?std::chrono::seconds(1):std::chrono::seconds(60));
            }
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch(...) {
        lifecycle::fail(std::current_exception());
    }
    // A held fast-forward input must never survive into the timer/event joins.
    // This also restores the native audio path before device teardown.
    timing::set_fast_forward_held(false);
    // The restoration above wakes a live timer if needed. Detach the optional
    // rate-change wake path before the timer join so late input/config teardown
    // cannot enqueue into a queue that will no longer have a consumer.
    timing::set_rate_change_notifier(nullptr);
    lifecycle::request_stop();
    // Before a ROM starts, the launcher is parked in librecomp's
    // game_status.wait(None), which lifecycle::request_stop cannot interrupt.
    // No more frontend event ticks can call start_game after this point. Quit
    // releases that wait; the ordinary gameplay path retains its existing
    // resource-join ordering and calls quit after the joins below.
    if (options.frontend && !ultramodern::is_game_started())
        ultramodern::quit();
    // Generated backedge polls and runtime waits unwind workers; stop never
    // mutates live guest queues from this controller thread.
    auto close_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while (!launcher_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<close_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!launcher_done.load() || !lifecycle::wait_for_producers(std::chrono::seconds(10))) {
        trace("continuous_shutdown_incomplete","G1","failure",0,nullptr,
              {{"reason","guest failed to reach cooperative stop"},{"rdram_released",false}});
        std::cerr<<"Continuous shutdown could not establish producer quiescence; terminating process with RDRAM retained\n";
        std::_Exit(70);
    }
    launcher.join();
    // Timer is a joined producer in this mode. Then stop device producers,
    // retain active_game until their joins, and drain only after guest enqueue
    // producers are gone. Cleanup joins every native worker before deleting it.
    lifecycle::stop_and_join_timer();
    exited.store(true);
    graphics_shutdown_ready.signal();
    ultramodern::join_event_threads();
    ultramodern::join_thread_cleaner_thread();
    ultramodern::join_saving_thread();
#ifdef TOOIE_NATIVE_HOST
    if(options.native_window) {
        const auto devices=native_host::counters();
        trace("native_device_totals","G5/G6","observation",0,nullptr,
              {{"graphics_tasks_submitted",devices.graphics_tasks_submitted},{"graphics_tasks_parsed",devices.graphics_tasks_parsed},
               {"vi_updates_submitted",devices.vi_updates_submitted},{"audio_frames_queued",devices.audio_frames_queued}});
        try{native_host::shutdown();}catch(const native_host::FinalizationError&){lifecycle::fail(std::current_exception());}
        device_lifetime.active=false;
    }
#endif
    auto counts=lifecycle::counts();
    const auto dp_counts=dp::counts();
    require(counts.producers==0 && counts.created==counts.enqueued && counts.created==counts.deleted,
            "Continuous worker ownership did not drain exactly once");
    trace("continuous_shutdown_complete","G1","pass",0,nullptr,
        {{"workers_created",counts.created},{"workers_enqueued",counts.enqueued},{"workers_deleted",counts.deleted},
         {"all_native_workers_joined",true},{"timer_joined",true},{"event_threads_joined",true},
         {"saving_thread_joined",true},{"active_game_retained_until_joins",true},{"rdram_retained_until_cleanup",true},
         {"si_observer_failures",si::observer_failures()},
         {"graphics_observation",graphics_observation_summary()},
         {"title_observation",title_observation_summary()},
         {"map_actor_list_observation",map_actor_list_summary()},
         {"menu_observation",menu_observation_summary()},
         {"sfx_wait_observation",sfx_wait_observation_summary()},
         {"dp_freeze_admission",{{"parse_waits",dp_counts.parse_waits},
             {"completion_waits",dp_counts.completion_waits},{"released",dp_counts.released},
             {"aborted",dp_counts.aborted},{"freeze_sets",dp_counts.freeze_sets},
             {"freeze_clears",dp_counts.freeze_clears},{"status",dp_counts.status}}},
         {"stop_reason",lifecycle::failure()?"caught failure":
             (std::chrono::steady_clock::now()>=deadline?"observation window complete":"cooperative stop request")}});
    ultramodern::quit();
    memory.release_allowed=true;
    memory.release();
    trace("continuous_rdram_released","G1","pass",0,nullptr,
          {{"after_all_resource_joins",true},{"workers_deleted",counts.deleted}});
    // Serialization/health failure here cannot skip any producer/device join or RAM release.
    finish_overlay_call_observation();
    if (auto error=lifecycle::failure()) std::rethrow_exception(error);
    return true;
}
}
