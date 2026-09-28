#include "game.hpp"
#include "continuous_host.hpp"
#include "platform_support.hpp"
#include "overlay_live_fixture.hpp"
#include "overlay_call_trace.hpp"
#include "build_metadata.hpp"
#include "tooie_build_identity.hpp"
#include "frontend_app.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <charconv>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#endif
namespace {
#ifdef _WIN32
void show_frontend_failure(const char* detail) noexcept {
    // This may run before SDL, NFD, profile creation, or session logging.
    MessageBoxA(nullptr, detail, "Banjo-Tooie: Recompiled could not start",
        MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);
}
std::filesystem::path wide_profile_argument(int expected_count) {
    int count=0;
    auto* arguments=CommandLineToArgvW(GetCommandLineW(),&count);
    if (!arguments) throw std::runtime_error("Could not read frontend profile path");
    const bool handoff = count == expected_count + 2 &&
        std::wstring_view(arguments[expected_count]) == L"--wait-for-parent";
    const auto profile=(count==expected_count || handoff)?
        std::filesystem::path(arguments[expected_count-1]):std::filesystem::path{};
    LocalFree(arguments);
    if (profile.empty()) throw std::runtime_error("Missing frontend profile path");
    return profile;
}
#endif
tooie::Json metadata_report() {
    const auto& metadata=tooie::build_metadata::current();
    tooie::Json files=tooie::Json::array();
    for(unsigned i=0;i<static_cast<unsigned>(tooie::build_metadata::File::Count);++i) {
        auto file=static_cast<tooie::build_metadata::File>(i);
        files.push_back({{"path",tooie::build_metadata::relative_path(file)},
            {"sha256",tooie::build_metadata::expected_hash(file)},{"bytes",metadata.bytes(file).size()}});
    }
    return {{"schema",1},{"identity",tooie::build_metadata::identity()},
        {"version",tooie::build_identity::version},
        {"revision_at_configure",tooie::build_identity::revision_at_configure},
        {"git_state_at_configure",tooie::build_identity::git_state_at_configure},
        {"directory",metadata.directory().string()},{"files",files},
        {"executable_sha256",tooie::file_sha256(tooie::platform::executable_path())}};
}
}
static int run_app(int argc,char**argv) {
    // Double-click and ordinary zero-argument launches enter the embedded
    // launcher. A profile override keeps engineering runs isolated while the
    // default remains the program-owned LocalAppData directory.
    if(argc==1) return tooie::frontend::run({});
    if(argc==3 && std::string_view(argv[1])=="--profile-dir") {
#ifdef _WIN32
        return tooie::frontend::run(wide_profile_argument(3));
#else
        return tooie::frontend::run(argv[2]);
#endif
    }
    if(argc==4 && std::string_view(argv[1])=="--restart-game" &&
        std::string_view(argv[2])=="--profile-dir") {
#ifdef _WIN32
        return tooie::frontend::run(wide_profile_argument(4),true);
#else
        return tooie::frontend::run(argv[3],true);
#endif
    }
    // Private save-state practice is a one-shot process launch choice, never
    // a persisted setting or an implicit change to ordinary EEPROM saves.
    if(argc==4 && std::string_view(argv[1])=="--practice-game" &&
        std::string_view(argv[2])=="--profile-dir") {
#ifdef _WIN32
        return tooie::frontend::run(wide_profile_argument(4),true,true);
#else
        return tooie::frontend::run(argv[3],true,true);
#endif
    }
    // This verified identity query starts no trace, ROM, runtime or guest worker.
    if(argc==2 && std::string_view(argv[1])=="--build-metadata-info") {
        try {std::cout<<metadata_report().dump()<<'\n';return 0;}
        catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 3;}
    }
    std::filesystem::path rom,trace_dir,save_dir;std::string stop;
    bool continuous=false,native_window=false,overlay_fixture=false,audio_pacing=false,trace_buffered=false,sdl_observation=false,overlay_calls_summary=false;unsigned run_seconds=30,audio_capture_seconds=0;
    for(int i=1;i<argc;i++) {
        std::string a=argv[i];
        if(i+1<argc && a=="--rom")rom=argv[++i];
        else if(i+1<argc && a=="--trace-dir")trace_dir=argv[++i];
        else if(i+1<argc && a=="--stop-at")stop=argv[++i];
        else if(a=="--run-continuous")continuous=true;
        else if(a=="--native-window")native_window=true;
        else if(a=="--observe-audio-pacing")audio_pacing=true;
        else if(a=="--observe-sdl-audio")sdl_observation=true;
        else if(a=="--trace-buffered")trace_buffered=true;
        else if(a=="--trace-overlay-calls-summary")overlay_calls_summary=true;
        else if(a=="--live-overlay-fixture")overlay_fixture=true;
        else if(i+1<argc && a=="--capture-audio-seconds") {
            try {audio_capture_seconds=std::stoul(argv[++i]);}catch(...){std::cerr<<"Invalid audio capture duration\n";return 2;}
            if(!audio_capture_seconds||audio_capture_seconds>3600){std::cerr<<"Audio capture duration outside 1..3600 seconds\n";return 2;}
        }
        else if(i+1<argc && a=="--save-dir")save_dir=argv[++i];
        else if(i+1<argc && a=="--run-seconds") {
            try {run_seconds=std::stoul(argv[++i]);}catch(...){std::cerr<<"Invalid run duration\n";return 2;}
            if(!run_seconds||run_seconds>86400){std::cerr<<"Run duration outside 1..86400 seconds\n";return 2;}
        }
        else {std::cerr<<"Unknown or incomplete argument: "<<a<<'\n';return 2;}
    }
    if(rom.empty()||trace_dir.empty()||(continuous?(!stop.empty()||save_dir.empty()):(stop!="boot-entry"&&stop!="core1-entry"&&stop!="idle-thread-entry"&&stop!="main-thread-entry"))) {std::cerr<<"Requires --rom PATH --trace-dir PATH and either --stop-at boot-entry|core1-entry|idle-thread-entry|main-thread-entry or --run-continuous --save-dir PATH [--run-seconds N]\n";return 2;}
    if((native_window&&!continuous)||((audio_capture_seconds||audio_pacing||sdl_observation)&&(!continuous||!native_window))) {std::cerr<<"Native devices require continuous mode; audio capture/pacing/SDL observation also requires --native-window\n";return 2;}
    if(overlay_calls_summary&&!continuous){std::cerr<<"Overlay call summary observation requires --run-continuous\n";return 2;}
    if(overlay_fixture&&!continuous){std::cerr<<"Live overlay fixture requires --run-continuous\n";return 2;}
    tooie::configure_overlay_call_observation(overlay_calls_summary);
    try {tooie::start_trace(trace_dir,{trace_buffered,std::chrono::milliseconds(16),65536});}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}
    tooie::trace("checkpoint","A","pass",0,nullptr,{{"mode",continuous?"continuous runtime host":"bounded diagnostic harness; original runtime init; no recomp::start"},{"stop_at",stop},{"compiler",__VERSION__}});
    try {
        const auto metadata=metadata_report();
        tooie::Json input_hash=nullptr;
        try {input_hash=tooie::file_sha256(rom);}catch(const std::exception&) { /* Validation below reports unavailable ROM input. */ }
        tooie::Json run_header={{"executable_sha256",tooie::file_sha256(tooie::platform::executable_path())},{"codegen_config_sha256",tooie::build_metadata::expected_hash(tooie::build_metadata::File::Config)},{"rom_sha256",input_hash},{"generator_pin","ffb39cdad1da5de07eaaa48bd1db4a89a7986771"},{"runtime_pin","ca568b6ad79b9029d14077f0c3ffa757727c5559"},
            {"build_metadata_identity",metadata["identity"]},{"build_metadata_directory",metadata["directory"]},{"build_metadata_files",metadata["files"]}};
        if(overlay_calls_summary)run_header["overlay_call_trace_policy"]="ordinary-overlay-entry-return-summary";
        tooie::trace("run_header","none","recorded",0,nullptr,std::move(run_header));
    }catch(const std::exception&e){tooie::trace("failure","none","failure",0,nullptr,{{"reason",e.what()}});tooie::fault_packet();return 3;}
    tooie::RomIdentity identity;
    try {identity=tooie::validate_and_install_rom(rom);}catch(const std::exception&e){tooie::trace("rom_rejected","B","failure",0,nullptr,{{"reason",e.what()}});tooie::fault_packet();std::cerr<<e.what()<<'\n';return 2;}
    std::ostringstream xxh;xxh<<std::hex<<identity.xxh3;
    tooie::trace("checkpoint","B","pass",0,nullptr,{{"rom",std::filesystem::absolute(rom).string()},{"bytes",identity.size},{"sha1",identity.sha1},{"sha256",identity.sha256},{"runtime_xxh3_64",xxh.str()},{"dma_source","original compressed ROM"}});
    try {
        if(continuous) {
            tooie::check_trace_health(); // No guest worker starts after a latched trace sink failure.
            tooie::enable_live_overlay_fixture(overlay_fixture);
            auto game=tooie::game_entry(identity.xxh3,false);
            game.on_init_callback=tooie::continuous_on_init;
            game.thread_create_callback=tooie::continuous_thread_create_callback;
            recomp::register_game(game);
            bool good=tooie::run_continuous_host({save_dir,std::chrono::seconds(run_seconds),native_window,audio_capture_seconds,audio_pacing,sdl_observation});
            return good?0:3;
        }
        bool main=stop=="main-thread-entry";
        tooie::check_trace_health();
        bool idle=stop=="idle-thread-entry"||main;
        auto game=tooie::game_entry(identity.xxh3,idle);recomp::register_game(game);
        if(!tooie::run_boot_diagnostic(stop=="core1-entry",idle,main))throw std::runtime_error("Entry failed");
        if(main){std::cout<<"PASS bounded idle-to-main priority handoff; stopped before original main body\n";return 0;}
        if(idle){std::cout<<"PASS bounded first scheduled Tooie thread; stopped before original idle body\n";return 0;}
        std::cout<<(stop=="core1-entry"?"PASS S1-B1 bounded core1 handoff; stopped before original func_80012030\n":"PASS S1-A generated boot entry, BSS and stack; deliberate stop before func_80000450\n");return 0;
    } catch(const std::exception&e){tooie::trace("failure","none","failure",0,nullptr,{{"reason",e.what()}});tooie::fault_packet();std::cerr<<e.what()<<'\n';return 3;}
}
int main(int argc,char**argv) {
#ifdef _WIN32
    // This gate runs before frontend/profile/log initialization. CreateProcess
    // success is not permission for the child to overlap its parent's writers.
    if (argc >= 3 && std::string_view(argv[argc-2]) == "--wait-for-parent") {
        try {
            const std::string_view text(argv[argc-1]);
            std::uint32_t parent = 0;
            const auto parsed = std::from_chars(text.data(), text.data()+text.size(), parent);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size())
                throw std::invalid_argument("Invalid frontend parent process argument");
            tooie::platform::wait_for_frontend_parent(parent);
            argc -= 2;
            argv[argc] = nullptr;
        } catch (const std::exception& error) {
            std::cerr << "Frontend handoff failed: " << error.what() << '\n';
            return 3;
        }
    }
