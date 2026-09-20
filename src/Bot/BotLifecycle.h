#pragma once

#include "Define.h"
#include "Bot/BotNeeds.h"
#include <string>

class Player;
class PlayerbotAI;

enum class BotLifecycleLane : uint8
{
    Mission,
    Combat,
    Needs,
    Loot,
    Maintenance
};

enum class BotMaintenanceKind : uint8
{
    None,
    BagSpace,
    Repair,
    Supplies,
    Training
};

struct BotLifecycleAssessment
{
    BotLifecycleLane lane = BotLifecycleLane::Mission;
    BotMaintenanceKind maintenance = BotMaintenanceKind::None;
    bool urgent = false;
    uint16 score = 0;
    std::string reason;

    // Playerbots-derived diagnostics. These are observations only; Playerbots
    // remains authoritative for the actual service action and item selection.
    uint8 bagSpacePct = 0;
    uint8 durabilityPct = 100;
    bool shouldSell = false;
    bool canSell = false;
    bool shouldRepair = false;
    bool canRepair = false;
    uint32 foodCount = 0;
    uint32 drinkCount = 0;
    uint32 ammoCount = 0;

    std::string LaneName() const;
    std::string MaintenanceName() const;
};

// Builds one deterministic lifecycle decision from direct player state and
// Playerbots' own maintenance values. It never changes BotMission.
BotLifecycleAssessment AssessBotLifecycle(Player const* bot, PlayerbotAI* ai, BotNeedAssessment const& topNeed);

// Runs or resumes temporary maintenance work. Returns true while maintenance
// owns execution and the LLM should not issue unrelated mission actions.
// Non-urgent maintenance starts only when allowOptionalStart is true.
bool ServiceBotLifecycle(Player* bot,
                         PlayerbotAI* ai,
                         BotLifecycleAssessment const& assessment,
                         uint64 missionRevision,
                         bool allowOptionalStart);

// True when a bot already has a retained maintenance detour in progress.
bool HasActiveBotLifecycleOverlay(Player const* bot);
// Retain an explicit service request through travel, recovery, and interaction.
bool RequestBotLifecycleService(Player* bot, BotMaintenanceKind kind);
bool RequestBotLifecycleService(Player* bot, BotMaintenanceKind kind, std::string& reason);
bool HasPendingBotLifecycleService(Player const* bot);

struct BotServiceAvailability
{
    bool local = false;
    bool travel = false;
    bool needed = false;
    std::string reason = "service_destination_unavailable";
};

BotServiceAvailability AssessBotServiceAvailability(Player* bot, PlayerbotAI* ai, BotMaintenanceKind kind);
// False during a short retry backoff after a maintenance route times out.
// Urgent maintenance is never suppressed by this backoff.
bool CanAcquireBotLifecycleMaintenance(Player const* bot, bool urgent);

// Monotonic execution generation. It changes whenever the effective lifecycle
// lane changes. Async LLM work must match this generation before it can apply.
uint64 GetBotLifecycleGeneration(Player const* bot);
uint64 GetBotLifecycleGeneration(uint64 botGuid);
uint64 SynchronizeBotLifecycleLane(Player const* bot, BotLifecycleLane lane);

// Explicit reset hook for shutdown/reload paths. Reset also advances the
// generation so work from a previous session cannot become valid again.
void ResetBotLifecycle(Player const* bot);
