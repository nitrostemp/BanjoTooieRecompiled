#pragma once
#include "recomp.h"
#include "json/json.hpp"
#include <filesystem>
#include "trace_transport.hpp"
namespace tooie {
using Json = nlohmann::json;
void start_trace(const std::filesystem::path& directory, TraceTransportOptions options={});
// Lock-free hot-path query. A false result means ordinary diagnostic events
// have no transport or fault-history consumer.
bool trace_enabled() noexcept;
void check_trace_health(); // Host control loop; surfaces periodic sink failure.
void stop_trace(); // After ALL producers/watchdog join; checked drain and close.
void trace(const char* event, const char* checkpoint, const char* outcome, uint32_t pc=0, const recomp_context* ctx=nullptr, Json extra=Json::object());
void fault_packet();
void capture_guest_state(uint8_t* rdram,const recomp_context* ctx);
void start_watchdog(unsigned seconds=30);
void stop_watchdog();
std::string hex32(uint32_t value);
}