#endif
    int result=3;
    const bool frontend_launch = argc==1 ||
        (argc==3 && std::string_view(argv[1])=="--profile-dir") ||
        (argc==4 && (std::string_view(argv[1])=="--restart-game" ||
            std::string_view(argv[1])=="--practice-game") &&
            std::string_view(argv[2])=="--profile-dir");
    const bool guarded_display = std::getenv("TOOIE_REQUIRED_DISPLAY_NAME") != nullptr;
    bool frontend_error_shown=false;
    try{result=run_app(argc,argv);}
    catch(const std::exception& e){
        std::cerr<<"Application failure: "<<e.what()<<'\n';
#ifdef _WIN32
        if(frontend_launch && !guarded_display){show_frontend_failure(e.what());frontend_error_shown=true;}
#endif
    }
    catch(...){
        std::cerr<<"Application failure: unknown exception\n";
#ifdef _WIN32
        if(frontend_launch && !guarded_display){show_frontend_failure("An unexpected error stopped the launcher.");frontend_error_shown=true;}
#endif
    }
    // These are independent so watchdog cleanup failure cannot skip trace drain.
    try{tooie::stop_watchdog();}
    catch(const std::exception& e){std::cerr<<"Watchdog cleanup failure: "<<e.what()<<'\n';result=3;}
    catch(...){std::cerr<<"Watchdog cleanup failure: unknown exception\n";result=3;}
    try{tooie::stop_trace();}
    catch(const std::exception& e){
        std::cerr<<"Trace finalization failed; output incomplete: "<<e.what()<<'\n';result=3;
        try{tooie::fault_packet();}
        catch(const std::exception& fault){std::cerr<<"Post-finalization fault packet failed: "<<fault.what()<<'\n';}
        catch(...){std::cerr<<"Post-finalization fault packet failed: unknown exception\n";}
    }
    catch(...){
        std::cerr<<"Trace finalization failed: unknown exception\n";result=3;
        try{tooie::fault_packet();}
        catch(const std::exception& fault){std::cerr<<"Post-finalization fault packet failed: "<<fault.what()<<'\n';}
        catch(...){std::cerr<<"Post-finalization fault packet failed: unknown exception\n";}
    }
    if (result==0 && frontend_launch) {
        try { tooie::frontend::relaunch_if_requested(); }
        catch(const std::exception& error) {
            std::cerr<<"Frontend relaunch failed: "<<error.what()<<'\n';
            result=3;
#ifdef _WIN32
            show_frontend_failure(error.what());
            frontend_error_shown=true;
#endif
        }
    }
#ifdef _WIN32
    if(frontend_launch && result!=0 && !frontend_error_shown && !guarded_display)
        show_frontend_failure("The launcher or game stopped before completing. Check the profile logs for details.");
#endif
    if(frontend_launch) tooie::frontend::record_process_exit(result);
    return result;
}
