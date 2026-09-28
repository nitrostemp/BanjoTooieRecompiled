#include "persistent_state_controller.hpp"
#include "persistent_state_scheduler.hpp"
#include "persistent_state_file.hpp"
#include "persistent_continuation_registry.hpp"
#include "persistent_state_devices.hpp"
#include "native_host_devices.hpp"
#include "runtime_lifecycle.hpp"
#include "virtual_clock.hpp"
#include "platform_support.hpp"
#include "frontend_config.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "trace.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>

extern std::atomic_bool exited;
extern "C" {
bool tooie_persistent_events_export(tooie::persistent_state::devices::Snapshot*) noexcept;
bool tooie_persistent_events_import(const tooie::persistent_state::devices::Snapshot*) noexcept;
bool tooie_persistent_events_work_empty() noexcept;
bool tooie_persistent_timer_export(tooie::persistent_state::devices::Snapshot*) noexcept;
bool tooie_persistent_timer_import(const tooie::persistent_state::devices::Snapshot*) noexcept;
bool tooie_persistent_pi_export(tooie::persistent_state::devices::Snapshot*) noexcept;
bool tooie_persistent_pi_import(const tooie::persistent_state::devices::Snapshot*) noexcept;
}
namespace tooie::persistent_state::controller {
namespace {
std::string action;
std::filesystem::path target;
bool frontend_mode=false,done=false;
unsigned attempts=0;
std::chrono::steady_clock::time_point due;
std::mutex status_mutex;
Status public_status;
struct Request {Action action;std::filesystem::path path;};
std::optional<Request> pending;
bool roundtrip=false,roundtrip_pending=false;
unsigned roundtrip_step=0;
Action roundtrip_next=Action::Restore;
std::chrono::steady_clock::time_point roundtrip_due;
constexpr auto timeout=std::chrono::milliseconds{4000};
bool valid_target(const std::filesystem::path& path) {
    return path.is_absolute()&&path.extension()==".tooie-state"&&std::filesystem::is_directory(path.parent_path());
}
Compatibility identity() {
    Compatibility result;
    result.program_sha256=continuation::generated::program_sha256();
    result.executable_sha256=platform::file_sha256(platform::executable_path());
    result.rom_sha256=platform::digest(recomp::get_rom(),true);
    result.runtime_revision="ca568b6ad79b9029d14077f0c3ffa757727c5559";
    auto settings=frontend_mode?tooie::frontend::settings::persistent_identity_payload():std::string{"continuous-native-immutable-oracle-v1"};
    result.settings_sha256=platform::digest({reinterpret_cast<const uint8_t*>(settings.data()),settings.size()},true);
    return result;
}
void fatal(uint64_t epoch) noexcept {
    // All hooks execute on this controller thread, which owns the audio lease.
    // No mixed epoch is released to the game; only terminal shutdown is allowed.
    lifecycle::fail(std::make_exception_ptr(std::runtime_error("Persistent restore failed after mutation; terminating parked epoch")));
    exited.store(true,std::memory_order_release);
    native_host::persistent_audio_abandon_for_shutdown(epoch);
    native_host::persistent_renderer_abandon_for_shutdown(epoch);
    devices::terminal_abort(epoch);
    scheduler::terminal_release();
}
devices::Hooks hooks(void* image) {
    devices::Hooks result;
    result.guest_context=image;
    result.capture_guest=scheduler::capture_guest;result.restore_guest=scheduler::restore_guest;
    result.clock_quiesce=[](uint64_t epoch,std::chrono::milliseconds) noexcept {return timing::persistent_quiesce(epoch);};
    result.clock_resume=timing::persistent_resume;result.clock_restore=timing::persistent_restore;
    result.freeze=devices::freeze_workers;result.resume=devices::resume_workers;
    result.export_state=[](devices::Snapshot& output) noexcept {
        return tooie_persistent_events_export(&output)&&tooie_persistent_timer_export(&output)&&tooie_persistent_pi_export(&output);
    };
    result.import_state=[](const devices::Snapshot& input) noexcept {
        return tooie_persistent_events_import(&input)&&tooie_persistent_timer_import(&input)&&tooie_persistent_pi_import(&input);
    };
    result.work_queues_empty=tooie_persistent_events_work_empty;
    result.external_queue_ready=scheduler::external_queue_ready;
    result.renderer_quiesce=native_host::persistent_renderer_quiesce;
    result.renderer_resume=native_host::persistent_renderer_resume;
    result.renderer_export=native_host::persistent_renderer_export;
    result.renderer_validate=native_host::persistent_renderer_validate;
    result.renderer_restore_epoch=native_host::persistent_renderer_restore_epoch;
    result.audio_quiesce=native_host::persistent_audio_quiesce;
    result.audio_resume=native_host::persistent_audio_resume;
    result.audio_export=native_host::persistent_audio_export;
    result.audio_restore_epoch=native_host::persistent_audio_restore_epoch;
    result.fatal_restore_failure=fatal;
    return result;
}
void report(const char* outcome,const std::string& reason,size_t threads=0) {
    {
        std::lock_guard lock(status_mutex);
        public_status.action=action;public_status.outcome=outcome;
        public_status.message=reason;public_status.path=target;
    }
    std::cerr<<"Persistent state "<<action<<" "<<outcome<<": "<<reason<<" threads="<<threads<<" path="<<target.string()<<'\n';
    trace("persistent_state_transaction","experimental",outcome,0,nullptr,
        {{"action",action},{"reason",reason},{"threads",threads},{"path",target.string()},{"attempt",attempts}});
}
}
bool practice_requested(bool launch_option,bool frontend) noexcept {
    if(frontend)return launch_option;
    if(launch_option)return true;
    const auto* enabled=std::getenv("TOOIE_PERSISTENT_PRACTICE");
    if(enabled&&std::string_view{enabled}=="1")return true;
    const auto* requested=std::getenv("TOOIE_PERSISTENT_ACTION");
    return requested&&(*requested!='\0');
}
Status status(){std::lock_guard lock(status_mutex);return public_status;}
bool request(Action requested,const std::filesystem::path& path) {
    std::lock_guard lock(status_mutex);
    if(!public_status.practice_enabled||lifecycle::stopping()) {
        public_status.message="Start an isolated persistent-practice session first.";return false;
    }
    if(public_status.busy)return false;
    try {
        if(!valid_target(path)) {
            public_status.outcome="refused";
            public_status.message="State path must be absolute .tooie-state with an existing parent folder.";
            return false;
        }
        if(requested!=Action::Capture&&requested!=Action::Restore)return false;
        pending=Request{requested,path};public_status.busy=true;
        public_status.action=requested==Action::Capture?"capture":"restore";
        public_status.path=path;public_status.outcome="queued";
        public_status.message="Waiting for a safe gameplay boundary.";
        return true;
    } catch(const std::exception& error) {
        public_status.outcome="error";public_status.message=error.what();return false;
    }
}
void initialize(const std::filesystem::path&,bool frontend) {
    frontend_mode=frontend;done=false;attempts=0;action.clear();target.clear();
    roundtrip=false;roundtrip_pending=false;roundtrip_step=0;
    {
        std::lock_guard lock(status_mutex);pending.reset();public_status={};
        public_status.practice_enabled=devices::experiment_enabled();
        public_status.outcome="idle";
        public_status.message=public_status.practice_enabled?
            "Isolated practice session: normal progress saves are protected.":
            "Normal session: progress saving is unchanged; persistent states require practice mode.";
    }
    const char* requested=std::getenv("TOOIE_PERSISTENT_ACTION");
    const char* private_requested=std::getenv("TOOIE_PERSISTENT_PRACTICE");
    if(frontend&&!devices::experiment_enabled()&&
        ((requested&&*requested)||(private_requested&&std::string_view{private_requested}=="1")))
        throw std::runtime_error("Persistent frontend actions require explicit --practice-game; normal play cannot silently use a private save medium");
    if(!requested||!*requested)return;
    action=requested;
    if(action!="capture"&&action!="restore")throw std::runtime_error("Unknown TOOIE_PERSISTENT_ACTION");
    if(const auto* sequence=std::getenv("TOOIE_PERSISTENT_ROUNDTRIP");sequence&&std::string_view{sequence}=="1") {
        if(action!="capture")throw std::runtime_error("Persistent roundtrip requires initial capture action");
        roundtrip=true;
    }
    const char* path=std::getenv("TOOIE_PERSISTENT_PATH");
    if(!path||!*path)throw std::runtime_error("TOOIE_PERSISTENT_PATH required");
    target=std::filesystem::u8path(path);
    if(!valid_target(target))
        throw std::runtime_error("Persistent target requires absolute .tooie-state path with existing parent");
    unsigned delay=20;
    if(const char* text=std::getenv("TOOIE_PERSISTENT_DELAY_SECONDS")) {
        const auto value=std::stoul(text);if(value<1||value>600)throw std::runtime_error("Persistent delay outside 1..600 seconds");
        delay=static_cast<unsigned>(value);
    }
    due=std::chrono::steady_clock::now()+std::chrono::seconds{delay};
    {
        std::lock_guard lock(status_mutex);public_status.busy=true;public_status.action=action;
        public_status.path=target;public_status.outcome="armed";public_status.message="Engineering action armed.";
    }
    std::cerr<<"Persistent engineering action armed: "<<action<<" in "<<delay<<"s; EEPROM is private in-memory only\n";
}
void poll() {
    if(roundtrip_pending&&!lifecycle::stopping()&&std::chrono::steady_clock::now()>=roundtrip_due) {
        if(request(roundtrip_next,target)) {
            roundtrip_pending=false;
            std::cerr<<"Persistent engineering roundtrip queued through request API: "
                <<(roundtrip_next==Action::Restore?"restore":"capture")<<'\n';
        }
    }
    {
        std::lock_guard lock(status_mutex);
        if(pending) {
            action=pending->action==Action::Capture?"capture":"restore";
            target=std::move(pending->path);pending.reset();done=false;attempts=0;
            due=std::chrono::steady_clock::now();
        }
    }
    if(action.empty()||done||lifecycle::stopping()||std::chrono::steady_clock::now()<due)return;
    struct Finish {
        ~Finish(){if(!done) {
            // A timeout may already have consumed four seconds. Give guests
            // and producers a real recovery interval after releasing leases,
            // rather than immediately freezing them again on the next poll.
            due=std::chrono::steady_clock::now()+std::chrono::seconds{1};
        } else {
            std::lock_guard lock(status_mutex);public_status.busy=false;++public_status.completed;
            if(roundtrip&&public_status.outcome=="committed"&&roundtrip_step<2) {
                roundtrip_next=roundtrip_step==0?Action::Restore:Action::Capture;
                ++roundtrip_step;roundtrip_due=std::chrono::steady_clock::now()+std::chrono::seconds{15};
                roundtrip_pending=true;
            } else if(public_status.outcome!="committed") {
                roundtrip=false;roundtrip_pending=false;
            }
        }}
    } finish;
    ++attempts;
    Snapshot image;
    try {
        auto compatibility=identity();
        if(action=="restore")image=file::load_validated(target,compatibility);
        else image.compatibility=std::move(compatibility);
        std::string reason;
        if(!scheduler::freeze(timeout,reason)) {report("refused",reason);done=attempts>=20;return;}
        bool release=true;
        try {
            if(action=="restore"&&!scheduler::validate_restore(image,reason)) {
                report("refused",reason);scheduler::release();done=attempts>=20;return;
            }
            auto callbacks=hooks(&image);
            const auto status=action=="capture"?devices::capture(callbacks,image.devices,timeout):devices::restore(callbacks,image.devices,timeout);
            if(status==devices::Status::FatalParked) {release=false;done=true;report("fatal","mixed epoch terminated");return;}
            scheduler::release();release=false;
            if(lifecycle::stopping()) {done=true;report("aborted","runtime stopped during checkpoint");return;}
            if(status!=devices::Status::Ready) {
                report("refused","device status "+std::to_string(static_cast<unsigned>(status))+" guest="+scheduler::refusal_reason()+" renderer="+native_host::persistent_renderer_refusal_reason());
                done=attempts>=20;return;
            }
            if(action=="capture")file::save_atomic(target,image);
            report("committed",std::string{action=="capture"?"durable actual-runtime image written":"restored epoch installed; execution released"}+
                " map="+std::to_string(image.scene_map_id),image.threads.size());
            done=true;
        } catch(...) {if(release)scheduler::release();throw;}
    } catch(const std::exception& error) {report("error",error.what());done=true;}
}
}
