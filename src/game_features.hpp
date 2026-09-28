#pragma once

#include <array>
#include <cstdint>

namespace tooie::features {

enum class Cheat : std::uint32_t {
    Feathers = 1, Eggs, Fallproof, Honeyback, Jukebox, GetJiggy,
    SuperBanjo, SuperBaddy, HoneyKing, NestKing, JiggywiggySpecial, Homing,
};

struct CheatStatus { bool typeable = false, available = false, active = false; };
enum class RequestOutcome : std::uint8_t {
    None, Queued, Applied, Unchanged, Rejected, Failed, Canceled,
};
struct RequestReceipt {
    std::uint64_t sequence = 0;
    std::uint32_t action = 0;
    std::uint32_t value = 0;
    RequestOutcome outcome = RequestOutcome::None;
};
struct CheatReadout {
    std::array<CheatStatus, 12> cheats{};
    bool current = false;
    bool observed = false;
    bool request_pending = false;
    std::uint64_t generation = 0;
    std::uint64_t applied_requests = 0;
    std::uint64_t rejected_locked_requests = 0;
    RequestReceipt receipt{};
};

// These values are the original game's AbilityID values. Only abilities with
// named, source-identified save flags are exposed individually.
enum class Move : std::uint32_t {
    GripGrab = 0x14, BreegullBlaster = 0x15, EggAim = 0x16,
    BillDrill = 0x19, BeakBayonet = 0x1A, AirborneEggAim = 0x1B,
    SplitUp = 0x1C, WingWhack = 0x1D, TalonTorpedo = 0x1E,
    SubAquaEggAim = 0x1F, TRexRoar = 0x20, ShackPack = 0x21,
    Glide = 0x22, SnoozePack = 0x23, LegSpring = 0x24,
    ClawClamberBoots = 0x25, SpringyStepShoes = 0x26, TaxiPack = 0x27,
    Hatch = 0x28, PackWhack = 0x29, SackPack = 0x2A,
    AmazeOGazeGoggles = 0x2B, FireEggs = 0x2C, GrenadeEggs = 0x2D,
    ClockworkKazooieEggs = 0x2E, IceEggs = 0x2F, FastSwimming = 0x30,
    BlueEggs = 0x31, BreegullBash = 0x32,
};

inline constexpr std::array<Move, 29> kMoves{
    Move::GripGrab, Move::BreegullBlaster, Move::EggAim, Move::BillDrill,
    Move::BeakBayonet, Move::AirborneEggAim, Move::SplitUp, Move::WingWhack,
    Move::TalonTorpedo, Move::SubAquaEggAim, Move::TRexRoar, Move::ShackPack,
    Move::Glide, Move::SnoozePack, Move::LegSpring, Move::ClawClamberBoots,
    Move::SpringyStepShoes, Move::TaxiPack, Move::Hatch, Move::PackWhack,
    Move::SackPack, Move::AmazeOGazeGoggles, Move::FireEggs,
    Move::GrenadeEggs, Move::ClockworkKazooieEggs, Move::IceEggs,
    Move::FastSwimming, Move::BlueEggs, Move::BreegullBash,
};

enum class World : std::uint32_t {
    MayahemTemple = 0, GlitterGulchMine, Witchyworld, JollyRogersLagoon,
    Terrydactyland, GruntyIndustries, HailfirePeaks, CloudCuckooland,
    CauldronKeep,
};

inline constexpr std::array<World, 9> kWorlds{
    World::MayahemTemple, World::GlitterGulchMine, World::Witchyworld,
    World::JollyRogersLagoon, World::Terrydactyland, World::GruntyIndustries,
    World::HailfirePeaks, World::CloudCuckooland, World::CauldronKeep,
};

// Counts of real source-table warp destinations in each world. Cloud
// Cuckooland and Cauldron Keep reserve three slots in the five-flag layout,
// but their original table exposes only two pads.
inline constexpr std::array<std::uint32_t, kWorlds.size()> kWarpPadTotals{
    5, 5, 5, 5, 5, 5, 5, 2, 2,
};

// Stations with original platform-switch unlock flags. Glitter Gulch Mine is
// Chuffy's home station; repairing the train there is a separate story action.
enum class TrainStation : std::uint32_t {
    Witchyworld = 0, Terrydactyland, GruntyIndustries,
    HailfirePeaksLava, HailfirePeaksIce, CliffTop,
};
inline constexpr std::array<TrainStation, 6> kTrainStations{
    TrainStation::Witchyworld, TrainStation::Terrydactyland,
    TrainStation::GruntyIndustries, TrainStation::HailfirePeaksLava,
    TrainStation::HailfirePeaksIce, TrainStation::CliffTop,
};

// Story bosses with distinct original defeated-state bookkeeping. Klungo's
// three encounters are separate because each has its own progression bit.
enum class Boss : std::uint32_t {
    Klungo1 = 0, Klungo2, Klungo3, Targitzan, OldKingCoal, MrPatch,
    LordWooFakFak, Terry, Weldar, ChillyWilly, ChilliBilli, MingyJongo, Hag1,
};

// Receipt action IDs for progression requests, shared with the Tools UI.
enum class ProgressionAction : std::uint32_t {
    Move, AllMoves, World, AllWorlds, AllNotes, AllJiggies, AllHoneycombs,
    MaxHealth, RefillHealth, AllJinjos, HubConnections, AllSilos,
    WorldWarpPads, AllWarpPads, BossState, RefillSelectedEggs, Invincibility,
    GIFrontDoor, TrainStation, AllTrainStations,
};

inline constexpr std::array<Boss, 13> kBosses{
    Boss::Klungo1, Boss::Klungo2, Boss::Klungo3, Boss::Targitzan,
    Boss::OldKingCoal, Boss::MrPatch, Boss::LordWooFakFak, Boss::Terry,
    Boss::Weldar, Boss::ChillyWilly, Boss::ChilliBilli, Boss::MingyJongo,
    Boss::Hag1,
};

struct BossSupport {
    bool can_mark_defeated = false;
    bool can_reset = false;
    const char* mark_reason = "No independent defeated state is verified.";
    const char* reset_reason = "No independent defeated state is verified.";
};
BossSupport boss_support(Boss boss) noexcept;

struct ProgressionReadout {
    std::array<bool, kMoves.size()> moves{};
    std::array<bool, kWorlds.size()> worlds{};
    std::array<bool, kBosses.size()> bosses{};
    std::array<bool, kBosses.size()> boss_edits_available{};
    std::uint32_t notes = 0;
    std::uint32_t jiggies = 0;
    std::uint32_t honeycombs = 0;
    std::uint32_t jinjos = 0;
    std::uint32_t jinjo_families = 0;
    std::uint32_t jinjo_family_rewards = 0;
    std::uint32_t current_health = 0;
    std::uint32_t health_capacity = 0;
    std::uint32_t health_upgrade_level = 0;
    std::uint32_t health_upgrade_total = 5;
    std::uint32_t hub_connections_open = 0;
    std::uint32_t silos_unlocked = 0;
    std::array<std::uint32_t, kWorlds.size()> warp_pads_active{};
    std::array<std::uint32_t, kWorlds.size()> warp_pad_totals = kWarpPadTotals;
    std::uint32_t selected_egg_item = 0;
    std::uint32_t selected_egg_ammo = 0;
    std::uint32_t selected_egg_capacity = 0;
    std::uint32_t note_total = 900;
    std::uint32_t jiggy_total = 90;
    std::uint32_t honeycomb_total = 25;
    std::uint32_t jinjo_total = 45;
    std::uint32_t jinjo_family_total = 9;
    std::uint32_t jinjo_family_reward_total = 9;
    std::uint32_t max_health_capacity = 10;
    std::uint32_t hub_connection_total = 0;
    std::uint32_t silo_total = 7;
    std::array<bool, kTrainStations.size()> train_stations_unlocked{};
    bool gi_front_door_open = false;
    bool invincible = false;
    bool current = false;
    bool observed = false;
    bool request_pending = false;
    std::uint64_t generation = 0;
    std::uint64_t applied_requests = 0;
    RequestReceipt receipt{};
};

// Launcher-owned, persisted access permission. The default is closed. Revoking
// it synchronously cancels unprocessed requests; the next guest update does not
// apply them. Enabling access alone never mutates the guest.
void set_cheats_access_enabled(bool enabled) noexcept;
bool cheats_access_enabled() noexcept;

// Requests are serviced on the guest thread. Persistent actions modify the
// active file's original state; the player must use the normal in-game save
// path to write it to disk. Health and egg refills affect current inventory;
// invincibility uses the original HoneyKing active state.
bool request_unlock_move(Move move) noexcept;
bool request_unlock_all_moves() noexcept;
bool request_unlock_world(World world) noexcept;
bool request_unlock_all_worlds() noexcept;
bool request_collect_all_notes() noexcept;
bool request_collect_all_jiggies() noexcept;
bool request_collect_all_honeycombs() noexcept;
bool request_upgrade_max_health() noexcept;
bool request_refill_health() noexcept;
bool request_collect_all_jinjos() noexcept;
bool request_unlock_all_hub_connections() noexcept;
bool request_open_gi_front_door() noexcept;
bool request_unlock_all_silos() noexcept;
bool request_unlock_train_station(TrainStation station) noexcept;
bool request_unlock_all_train_stations() noexcept;
bool request_activate_world_warp_pads(World world) noexcept;
bool request_activate_all_warp_pads() noexcept;
bool request_boss_defeated(Boss boss, bool defeated) noexcept;
bool request_refill_selected_eggs() noexcept;
bool request_invincibility_enabled(bool enabled) noexcept;
void request_progression_refresh() noexcept;
ProgressionReadout progression_readout() noexcept;

// Serviced on the guest thread. The ordinary request requires the original
// `available` flag. The explicit unlock request writes only that cheat's
// source-identified discovery/availability flags before enabling it.
bool request_cheat_enabled(Cheat cheat, bool enabled) noexcept;
bool request_unlock_and_enable_cheat(Cheat cheat) noexcept;
void request_cheat_refresh() noexcept;
CheatReadout cheat_readout() noexcept;

enum class CutsceneAspect : std::uint32_t { Original = 0, Widescreen = 1 };
void configure_cutscene_aspect(CutsceneAspect aspect) noexcept;
void latch_for_game_start() noexcept;
CutsceneAspect configured_cutscene_aspect() noexcept;
CutsceneAspect latched_cutscene_aspect() noexcept;
void set_cutscene_pillarbox_available(bool available) noexcept;
bool cutscene_pillarbox_available() noexcept;
bool cutscene_active() noexcept;
bool cutscene_requires_pillarbox() noexcept;
#ifdef TOOIE_PERSISTENT_RUNTIME_EXPERIMENT
bool persistent_requests_idle() noexcept;
void persistent_restore_epoch(bool cutscene_active) noexcept;
#endif

} // namespace tooie::features
