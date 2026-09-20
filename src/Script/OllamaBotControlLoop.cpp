#include "Script/OllamaBotControlLoop.h"
#include "Script/AmigoControlControllerScript.h"
#include "Ai/ControlAction.h"
#include "Ai/ControlContract.h"
#include "Ai/BotMindState.h"
#include "Script/OllamaBotConfig.h"
#include "Bot/BotControlApi.h"
#include "Ai/LlmContext.h"
#include "Ai/LlmDispatch.h"
#include "Ai/OllamaClient.h"
#include "Ai/LlmRoles.h"
#include "DBCStores.h"
#include "Util/PlayerbotsCompat.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Item.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "GameObject.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Timer.h"
#include "Errors.h"
#include "Ai/OllamaRuntime.h"
#include "Bot/BotMovement.h"
#include "Bot/BotRecentHistory.h"
#include "Util/WorldChecks.h"
#include "Db/BotMemory.h"
#include "Bot/BotTravel.h"
#include "Bot/BotProfession.h"
#include "Bot/BotNavState.h"
#include "Bot/BotMission.h"
#include "Bot/BotNeeds.h"
#include "Bot/BotLifecycle.h"
#include "Bot/BotNavigationPenalty.h"
#include "LootObjectStack.h"
#include "Script/OllamaBotPlannerRefresh.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    std::mutex gLatestControlStateMutex;
    std::string gLatestControlStateJson;

    // Timing and tuning constants for planner/control loops.
    // Defaults are tuned to scale across many bots without spamming the control plane.
    constexpr uint32 kStrategicIntervalMs = 20000;           // 20s
    constexpr uint32 kControlIntervalMs = 2500;              // 2.5s (fallback; normally overridden via config)
    constexpr uint32 kStrategicGoalChangeCooldownMs = 60000; // 60s
    constexpr float kIdlePositionEpsilon = 0.1f;
    constexpr uint32 kOllamaFailureHoldMs = 60000;   // 60s
    constexpr uint32 kPlannerFailureDelayMs = 90000; // 90s
    constexpr uint32 kGlobalFailureWindowMs = 60000;
    constexpr uint32 kGlobalFailureThreshold = 40;
    constexpr uint32 kGlobalControlPauseMs = 3000;
    constexpr uint32 kGlobalResumeSpreadMs = 5000;

    constexpr uint32 kOllamaBaseCooldownMs = 5000; // 5 seconds
    constexpr uint32 kOllamaMaxCooldownMs = 60000; // 60 seconds
    // When entering grind mode, give the bot time to start fighting before requesting
    // another control action from the LLM (prevents rapid grind spam).
    constexpr uint32 kPostEnterGrindControlDelayMs = 10000; // 10 seconds
    constexpr uint32 kNavigationTimeoutPenaltyMs = 90000;   // 90 seconds
    constexpr float kQuestGiverApproachOffsetMeters = 1.8f;
    struct DistanceBand
    {
        // Label and concrete distance for move hop tool arguments.
        char const* label;
        float distance;
    };

    constexpr std::array<DistanceBand, 5> kMoveHopDistanceBands = {{{"very close", 12.0f},
                                                                    {"close", 18.0f},
                                                                    {"medium", 36.0f},
                                                                    {"medium far", 46.0f},
                                                                    {"far", 58.0f}}};

    std::string DistanceBandLabelForDistance(float distance)
    {
        // Convert a numeric distance into the closest allowed distance-band label.
        // NOTE: This is for LLM-facing summaries only; the engine remains authoritative.
        for (auto const &band : kMoveHopDistanceBands)
        {
            if (distance <= band.distance)
            {
                return band.label;
            }
        }
        return kMoveHopDistanceBands.back().label;
    }

    std::string DistanceText(float distance)
    {
        std::ostringstream value;
        value << std::fixed << std::setprecision(1) << distance << " yd (" << DistanceBandLabelForDistance(distance) << ")";
        return value.str();
    }

    void SuppressAutonomousMovement(Player* bot, PlayerbotAI* ai)
    {
        if (!bot || !ai || HasActiveBotLifecycleOverlay(bot))
            return;

        std::string activity;
        std::string reason;
        bool retainGrind = TryGetActivityState(bot, activity, reason) && activity == "grind";
        for (auto const& quest : bot->getQuestStatusMap())
        {
            if (quest.second.Status == QUEST_STATUS_COMPLETE)
            {
                retainGrind = false;
                ai->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);
                if (activity == "grind")
                    UpdateActivityState(bot, "quest_turn_in", "completed quest has precedence");
                break;
            }
        }
        bool hadAutonomousMovement = ai->HasStrategy("rpg", BOT_STATE_NON_COMBAT) ||
                                     ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) ||
                                     ai->HasStrategy("move random", BOT_STATE_NON_COMBAT) ||
                                     ai->HasStrategy("travel", BOT_STATE_NON_COMBAT) ||
                                     (!retainGrind && ai->HasStrategy("grind", BOT_STATE_NON_COMBAT));
        if (!hadAutonomousMovement)
            return;

        ai->ChangeStrategy("-rpg,-new rpg,-move random,-travel", BOT_STATE_NON_COMBAT);
        if (!retainGrind)
            ai->ChangeStrategy("-grind", BOT_STATE_NON_COMBAT);

        if (g_EnableOllamaBotAmigoDebug)
        {
            LOG_INFO("server.loading",
                     "[OllamaBotAmigo] Suppressed autonomous Playerbots movement for {} (retain_grind={})",
                     bot->GetName(),
                     retainGrind ? "yes" : "no");
        }
    }

    struct BotSnapshot
    {
        // Condensed view of bot/world state sent to the LLM.
        uint64 botGuid = 0;
        std::string botName;
        uint32 nowMs = 0;
        Position3 pos;
        float orientation = 0.0f;
        uint32 mapId = 0;
        uint32 navEpoch = 0;
        uint32 zoneId = 0;
        uint32 areaId = 0;
        bool inCombat = false;
        bool grindMode = false;
        bool isMoving = false;
        struct GearSlot
        {
            std::string slot;
            std::string item;
            uint32 itemLevel = 0;
        };
        float avgItemLevel = 0.0f;
        float expectedAvgItemLevel = 0.0f;
        std::string gearBand = "unknown"; // low/medium/high/unknown
        std::vector<GearSlot> lowGearSlots;
        // Travel target state (semantic completion layer).
        bool travelActive = false;
        TravelResult travelLastResult = TravelResult::None;
        uint32 travelLastChangeMs = 0;
        float travelRadius = 0.0f;
        std::string travelLabel;

        // Profession execution state (execution-only, non-combat).
        bool professionActive = false;
        ProfessionActivity professionActivity = ProfessionActivity::None;
        ProfessionResult professionLastResult = ProfessionResult::None;
        uint32 professionLastChangeMs = 0;
        // Debug/backpressure signals (safe to expose; no engine control).
        uint32 controlCooldownRemainingMs = 0;
        uint32 controlOllamaBackoffMs = 0;
        uint32 memoryPendingWrites = 0;
        uint32 memoryNextFlushMs = 0;
        uint32 idleCycles = 0;
        float hpPct = 0.0f;
        float manaPct = 0.0f;
        uint32 level = 0;
        BotMission mission;
        uint64 missionRevision = 1;
        uint64 lifecycleGeneration = 1;
        BotNeedAssessment topNeed;
        BotLifecycleAssessment lifecycle;
        bool hasWeapon = false;
        std::vector<std::string> weaponTypes;
        std::vector<std::string> professions;
        struct NavCandidate
        {
            std::string label;
            Position3 pos;
            bool canMove = false;
            // Engine-derived feasibility signals.
            bool hasLOS = false;
            bool reachable = false;
            // Derived orientation helpers for the LLM.
            float distance2d = 0.0f;
            float bearingDeg = 0.0f;
            std::string direction;
            bool temporarilyBlocked = false;
            uint32 penaltyRemainingMs = 0;
        };
        std::vector<NavCandidate> navCandidates;
        std::vector<uint32> activeQuestIds;
        struct QuestObjectiveProgress
        {
            std::string type;
            int32 targetId = 0;
            std::string targetName;
            uint32 current = 0;
            uint32 required = 0;
        };
        struct QuestProgress
        {
            uint32 questId = 0;
            std::string title;
            QuestStatus status = QUEST_STATUS_NONE;
            bool explored = false;
            std::vector<QuestObjectiveProgress> objectives;
        };
        std::vector<QuestProgress> activeQuests;
        std::string actionableGoal;
        bool postGrindDecision = false;
        std::vector<std::string> postGrindOptions;
        std::vector<ControlDecisionOption> decisionOptions;
        std::unordered_map<std::string, BotServiceAvailability> serviceAvailability;
        bool lifecycleActive = false;
        struct QuestPoi
        {
            uint32 questId = 0;
            int32 objectiveIndex = 0;
            uint32 mapId = 0;
            uint32 areaId = 0;
            Position3 pos;
            bool hasZ = false;
            bool isTurnIn = false;
        };
        std::vector<QuestPoi> questPois;
        struct NearbyEntity
        {
            std::string name;
            std::string type;
            uint32 entryId = 0;
            Position3 pos;
            float distance = 0.0f;
            bool isQuestGiver = false;
            std::string questMarker;
            bool isVendor = false;
            bool isTrainer = false;
            bool isRepair = false;
            bool isFlightMaster = false;
            std::vector<uint32> turnInQuestIds;
        };
        std::vector<NearbyEntity> nearbyEntities;
        struct QuestGiverInRange
        {
            std::string name;
            std::string type;
            uint32 entryId = 0;
            float distance = 0.0f;
            Position3 pos;
            std::vector<uint32> availableQuestIds;
            std::vector<uint32> turnInQuestIds;
            std::string questMarker;
            // Derived relevance tags for planners.
            std::vector<uint32> availableNewQuestIds;
            std::vector<uint32> turnInActiveQuestIds;
        };
        std::vector<QuestGiverInRange> questGiversInRange;

        // Recent outcomes to help the LLM adapt quickly (in-memory, size-limited).
        std::vector<BotRecentHistory::MovementAttempt> recentMovementAttempts;
        std::vector<BotRecentHistory::MovementResult> recentMovementResults;
        std::vector<BotRecentHistory::LongTermGoalChange> recentLongTermGoalChanges;
        std::vector<BotRecentHistory::ShortTermGoalsChange> recentShortTermGoalsChanges;
        std::vector<BotRecentHistory::ShortTermGoalCompletion> recentShortTermGoalCompletions;
    };

    struct WorldSnapshot
    {
        // Friendly names for the current location.
        std::string zone;
        std::string area;
    };

    struct Task
    {
        // Human-readable next action for planner summaries.
        std::string description;
    };

    class Goal
    {
    public:
        // Base interface for LLM-selected goals.
        virtual ~Goal() = default;
        virtual bool IsComplete(BotSnapshot const &snapshot) const = 0;
        virtual bool IsInvalid(BotSnapshot const &snapshot) const
        {
            (void)snapshot;
            return false;
        }
        virtual bool RequiresCombat() const { return false; }
        virtual Task NextTask(BotSnapshot const &snapshot, WorldSnapshot const &world) = 0;
        virtual nlohmann::json ToJson() const = 0;
    };

    class WorldQuestGoal : public Goal
    {
    public:
        // Represents a single quest with incomplete objectives.
        explicit WorldQuestGoal(uint32 questId)
            : questId_(questId) {}

        bool IsComplete(BotSnapshot const &snapshot) const override
        {
            if (!questId_)
            {
                return false;
            }

            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.questId == questId_)
                {
                    return quest.status == QUEST_STATUS_COMPLETE;
                }
            }

            return false;
        }

        bool IsInvalid(BotSnapshot const &snapshot) const override
        {
            if (questId_ == 0)
            {
                return true;
            }

            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.questId == questId_)
                {
                    return false;
                }
            }

            return true;
        }

        Task NextTask(BotSnapshot const & /*snapshot*/, WorldSnapshot const & /*world*/) override
        {
            return Task{"world_quest"};
        }

        bool RequiresCombat() const override
        {
            return true;
        }

        nlohmann::json ToJson() const override
        {
            return nlohmann::json{
                {"type", "world_quest"},
                {"quest_id", questId_}};
        }

    private:
        uint32 questId_ = 0;
    };

    class GrindGoal : public Goal
    {
    public:
        // "Grind" is always valid and never complete by itself.
        bool IsComplete(BotSnapshot const & /*snapshot*/) const override
        {
            return false;
        }

        bool IsInvalid(BotSnapshot const & /*snapshot*/) const override
        {
            return false;
        }

        Task NextTask(BotSnapshot const & /*snapshot*/, WorldSnapshot const & /*world*/) override
        {
            return Task{"grind"};
        }

        bool RequiresCombat() const override
        {
            return true;
        }

        nlohmann::json ToJson() const override
        {
            return nlohmann::json{{"type", "grind"}};
        }
    };

    class TravelGoal : public Goal
    {
    public:
        // Travel goal references an index into navCandidates.
        explicit TravelGoal(int navTargetIndex)
            : navTargetIndex_(navTargetIndex) {}

        bool IsComplete(BotSnapshot const & /*snapshot*/) const override
        {
            return false;
        }

        bool IsInvalid(BotSnapshot const &snapshot) const override
        {
            if (navTargetIndex_ < 0)
            {
                return true;
            }
            return static_cast<size_t>(navTargetIndex_) >= snapshot.navCandidates.size();
        }

        Task NextTask(BotSnapshot const & /*snapshot*/, WorldSnapshot const & /*world*/) override
        {
            return Task{"travel"};
        }

        nlohmann::json ToJson() const override
        {
            return nlohmann::json{
                {"type", "travel"},
                {"nav_target_index", navTargetIndex_}};
        }

    private:
        int navTargetIndex_ = -1;
    };

    class TurnInGoal : public Goal
    {
    public:
        // Turn in a quest that is already complete.
        explicit TurnInGoal(uint32 questId)
            : questId_(questId) {}

        bool IsComplete(BotSnapshot const &snapshot) const override
        {
            if (!questId_)
            {
                return false;
            }

            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.questId == questId_)
                {
                    return false;
                }
            }

            return true;
        }

        bool IsInvalid(BotSnapshot const &snapshot) const override
        {
            if (questId_ == 0)
            {
                return true;
            }

            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.questId == questId_)
                {
                    return quest.status != QUEST_STATUS_COMPLETE;
                }
            }

            return true;
        }

        Task NextTask(BotSnapshot const & /*snapshot*/, WorldSnapshot const & /*world*/) override
        {
            return Task{"turn_in"};
        }

        nlohmann::json ToJson() const override
        {
            return nlohmann::json{
                {"type", "turn_in"},
                {"quest_id", questId_}};
        }

    private:
        uint32 questId_ = 0;
    };

    struct PlannerPlan
    {
        // Long-term goal and short-term goals derived from planner output.
        std::string longTermGoal;
        std::vector<std::string> shortTermGoals;
    };

    std::string TrimCopy(std::string const &input)
    {
        // Trim without modifying the original string.
        size_t start = input.find_first_not_of(" \t\r\n");
        size_t end = input.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos)
        {
            return {};
        }
        return input.substr(start, end - start + 1);
    }

    class ThinkScheduler
    {
    public:
        // Simple periodic scheduler for planner/control ticks.
        bool ShouldRunStrategic(uint32 nowMs)
        {
            if (nowMs - lastStrategicMs_ >= kStrategicIntervalMs)
            {
                lastStrategicMs_ = nowMs;
                return true;
            }
            return false;
        }

        bool ShouldRunControl(uint32 nowMs, uint64 guid)
        {
            uint32 intervalMs = g_OllamaBotControlDelayControlMs > 0
                                    ? g_OllamaBotControlDelayControlMs
                                    : kControlIntervalMs;
            // Spread calls across bots to reduce thundering herd.
            uint32 jitterMs = static_cast<uint32>(guid % 500u);
            if (nowMs - lastControlMs_ >= intervalMs + jitterMs)
            {
                lastControlMs_ = nowMs;
                return true;
            }
            return false;
        }

    private:
        uint32 lastStrategicMs_ = 0;
        uint32 lastControlMs_ = 0;
    };

    struct PendingStrategicUpdate
    {
        // Planner output waiting to be applied on the main thread.
        PlannerPlan plan;
        uint64 missionRevision = 0;
        uint64 lifecycleGeneration = 0;
        bool hasUpdate = false;
        bool refreshedShortTermGoals = false;
    };

    std::mutex pendingMutex;
    std::unordered_map<uint64, PendingStrategicUpdate> pendingStrategicUpdates;
    std::mutex globalControlMutex;
    uint32 globalFailureWindowStartMs = 0;
    uint32 globalFailureCount = 0;
    std::atomic<uint32> globalControlPauseUntilMs{0};
    std::atomic<uint32> globalControlResumeBaseMs{0};
    std::mutex plannerSummaryLogMutex;

    std::string NormalizeCommandToken(std::string value)
    {
        // Normalize tokens for case/whitespace comparison.
        auto trim = [](std::string &text)
        {
            size_t start = text.find_first_not_of(" \t\r\n");
            size_t end = text.find_last_not_of(" \t\r\n");
            if (start == std::string::npos || end == std::string::npos)
            {
                text.clear();
                return;
            }
            text = text.substr(start, end - start + 1);
        };

        trim(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    BotMission BuildConfiguredMission()
    {
        BotMission mission;
        std::string kind = NormalizeCommandToken(g_OllamaBotControlMissionKind);
        mission.target = TrimCopy(g_OllamaBotControlMissionTarget);
        mission.role = TrimCopy(g_OllamaBotControlMissionRole);

        if (kind == "gather") mission.kind = BotMissionKind::Gather;
        else if (kind == "grind") mission.kind = BotMissionKind::Grind;
        else if (kind == "pvp_bg" || kind == "pvp") mission.kind = BotMissionKind::PvpBg;
        else if (kind == "party") mission.kind = BotMissionKind::Party;
        else if (kind == "raid") mission.kind = BotMissionKind::Raid;
        else if (kind == "goal") mission.kind = BotMissionKind::Goal;
        else mission.kind = BotMissionKind::Quest;

        // Preserve the existing hard quest-only override.
        if (g_OllamaBotControlQuestingOnly)
        {
            mission.kind = BotMissionKind::Quest;
            mission.target.clear();
            mission.role.clear();
        }

        return mission;
    }

    std::string MissionSignature(BotMission const& mission)
    {
        return mission.KindName() + "\n" + mission.target + "\n" + mission.role;
    }

    bool MissionAllowsControlCapability(BotMission const& mission, ControlAction::Capability capability, std::string& reason,
                                        bool postGrindDecision = false)
    {
        reason.clear();
        if (capability == ControlAction::Capability::TalkToQuestGiver && postGrindDecision)
            return true;
        if (capability == ControlAction::Capability::EnterAttackPull &&
            mission.kind != BotMissionKind::Grind && mission.kind != BotMissionKind::Quest)
        {
            reason = "targeted_attack_requires_named_grind";
            return false;
        }
        if (capability == ControlAction::Capability::GatherTarget && mission.kind != BotMissionKind::Gather)
        {
            reason = "targeted_gather_requires_named_gather";
            return false;
        }
        switch (mission.kind)
        {
            case BotMissionKind::Gather:
                if (capability == ControlAction::Capability::EnterGrind ||
                    capability == ControlAction::Capability::EnterAttackPull ||
                    capability == ControlAction::Capability::TalkToQuestGiver)
                {
                    reason = "gather_mission_scope";
                    return false;
                }
                if ((capability == ControlAction::Capability::Fish || capability == ControlAction::Capability::UseProfession) &&
                    NormalizeCommandToken(mission.target) != "fish" && NormalizeCommandToken(mission.target) != "fishing")
                {
                    reason = "named_gather_executor_not_available";
                    return false;
                }
                return true;
            case BotMissionKind::Grind:
                if ((capability == ControlAction::Capability::TalkToQuestGiver && !postGrindDecision) ||
                    capability == ControlAction::Capability::Fish ||
                    capability == ControlAction::Capability::UseProfession)
                {
                    reason = "grind_mission_scope";
                    return false;
                }
                if (capability == ControlAction::Capability::EnterGrind && !TrimCopy(mission.target).empty())
                {
                    // The existing grind command is not target-scoped. Do not pretend
                    // exact named grind missions are enforced until a targeted executor exists.
                    reason = "named_grind_requires_targeted_executor";
                    return false;
                }
                return true;
            case BotMissionKind::PvpBg:
                if (capability == ControlAction::Capability::EnterGrind ||
                    capability == ControlAction::Capability::EnterAttackPull ||
                    capability == ControlAction::Capability::TalkToQuestGiver ||
                    capability == ControlAction::Capability::Fish ||
                    capability == ControlAction::Capability::UseProfession)
                {
                    reason = "pvp_mission_scope";
                    return false;
                }
                return true;
            case BotMissionKind::Quest:
            case BotMissionKind::Party:
            case BotMissionKind::Raid:
            case BotMissionKind::Goal:
            default:
                return true;
        }
    }

    bool ServiceDeterministicLifecycle(Player* bot,
                                       PlayerbotAI* ai,
                                       uint64 missionRevision,
                                       bool allowOptionalMaintenance)
    {
        BotNeedAssessment topNeed = AssessBotNeeds(bot);
        BotLifecycleAssessment lifecycle = AssessBotLifecycle(bot, ai, topNeed);

        const bool maintenanceOwns = lifecycle.lane == BotLifecycleLane::Maintenance &&
                                     (lifecycle.urgent || allowOptionalMaintenance || HasActiveBotLifecycleOverlay(bot) ||
                                      HasPendingBotLifecycleService(bot)) &&
                                     CanAcquireBotLifecycleMaintenance(bot, lifecycle.urgent);
        BotLifecycleLane effectiveLane = BotLifecycleLane::Mission;
        if (lifecycle.lane == BotLifecycleLane::Combat)
            effectiveLane = BotLifecycleLane::Combat;
        else if (lifecycle.lane == BotLifecycleLane::Needs)
            effectiveLane = BotLifecycleLane::Needs;
        else if (lifecycle.lane == BotLifecycleLane::Loot)
            effectiveLane = BotLifecycleLane::Loot;
        else if (maintenanceOwns)
            effectiveLane = BotLifecycleLane::Maintenance;
        SynchronizeBotLifecycleLane(bot, effectiveLane);

        // Higher-priority deterministic lanes suspend a retained maintenance
        // overlay without changing the durable mission.
        if (lifecycle.lane != BotLifecycleLane::Maintenance)
            ServiceBotLifecycle(bot, ai, lifecycle, missionRevision, allowOptionalMaintenance);

        if (lifecycle.lane == BotLifecycleLane::Combat)
        {
            // Playerbots owns normal combat after Amigo has selected/validated
            // the engagement. Do not spend LLM calls steering an active fight.
            UpdateActivityState(bot, "combat", lifecycle.reason);
            return true;
        }

        if (lifecycle.lane == BotLifecycleLane::Needs)
        {
            char const* recoveryAction = nullptr;
            switch (topNeed.kind)
            {
                case BotNeedKind::Health: recoveryAction = "food"; break;
                case BotNeedKind::Mana: recoveryAction = "drink"; break;
                case BotNeedKind::Survival:
                    if (bot && bot->IsAlive())
                        recoveryAction = "flee";
                    break;
                case BotNeedKind::None:
                default:
                    break;
            }

            bool started = false;
            if (recoveryAction)
                started = ExecutePlayerbotAction(bot, recoveryAction);

            if (g_EnableOllamaBotAmigoDebug && bot)
            {
                LOG_INFO("server.loading",
                         "[OllamaBotAmigo] Lifecycle needs lane for {}: kind={} reason={} playerbots_action={} started={}",
                         bot->GetName(),
                         topNeed.KindName(),
                         topNeed.reason,
                         recoveryAction ? recoveryAction : "native_dead_state",
                         started ? "yes" : "no");
            }
            return true;
        }

        if (lifecycle.lane == BotLifecycleLane::Loot)
        {
            // Drive Playerbots' existing loot state machine directly. These
            // actions retain Playerbots' own distance, ownership, movement,
            // lock/skill, and open-loot checks. Repeated ticks advance from
            // target selection to approach and finally opening the target.
            if (AiObjectContext* context = ai->GetAiObjectContext())
            {
                if (LootObjectStack* availableLoot = context->GetValue<LootObjectStack*>("available loot")->Get())
                {
                    LootObject pendingLoot = availableLoot->GetLoot();
                    if (!pendingLoot.IsEmpty())
                        context->GetValue<LootObject>("loot target")->Set(pendingLoot);
                }
            }
            const bool selected = ExecutePlayerbotAction(bot, "loot");
            const bool moved = ExecutePlayerbotAction(bot, "move to loot");
            const bool opened = ExecutePlayerbotAction(bot, "open loot");
            UpdateActivityState(bot, "loot", lifecycle.reason);
            if (g_EnableOllamaBotAmigoDebug && bot)
            {
                LOG_INFO("server.loading",
                         "[OllamaBotAmigo] Lifecycle loot lane for {}: selected={} moved={} opened={} reason='{}'",
                         bot->GetName(),
                         selected ? "yes" : "no",
                         moved ? "yes" : "no",
                         opened ? "yes" : "no",
                         lifecycle.reason);
            }
            return true;
        }

        if (maintenanceOwns)
        {
            if (ServiceBotLifecycle(bot, ai, lifecycle, missionRevision, allowOptionalMaintenance))
            {
                return true;
            }
        }

        return false;
    }

    bool UseCompactPromptFormat()
    {
        return NormalizeCommandToken(g_OllamaBotControlPromptFormat) == "compact";
    }

    uint64 GetNowMs()
    {
        // Monotonic clock for LLM context timestamps.
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint32 ReadEnvDelayMs(char const* name, uint32 fallback)
    {
        char const* value = std::getenv(name);
        if (!value || !*value)
            return fallback;

        char *endptr = nullptr;
        unsigned long parsed = std::strtoul(value, &endptr, 10);
        if (endptr == value)
            return fallback;

        if (parsed > std::numeric_limits<uint32>::max())
            return fallback;

        return static_cast<uint32>(parsed);
    }

    uint32 GetPlannerShortTermDelayMs()
    {
        uint32 configured = g_OllamaBotControlDelayStgMs;
        // Optional env override (no rebuild of config needed):
        //   AMIGO_PLANNER_SHORT_DELAY_MS=30000
        return ReadEnvDelayMs("AMIGO_PLANNER_SHORT_DELAY_MS", configured);
    }

    uint32 GetPlannerLongTermDelayMs()
    {
        uint32 configured = g_OllamaBotControlDelayLtgMs;
        // Optional env override:
        //   AMIGO_PLANNER_LONG_DELAY_MS=900000
        return ReadEnvDelayMs("AMIGO_PLANNER_LONG_DELAY_MS", configured);
    }

    std::string BuildPlanSummary(std::string const &longTermGoal,
                                 std::vector<std::string> const &shortTermGoals,
                                 size_t shortTermIndex)
    {
        // Summarize the current plan for logs/debug output.
        std::ostringstream oss;
        if (!longTermGoal.empty())
        {
            oss << "long_term_goal: " << longTermGoal;
        }
        else
        {
            oss << "long_term_goal: none";
        }
        if (!shortTermGoals.empty())
        {
            size_t index = std::min(shortTermIndex, shortTermGoals.size() - 1);
            oss << " | short_term_goal (" << (index + 1) << "/" << shortTermGoals.size() << "): "
                << shortTermGoals[index];
        }
        return oss.str();
    }

    std::string ExtractPlannerSentence(std::string const &reply)
    {
        // Pull the first non-empty line from a planner response.
        std::istringstream iss(reply);
        std::string line;
        while (std::getline(iss, line))
        {
            line = TrimCopy(line);
            if (!line.empty())
            {
                if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
                {
                    line = line.substr(1, line.size() - 2);
                }
                return TrimCopy(line);
            }
        }
        return {};
    }

    bool LooksLikeJsonOrToolBlock(std::string const &text)
    {
        std::string s = TrimCopy(text);
        if (s.empty())
        {
            return false;
        }
        // Tool blocks.
        if (s.find("<tool_call>") != std::string::npos || s.find("</tool_call>") != std::string::npos)
        {
            return true;
        }
        // JSON-like structures or controller-style payloads.
        if (s.find('{') != std::string::npos || s.find('[') != std::string::npos)
        {
            return true;
        }
        // Common JSON fields seen in tool calls.
        if (s.find("\"name\"") != std::string::npos || s.find("\"arguments\"") != std::string::npos)
        {
            return true;
        }
        return false;
    }

    size_t CountSentenceTerminators(std::string const &text)
    {
        size_t count = 0;
        bool inTerminatorRun = false;
        for (char c : text)
        {
            if (c == '.' || c == '!' || c == '?')
            {
                if (!inTerminatorRun)
                {
                    count += 1;
                    inTerminatorRun = true;
                }
            }
            else
            {
                inTerminatorRun = false;
            }
        }
        return count;
    }

    std::string StripListPrefix(std::string const &text)
    {
        std::string s = TrimCopy(text);
        if (s.empty())
        {
            return s;
        }
        if (s[0] == '-' || s[0] == '*')
        {
            size_t i = 1;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            {
                ++i;
            }
            return TrimCopy(s.substr(i));
        }
        // "1. foo" or "1) foo" or "(1) foo"
        if (s.size() >= 3 && s[0] == '(')
        {
            size_t close = s.find(')');
            if (close != std::string::npos && close > 1 && close <= 4)
            {
                bool numeric = true;
                for (size_t i = 1; i < close; ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(s[i])))
                    {
                        numeric = false;
                        break;
                    }
                }
                if (numeric)
                {
                    size_t i = close + 1;
                    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                    {
                        ++i;
                    }
                    return TrimCopy(s.substr(i));
                }
            }
        }
        size_t dotPos = s.find('.');
        if (dotPos != std::string::npos && dotPos > 0 && dotPos <= 3)
        {
            bool numeric = true;
            for (size_t i = 0; i < dotPos; ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(s[i])))
                {
                    numeric = false;
                    break;
                }
            }
            if (numeric)
            {
                size_t i = dotPos + 1;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                {
                    ++i;
                }
                return TrimCopy(s.substr(i));
            }
        }
        size_t parenPos = s.find(')');
        if (parenPos != std::string::npos && parenPos > 0 && parenPos <= 3)
        {
            bool numeric = true;
            for (size_t i = 0; i < parenPos; ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(s[i])))
                {
                    numeric = false;
                    break;
                }
            }
            if (numeric)
            {
                size_t i = parenPos + 1;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                {
                    ++i;
                }
                return TrimCopy(s.substr(i));
            }
        }
        return s;
    }

    bool ValidatePlannerSentence(std::string const& text, std::string& outReason)
    {
        std::string s = StripListPrefix(text);
        if (s.empty())
        {
            outReason = "empty";
            return false;
        }
        if (s.size() > 220)
        {
            outReason = "too_long";
            return false;
        }
        if (LooksLikeJsonOrToolBlock(s))
        {
            outReason = "json_or_tool";
            return false;
        }
        if (CountSentenceTerminators(s) > 1)
        {
            outReason = "multi_sentence";
            return false;
        }
        if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos)
        {
            outReason = "contains_newlines";
            return false;
        }
        outReason.clear();
        return true;
    }

    bool ValidateShortTermGoal(std::string const& text, std::string& outReason)
    {
        std::string s = StripListPrefix(text);
        if (s.empty())
        {
            outReason = "empty";
            return false;
        }
        if (s.size() > 260)
        {
            outReason = "too_long";
            return false;
        }
        if (LooksLikeJsonOrToolBlock(s))
        {
            outReason = "json_or_tool";
            return false;
        }
        if (CountSentenceTerminators(s) > 1)
        {
            outReason = "multi_sentence";
            return false;
        }
        if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos)
        {
            outReason = "contains_newlines";
            return false;
        }
        outReason.clear();
        return true;
    }

    std::string ParseShortTermGoal(std::string const& reply)
    {
        // Short-term planner is expected to return exactly one non-empty line.
        return ExtractPlannerSentence(reply);
    }

    std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool ContainsInsensitive(std::string const &haystack, std::string const &needle)
    {
        if (needle.empty())
        {
            return false;
        }
        std::string hay = ToLowerCopy(haystack);
        std::string ned = ToLowerCopy(needle);
        return hay.find(ned) != std::string::npos;
    }

    std::string QuestStatusToString(QuestStatus status);

    BotSnapshot::QuestProgress const *FindFocusQuest(BotSnapshot const &bot, std::string const &longTermGoal)
    {
        BotSnapshot::QuestProgress const *best = nullptr;
        size_t bestLen = 0;
        for (auto const &quest : bot.activeQuests)
        {
            if (quest.title.empty())
            {
                continue;
            }
            if (ContainsInsensitive(longTermGoal, quest.title))
            {
                if (quest.title.size() > bestLen)
                {
                    best = &quest;
                    bestLen = quest.title.size();
                }
            }
        }
        return best;
    }

    std::string BuildFocusQuestBlock(BotSnapshot::QuestProgress const &quest)
    {
        std::ostringstream oss;
        oss << "title: " << quest.title << "\n";
        oss << "status: " << QuestStatusToString(quest.status) << "\n";
        if (!quest.objectives.empty())
        {
            oss << "objectives:\n";
            for (auto const &objective : quest.objectives)
            {
                oss << "- " << objective.type;
                if (!objective.targetName.empty())
                {
                    oss << " " << objective.targetName;
                }
                else if (objective.targetId != 0)
                {
                    oss << " " << objective.targetId;
                }
                oss << " " << objective.current << "/" << objective.required << "\n";
            }
        }
        return oss.str();
    }

    bool MentionsOtherQuest(std::string const &text,
                            std::vector<BotSnapshot::QuestProgress> const &quests,
                            std::string const &focusTitle)
    {
        for (auto const &quest : quests)
        {
            if (quest.title.empty())
            {
                continue;
            }
            if (!focusTitle.empty() && quest.title == focusTitle)
            {
                continue;
            }
            if (ContainsInsensitive(text, quest.title))
            {
                return true;
            }
        }
        return false;
    }

    std::string CurrentShortTermGoal(std::vector<std::string> const &goals, size_t index)
    {
        if (goals.empty())
        {
            return {};
        }
        size_t clamped = std::min(index, goals.size() - 1);
        return goals[clamped];
    }

    std::string SummarizeControlAction(ControlAction const &action)
    {
        // Summary text stored in the LLM context for debugging.
        std::ostringstream oss;
        switch (action.capability)
        {
        case ControlAction::Capability::MoveHop:
            oss << "move_hop"
                << " nav_epoch=" << action.navEpoch;
            if (!action.navCandidateId.empty())
            {
                oss << " candidate_id=" << action.navCandidateId;
            }
            break;
        case ControlAction::Capability::MoveHopNpc:
            oss << "move_hop_npc";
            if (action.npcEntryId > 0)
            {
                oss << " entry_id=" << action.npcEntryId;
            }
            break;
        case ControlAction::Capability::EnterGrind:
            oss << "enter_grind";
            break;
        case ControlAction::Capability::StopGrind:
            oss << "stop_grind";
            break;
        case ControlAction::Capability::Stay:
            oss << "stay";
            break;
        case ControlAction::Capability::Unstay:
            oss << "unstay";
            break;
        case ControlAction::Capability::TalkToQuestGiver:
            oss << "talk_to_quest_giver";
            if (action.questId > 0)
            {
                oss << " quest_id=" << action.questId;
            }
            break;
        case ControlAction::Capability::VendorSell: oss << "vendor_sell"; break;
        case ControlAction::Capability::VendorBuyUseful: oss << "vendor_buy_useful"; break;
        case ControlAction::Capability::Repair: oss << "repair"; break;
        case ControlAction::Capability::Trainer: oss << "trainer"; break;
        case ControlAction::Capability::Hearthstone: oss << "hearthstone"; break;
        case ControlAction::Capability::Taxi: oss << "taxi"; break;
        case ControlAction::Capability::Loot: oss << "loot"; break;
        case ControlAction::Capability::Food: oss << "food"; break;
        case ControlAction::Capability::Drink: oss << "drink"; break;
        case ControlAction::Capability::Maintenance: oss << "maintenance"; break;
        case ControlAction::Capability::Fish:
            oss << "fish";
            break;
        case ControlAction::Capability::UseProfession:
            oss << "profession";
            if (!action.professionSkill.empty())
            {
                oss << " skill=" << action.professionSkill;
            }
            if (!action.professionIntent.empty())
            {
                oss << " intent=" << action.professionIntent;
            }
            break;
        case ControlAction::Capability::Idle:
        default:
            oss << "idle";
            break;
        }
        return oss.str();
    }

    std::string NormalizeAreaToken(std::string value)
    {
        // Normalize zone/area names for compact JSON fields.
        std::string output;
        output.reserve(value.size());
        bool lastWasUnderscore = false;
        for (unsigned char c : value)
        {
            if (std::isalnum(c))
            {
                output.push_back(static_cast<char>(std::tolower(c)));
                lastWasUnderscore = false;
            }
            else if (!lastWasUnderscore)
            {
                output.push_back('_');
                lastWasUnderscore = true;
            }
        }

        while (!output.empty() && output.front() == '_')
        {
            output.erase(output.begin());
        }
        while (!output.empty() && output.back() == '_')
        {
            output.pop_back();
        }
        if (output.empty())
        {
            output = "unknown";
        }
        return output;
    }

    std::string QuestStatusToString(QuestStatus status)
    {
        // Convert enum to a stable string for LLM consumption.
        switch (status)
        {
        case QUEST_STATUS_INCOMPLETE:
            return "incomplete";
        case QUEST_STATUS_COMPLETE:
            return "complete";
        case QUEST_STATUS_FAILED:
            return "failed";
        case QUEST_STATUS_REWARDED:
            return "rewarded";
        case QUEST_STATUS_NONE:
        default:
            return "none";
        }
    }

    bool IsFollowingCorrectly(Player *bot, PlayerbotAI *ai)
    {
        // Follow correctness is based on Amigo's explicit temporary peer
        // directive, never on Playerbots' master pointer. Group membership by
        // itself does not grant authority.
        if (!bot || !ai)
            return false;

        AmigoBotMindSnapshot mind = AmigoMindSnapshot(bot->GetGUID().GetRawValue());
        if (mind.peerDirective.kind != AmigoPeerDirectiveKind::Follow || !mind.peerDirective.targetGuid)
            return false;

        Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid(mind.peerDirective.targetGuid));
        if (!target || !target->IsInWorld() || target->GetGroup() != bot->GetGroup())
            return false;

        return bot->IsWithinDistInMap(target, sPlayerbotAIConfig.followDistance);
    }

    struct ToolCall
    {
        // Parsed tool call output from the control LLM.
        std::string name;
        nlohmann::json arguments;
    };

    using ControlToolDefinition = ControlActionDefinition;

    std::vector<ControlToolDefinition> const& kControlTools = GetControlActionCatalog();

    bool TryExtractToolCall(std::string const &reply, ToolCall &outCall, std::string &outJson)
    {
        // Parse the first <tool_call> block found in the response.
        static std::string const kStartTag = "<tool_call>";
        static std::string const kEndTag = "</tool_call>";

        size_t start = reply.find(kStartTag);
        if (start == std::string::npos)
        {
            return false;
        }
        size_t end = reply.find(kEndTag, start + kStartTag.size());
        if (end == std::string::npos)
        {
            return false;
        }

        size_t contentStart = start + kStartTag.size();
        std::string inner = TrimCopy(reply.substr(contentStart, end - contentStart));
        if (inner.empty())
        {
            return false;
        }

        try
        {
            nlohmann::json parsed = nlohmann::json::parse(inner);
            if (!parsed.contains("name") || !parsed["name"].is_string())
            {
                return false;
            }
            outCall.name = NormalizeCommandToken(parsed["name"].get<std::string>());
            if (parsed.contains("arguments"))
            {
                outCall.arguments = parsed["arguments"];
            }
            else
            {
                outCall.arguments = nlohmann::json::object();
            }
            outJson = parsed.dump(2);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TryExtractSingleToolCall(std::string const &reply, ToolCall &outCall, std::string &outJson)
    {
        // Ensure the output is exactly one tool call block.
        static std::string const kStartTag = "<tool_call>";
        static std::string const kEndTag = "</tool_call>";

        std::string trimmed = TrimCopy(reply);
        if (trimmed.rfind(kStartTag, 0) != 0)
        {
            return false;
        }
        if (trimmed.size() < kEndTag.size() || trimmed.find(kEndTag) != trimmed.size() - kEndTag.size())
        {
            return false;
        }

        return TryExtractToolCall(trimmed, outCall, outJson);
    }

    bool FindControlToolDefinition(std::string const &name, ControlToolDefinition &outDefinition)
    {
        // Look up tool metadata by name.
        for (auto const& tool : kControlTools)
        {
            if (tool.supported && name == tool.name)
            {
                outDefinition = tool;
                return true;
            }
        }
        return false;
    }

    bool ParseProfessionArguments(nlohmann::json const &arguments, std::string &outSkill, std::string &outIntent)
    {
        outSkill.clear();
        outIntent.clear();

        if (!arguments.is_object())
        {
            return false;
        }
        if (!arguments.contains("skill") || !arguments["skill"].is_string())
        {
            return false;
        }
        if (!arguments.contains("intent") || !arguments["intent"].is_string())
        {
            return false;
        }

        outSkill = NormalizeCommandToken(arguments["skill"].get<std::string>());
        outIntent = NormalizeCommandToken(arguments["intent"].get<std::string>());
        return !outSkill.empty() && !outIntent.empty();
    }
    std::string BuildControlToolList(std::string const &prefix)
    {
        // Build a bullet list of tools for the LLM prompt.
        std::ostringstream oss;
        for (size_t i = 0; i < kControlTools.size(); ++i)
        {
            auto const& tool = kControlTools[i];
            if (!tool.supported)
                continue;
            oss << prefix << tool.signature;
            char const* description = tool.description;
            if (description && description[0] != '\0')
            {
                oss << " — " << description;
            }
            if (i + 1 < kControlTools.size())
            {
                oss << "\n";
            }
        }
        return oss.str();
    }

    std::string QueryOllamaLLMOnce(std::string const &prompt, std::string const &model, bool requestThink)
    {
        // Worker-side provider-neutral request. Ollama and oMLX share this path.
        AmigoOllamaResult result = QueryOllamaLLMEx(model, prompt, requestThink);
        if (!result.ok)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] LLM request failed: {}", result.error);
            return "";
        }
        return result.text;
    }

    std::string BuildControlToolInstructions(std::string const &stateToken)
    {
        // Shared instruction block appended to control prompts.
        std::ostringstream oss;
        oss << "Available control tools (choose exactly one):\n";
        oss << BuildControlToolList("- ");
        oss << R"(

Rules:
- Output one <tool_call> block, or no output when no new action is required.
- request_move_hop: choose a candidate from )";
        oss << stateToken;
        oss << R"(.nav.candidates by its candidate_id, and echo )";
        oss << stateToken;
        oss << R"(.nav.nav_epoch.
  Only choose candidates where can_move is true (and preferably reachable is true).
- request_move_hop_npc: entry_id must match a nearby NPC in )";
        oss << stateToken;
        oss << R"(.nearby_entities (type \"npc\"). This moves near the NPC but does not talk.
- request_talk_to_quest_giver: quest_id must be in )";
        oss << stateToken;
        oss << R"(.quest_givers_in_range entries (available_quest_ids or turn_in_quest_ids).
- If )";
        oss << stateToken;
        oss << R"(.quest_givers_in_range is not empty, prioritize request_talk_to_quest_giver.
- request_stop_grind: call this when )";
        oss << stateToken;
        oss << R"(.bot.grind_mode is true and you need to travel/quest/talk; it disables grinding.
- request_profession supports only skill="fishing" and intent="fish".
- If no valid control action exists, return no output.

Tool call format:
<tool_call>
{"name":"request_idle","arguments":{}}
</tool_call>

request_move_hop format:
<tool_call>
{"name":"request_move_hop","arguments":{"nav_epoch":42,"candidate_id":"nav_0"}}
</tool_call>

request_move_hop_npc format:
<tool_call>
{"name":"request_move_hop_npc","arguments":{"entry_id":12345}}
</tool_call>

request_profession format:
<tool_call>
{"name":"request_profession","arguments":{"skill":"fishing","intent":"fish"}}
</tool_call>
)";
        return oss.str();
    }

    char const* CapabilityName(ControlAction::Capability capability)
    {
        // Human-readable labels for logging and summaries.
        switch (capability)
        {
        case ControlAction::Capability::Idle:
            return "idle";
        case ControlAction::Capability::MoveHop:
            return "move_hop";
        case ControlAction::Capability::MoveHopNpc:
            return "move_hop_npc";
        case ControlAction::Capability::EnterGrind:
            return "enter_grind";
        case ControlAction::Capability::StopGrind:
            return "stop_grind";
        case ControlAction::Capability::EnterAttackPull:
            return "enter_attack_pull";
        case ControlAction::Capability::GatherTarget:
            return "gather_target";
        case ControlAction::Capability::Stay:
            return "stay";
        case ControlAction::Capability::Unstay:
            return "unstay";
        case ControlAction::Capability::TalkToQuestGiver:
            return "talk_to_quest_giver";
        case ControlAction::Capability::VendorSell: return "vendor_sell";
        case ControlAction::Capability::VendorBuyUseful: return "vendor_buy_useful";
        case ControlAction::Capability::Repair: return "repair";
        case ControlAction::Capability::Trainer: return "trainer";
        case ControlAction::Capability::Hearthstone: return "hearthstone";
        case ControlAction::Capability::Taxi: return "taxi";
        case ControlAction::Capability::Loot: return "loot";
        case ControlAction::Capability::Food: return "food";
        case ControlAction::Capability::Drink: return "drink";
        case ControlAction::Capability::Maintenance: return "maintenance";
        case ControlAction::Capability::Fish:
            return "fish";
        case ControlAction::Capability::UseProfession:
            return "profession";
        case ControlAction::Capability::TurnLeft90:
            return "turn_left_90";
        case ControlAction::Capability::TurnRight90:
            return "turn_right_90";
        case ControlAction::Capability::TurnAround:
            return "turn_around";
        default:
            return "unknown";
        }
    }

    float Distance2d(Position3 const &a, Position3 const &b);

    float BearingDegrees(Position3 const &from, Position3 const &to);

    std::string DirectionLabelFromBearing(float bearingDeg);

    std::vector<BotSnapshot::NavCandidate> BuildNavCandidates(Player *bot)
    {
        // Build forward/back/left/right candidates around the bot.
        std::vector<BotSnapshot::NavCandidate> candidates;
        if (!bot)
        {
            return candidates;
        }

        float baseDistance = std::max(g_OllamaBotControlNavBaseDistance, sPlayerbotAIConfig.followDistance);
        if (!(baseDistance > 0.0f))
        {
            baseDistance = 6.0f;
        }
        float distanceMultiplier = g_OllamaBotControlNavDistanceMultiplier;
        if (!(distanceMultiplier > 1.0f))
        {
            distanceMultiplier = 2.0f;
        }
        float maxDistance = g_OllamaBotControlNavMaxDistance;
        if (!(maxDistance > 0.0f))
        {
            maxDistance = 60.0f;
        }
        uint32 bands = g_OllamaBotControlNavDistanceBands;
        if (bands < 1)
        {
            bands = 1;
        }
        if (bands > 6)
        {
            bands = 6;
        }
        float const orientation = bot->GetOrientation();
        float const cosO = std::cos(orientation);
        float const sinO = std::sin(orientation);

        Position3 origin{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        Map *map = bot->GetMap();
        uint32 mapId = bot->GetMapId();

        auto addCandidate = [&](std::string label, float dx, float dy)
        {
            BotSnapshot::NavCandidate candidate;
            candidate.label = std::move(label);
            float x = origin.x + dx;
            float y = origin.y + dy;
            float z = origin.z;

            // Resolve a ground/water Z at the candidate X/Y to avoid "mid-air" points.
            if (map)
            {
                float height = map->GetHeight(x, y, MAX_HEIGHT);
                float water = map->GetWaterLevel(x, y);
                float candidateZ = std::max(height, water);
                if (candidateZ != INVALID_HEIGHT)
                    z = candidateZ;
            }

            candidate.pos = Position3{x, y, z};

            // Derived, engine-backed feasibility signals.
            WorldPosition wp(mapId, x, y, z);
            candidate.hasLOS = WorldChecks::IsWithinLOS(bot, wp);
            candidate.reachable = WorldChecks::CanReach(bot, wp);

            // Presentation helpers for the LLM.
            candidate.distance2d = Distance2d(origin, candidate.pos);
            candidate.bearingDeg = BearingDegrees(origin, candidate.pos);
            candidate.direction = DirectionLabelFromBearing(candidate.bearingDeg);
            candidates.push_back(std::move(candidate));
        };

        std::vector<float> distances;
        distances.reserve(bands);
        float current = baseDistance;
        for (uint32 i = 0; i < bands; ++i)
        {
            distances.push_back(std::min(current, maxDistance));
            current *= distanceMultiplier;
        }

        float const diagScale = 0.70710677f;

        for (float dist : distances)
        {
            float fwdX = dist * cosO;
            float fwdY = dist * sinO;
            float rightX = dist * sinO;
            float rightY = -dist * cosO;

            addCandidate("forward", fwdX, fwdY);
            addCandidate("backward", -fwdX, -fwdY);
            addCandidate("left", -rightX, -rightY);
            addCandidate("right", rightX, rightY);

            float diagX = fwdX * diagScale;
            float diagY = fwdY * diagScale;
            float diagRX = rightX * diagScale;
            float diagRY = rightY * diagScale;

            addCandidate("forward_left", diagX - diagRX, diagY - diagRY);
            addCandidate("forward_right", diagX + diagRX, diagY + diagRY);
            addCandidate("backward_left", -diagX - diagRX, -diagY - diagRY);
            addCandidate("backward_right", -diagX + diagRX, -diagY + diagRY);
        }

        return candidates;
    }

    void AppendQuestGiverNavCandidates(Player *bot,
                                       std::vector<BotSnapshot::NearbyEntity> const &nearbyEntities,
                                       std::vector<BotSnapshot::NavCandidate> &candidates,
                                       size_t maxTargets = 4)
    {
        if (!bot || nearbyEntities.empty())
        {
            return;
        }

        std::vector<BotSnapshot::NearbyEntity> questGivers;
        questGivers.reserve(nearbyEntities.size());
        for (auto const &entity : nearbyEntities)
        {
            if (entity.type == "npc" && entity.isQuestGiver && !entity.questMarker.empty())
            {
                questGivers.push_back(entity);
            }
        }
        if (questGivers.empty())
        {
            return;
        }

        std::sort(questGivers.begin(), questGivers.end(),
                  [](BotSnapshot::NearbyEntity const &a, BotSnapshot::NearbyEntity const &b)
                  {
                      return a.distance < b.distance;
                  });
        if (questGivers.size() > maxTargets)
        {
            questGivers.resize(maxTargets);
        }

        Position3 origin{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        Map *map = bot->GetMap();
        uint32 mapId = bot->GetMapId();
        float maxDistance = g_OllamaBotControlNavMaxDistance > 0.0f ? g_OllamaBotControlNavMaxDistance : 60.0f;

        for (auto const &entity : questGivers)
        {
            float dx = entity.pos.x - origin.x;
            float dy = entity.pos.y - origin.y;
            float dist2d = std::sqrt(dx * dx + dy * dy);
            if (dist2d <= 0.1f)
            {
                continue;
            }

            float step = std::min(dist2d, maxDistance);
            if (dist2d > kQuestGiverApproachOffsetMeters)
            {
                step = std::min(dist2d - kQuestGiverApproachOffsetMeters, maxDistance);
            }
            else
            {
                // Already close enough; avoid suggesting moves that clip into the quest giver.
                continue;
            }
            float dirX = dx / dist2d;
            float dirY = dy / dist2d;
            float x = origin.x + dirX * step;
            float y = origin.y + dirY * step;
            float z = origin.z;

            if (map)
            {
                float height = map->GetHeight(x, y, MAX_HEIGHT);
                float water = map->GetWaterLevel(x, y);
                float candidateZ = std::max(height, water);
                if (candidateZ != INVALID_HEIGHT)
                    z = candidateZ;
            }

            BotSnapshot::NavCandidate candidate;
            std::string label = "quest_giver";
            label += entity.questMarker;
            if (!entity.name.empty())
            {
                label += "_";
                label += NormalizeAreaToken(entity.name);
            }
            candidate.label = std::move(label);
            candidate.pos = Position3{x, y, z};

            WorldPosition wp(mapId, x, y, z);
            candidate.hasLOS = WorldChecks::IsWithinLOS(bot, wp);
            candidate.reachable = WorldChecks::CanReach(bot, wp);
            candidate.distance2d = Distance2d(origin, candidate.pos);
            candidate.bearingDeg = BearingDegrees(origin, candidate.pos);
            candidate.direction = DirectionLabelFromBearing(candidate.bearingDeg);
            candidates.push_back(std::move(candidate));
        }
    }

    float Distance(Position3 const &a, Position3 const &b)
    {
        // 3D Euclidean distance helper.
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float Distance2d(Position3 const &a, Position3 const &b)
    {
        // 2D distance helper for map-based calculations.
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    float BearingDegrees(Position3 const &from, Position3 const &to)
    {
        // Compass bearing in degrees, 0 = east, 90 = north.
        float dx = to.x - from.x;
        float dy = to.y - from.y;
        constexpr float kPi = 3.14159265f;
        float angle = std::atan2(dy, dx) * 180.0f / kPi;
        if (angle < 0.0f)
        {
            angle += 360.0f;
        }
        return angle;
    }

    std::string DirectionLabelFromBearing(float bearingDeg)
    {
        // Map a bearing angle to a coarse cardinal label.
        static constexpr std::array<char const*, 8> kDirections = {
            "east",
            "northeast",
            "north",
            "northwest",
            "west",
            "southwest",
            "south",
            "southeast"};

        float normalized = std::fmod(bearingDeg, 360.0f);
        if (normalized < 0.0f)
        {
            normalized += 360.0f;
        }
        int index = static_cast<int>(std::round(normalized / 45.0f)) % 8;
        return kDirections[static_cast<size_t>(index)];
    }

    std::vector<BotSnapshot::QuestGiverInRange> BuildQuestGiversInRange(Player *bot, PlayerbotAI *ai)
    {
        // Find quest givers that can offer or turn in quests.
        std::vector<BotSnapshot::QuestGiverInRange> results;
        if (!bot || !ai)
        {
            return results;
        }

        AiObjectContext *context = ai->GetAiObjectContext();
        if (!context)
        {
            return results;
        }

        auto addQuestGiver = [&](WorldObject *questGiver,
                                 std::string const &typeLabel,
                                 QuestRelationBounds offeredBounds,
                                 QuestRelationBounds involvedBounds)
        {
            if (!questGiver || !bot->CanInteractWithQuestGiver(questGiver))
            {
                return;
            }

            std::vector<uint32> availableIds;
            for (auto it = offeredBounds.first; it != offeredBounds.second; ++it)
            {
                uint32 questId = it->second;
                if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE)
                {
                    continue;
                }
                Quest const *quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest)
                {
                    continue;
                }
                if (!bot->CanTakeQuest(quest, false))
                {
                    continue;
                }
                availableIds.push_back(questId);
            }

            std::vector<uint32> turnInIds;
            for (auto it = involvedBounds.first; it != involvedBounds.second; ++it)
            {
                uint32 questId = it->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                {
                    turnInIds.push_back(questId);
                }
            }

            if (availableIds.empty() && turnInIds.empty())
            {
                return;
            }

            BotSnapshot::QuestGiverInRange candidate;
            candidate.type = typeLabel;
            candidate.distance = bot->GetDistance(questGiver);
            candidate.pos = Position3{questGiver->GetPositionX(), questGiver->GetPositionY(), questGiver->GetPositionZ()};
            candidate.availableQuestIds = std::move(availableIds);
            candidate.turnInQuestIds = std::move(turnInIds);
            // Compute relevance relative to current quest log (helps prevent planner picking unrelated NPCs).
            for (uint32 questId : candidate.availableQuestIds)
            {
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                {
                    candidate.availableNewQuestIds.push_back(questId);
                }
            }
            for (uint32 questId : candidate.turnInQuestIds)
            {
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                {
                    candidate.turnInActiveQuestIds.push_back(questId);
                }
            }
            if (!candidate.turnInQuestIds.empty())
            {
                candidate.questMarker = "?";
            }
            else if (!candidate.availableQuestIds.empty())
            {
                candidate.questMarker = "!";
            }

            if (Creature *creature = questGiver->ToCreature())
            {
                candidate.entryId = creature->GetEntry();
                candidate.name = creature->GetName();
            }
            else if (GameObject *gameObject = questGiver->ToGameObject())
            {
                candidate.entryId = gameObject->GetEntry();
                candidate.name = gameObject->GetName();
            }

            results.push_back(std::move(candidate));
        };

        GuidVector npcs = context->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const &guid : npcs)
        {
            Creature *creature = ai->GetCreature(guid);
            if (!creature || !creature->IsQuestGiver())
            {
                continue;
            }

            QuestRelationBounds offered = sObjectMgr->GetCreatureQuestRelationBounds(creature->GetEntry());
            QuestRelationBounds involved = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(creature->GetEntry());
            addQuestGiver(creature, "npc", offered, involved);
        }

        GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
        for (ObjectGuid const &guid : gos)
        {
            GameObject *gameObject = ai->GetGameObject(guid);
            if (!gameObject || gameObject->GetGoType() != GAMEOBJECT_TYPE_QUESTGIVER)
            {
                continue;
            }

            QuestRelationBounds offered = sObjectMgr->GetGOQuestRelationBounds(gameObject->GetEntry());
            QuestRelationBounds involved = sObjectMgr->GetGOQuestInvolvedRelationBounds(gameObject->GetEntry());
            addQuestGiver(gameObject, "game_object", offered, involved);
        }

        return results;
    }

    std::vector<BotSnapshot::NearbyEntity> BuildNearbyEntities(Player *bot, PlayerbotAI *ai)
    {
        // Collect nearby NPCs and game objects for context.
        std::vector<BotSnapshot::NearbyEntity> results;
        if (!bot || !ai)
        {
            return results;
        }

        AiObjectContext *context = ai->GetAiObjectContext();
        if (!context)
        {
            return results;
        }

        GuidVector npcs = context->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const &guid : npcs)
        {
            Creature *creature = ai->GetCreature(guid);
            if (!creature)
            {
                continue;
            }

            BotSnapshot::NearbyEntity entity;
            entity.type = "npc";
            entity.entryId = creature->GetEntry();
            entity.name = creature->GetName();
            entity.pos = Position3{creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()};
            entity.distance = bot->GetDistance(creature);
            entity.isQuestGiver = creature->IsQuestGiver();
            entity.isVendor = creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR);
            entity.isTrainer = creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER);
            entity.isRepair = creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR);
            entity.isFlightMaster = creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER);
            if (entity.isQuestGiver)
            {
                QuestRelationBounds startBounds = sObjectMgr->GetCreatureQuestRelationBounds(creature->GetEntry());
                QuestRelationBounds endBounds = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(creature->GetEntry());
                bool hasTurnIn = false;
                bool hasAvailable = false;
                for (auto it = endBounds.first; it != endBounds.second; ++it)
                {
                    uint32 questId = it->second;
                    if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                    {
                        entity.turnInQuestIds.push_back(questId);
                        hasTurnIn = true;
                    }
                }
                if (!hasTurnIn)
                {
                    for (auto it = startBounds.first; it != startBounds.second; ++it)
                    {
                        uint32 questId = it->second;
                        if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                        {
                            hasAvailable = true;
                            break;
                        }
                    }
                }
                if (hasTurnIn)
                {
                    entity.questMarker = "?";
                }
                else if (hasAvailable)
                {
                    entity.questMarker = "!";
                }
            }
            results.push_back(std::move(entity));
        }

        GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
        for (ObjectGuid const &guid : gos)
        {
            GameObject *gameObject = ai->GetGameObject(guid);
            if (!gameObject)
            {
                continue;
            }

            BotSnapshot::NearbyEntity entity;
            entity.type = "game_object";
            entity.entryId = gameObject->GetEntry();
            entity.name = gameObject->GetName();
            entity.pos = Position3{gameObject->GetPositionX(), gameObject->GetPositionY(), gameObject->GetPositionZ()};
            entity.distance = bot->GetDistance(gameObject);
            entity.isQuestGiver = false;
            results.push_back(std::move(entity));
        }

        return results;
    }

    std::vector<BotSnapshot::QuestPoi> BuildQuestPois(Player *bot)
    {
        // Build quest POIs for active objectives in the current map.
        std::vector<BotSnapshot::QuestPoi> results;
        if (!bot)
        {
            return results;
        }

        Map *map = bot->GetMap();
        for (auto const &entry : bot->getQuestStatusMap())
        {
            uint32 questId = entry.first;
            QuestStatusData const &statusData = entry.second;
            if (statusData.Status != QUEST_STATUS_INCOMPLETE && statusData.Status != QUEST_STATUS_COMPLETE)
            {
                continue;
            }

            Quest const *quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
            {
                continue;
            }

            if (statusData.Status == QUEST_STATUS_COMPLETE)
            {
                WorldPosition origin(bot);
                WorldPosition* closest = nullptr;
                for (auto* destination : TravelMgr::instance().getQuestTravelDestinations(
                         bot, questId, true, false, 0, true))
                {
                    if (!destination)
                        continue;
                    for (auto* point : destination->nextPoint(&origin, true))
                    {
                        if (point && point->GetMapId() == bot->GetMapId() &&
                            (!closest || origin.distance(*point) < origin.distance(*closest)))
                            closest = point;
                    }
                }
                if (closest)
                {
                    BotSnapshot::QuestPoi route;
                    route.questId = questId;
                    route.objectiveIndex = -1;
                    route.mapId = bot->GetMapId();
                    route.pos = {closest->GetPositionX(), closest->GetPositionY(), closest->GetPositionZ()};
                    route.hasZ = true;
                    route.isTurnIn = true;
                    results.push_back(std::move(route));
                    continue;
                }
            }

            QuestPOIVector const *poiVector = sObjectMgr->GetQuestPOIVector(questId);
            if (!poiVector)
            {
                continue;
            }

            std::vector<int32> incompleteObjectiveIdx;
            if (statusData.Status == QUEST_STATUS_INCOMPLETE)
            {
                for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                {
                    if (quest->RequiredNpcOrGoCount[i] > 0 && statusData.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                    {
                        incompleteObjectiveIdx.push_back(i);
                    }
                }
                for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                {
                    if (quest->RequiredItemCount[i] > 0 && statusData.ItemCount[i] < quest->RequiredItemCount[i])
                    {
                        incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
                    }
                }
            }

            for (QuestPOI const &poi : *poiVector)
            {
                if (poi.MapId != bot->GetMapId())
                {
                    continue;
                }
                if (poi.points.empty())
                {
                    continue;
                }

                bool includePoi = false;
                bool isTurnIn = false;
                if (statusData.Status == QUEST_STATUS_COMPLETE)
                {
                    if (poi.ObjectiveIndex == -1)
                    {
                        includePoi = true;
                        isTurnIn = true;
                    }
                }
                else if (statusData.Status == QUEST_STATUS_INCOMPLETE)
                {
                    for (int32 objectiveIndex : incompleteObjectiveIdx)
                    {
                        if (poi.ObjectiveIndex == objectiveIndex)
                        {
                            includePoi = true;
                            break;
                        }
                    }
                }

                if (!includePoi)
                {
                    continue;
                }

                float sumX = 0.0f;
                float sumY = 0.0f;
                for (QuestPOIPoint const &point : poi.points)
                {
                    sumX += static_cast<float>(point.x);
                    sumY += static_cast<float>(point.y);
                }
                float avgX = sumX / static_cast<float>(poi.points.size());
                float avgY = sumY / static_cast<float>(poi.points.size());

                BotSnapshot::QuestPoi entryPoi;
                entryPoi.questId = questId;
                entryPoi.objectiveIndex = poi.ObjectiveIndex;
                entryPoi.mapId = poi.MapId;
                entryPoi.areaId = poi.AreaId;
                entryPoi.pos = Position3{avgX, avgY, bot->GetPositionZ()};
                entryPoi.isTurnIn = isTurnIn;
                entryPoi.hasZ = false;

                if (map)
                {
                    float height = map->GetHeight(avgX, avgY, MAX_HEIGHT);
                    float water = map->GetWaterLevel(avgX, avgY);
                    float z = std::max(height, water);
                    if (z != INVALID_HEIGHT)
                    {
                        entryPoi.pos.z = z;
                        entryPoi.hasZ = true;
                    }
                }

                results.push_back(std::move(entryPoi));
            }
        }

        return results;
    }

    float ExpectedAvgItemLevelForLevel(uint8 level)
    {
        // Heuristic baseline for "appropriate gear" by level. This is not a true GearScore implementation,
        // but it provides a consistent low/medium/high signal across leveling bands.
        if (level == 0)
        {
            return 0.0f;
        }
        if (level <= 60)
        {
            return static_cast<float>(level) + 5.0f;
        }
        if (level <= 70)
        {
            // 60->70: roughly 65 -> 115
            return 65.0f + (static_cast<float>(level) - 60.0f) * 5.0f;
        }
        // 70->80: roughly 115 -> 187
        return 115.0f + (static_cast<float>(level) - 70.0f) * 7.2f;
    }

    char const* SlotName(uint8 slot)
    {
        switch (slot)
        {
        case EQUIPMENT_SLOT_HEAD:
            return "head";
        case EQUIPMENT_SLOT_NECK:
            return "neck";
        case EQUIPMENT_SLOT_SHOULDERS:
            return "shoulder";
        case EQUIPMENT_SLOT_CHEST:
            return "chest";
        case EQUIPMENT_SLOT_WAIST:
            return "waist";
        case EQUIPMENT_SLOT_LEGS:
            return "legs";
        case EQUIPMENT_SLOT_FEET:
            return "feet";
        case EQUIPMENT_SLOT_WRISTS:
            return "wrist";
        case EQUIPMENT_SLOT_HANDS:
            return "hands";
        case EQUIPMENT_SLOT_FINGER1:
            return "ring1";
        case EQUIPMENT_SLOT_FINGER2:
            return "ring2";
        case EQUIPMENT_SLOT_TRINKET1:
            return "trinket1";
        case EQUIPMENT_SLOT_TRINKET2:
            return "trinket2";
        case EQUIPMENT_SLOT_BACK:
            return "cloak";
        case EQUIPMENT_SLOT_MAINHAND:
            return "mainhand";
        default:
            return "unknown";
        }
    }

    BotSnapshot BuildBotSnapshot(Player *bot, PlayerbotAI *ai)
    {
        // Gather bot state needed for planning and control.
        BotSnapshot snapshot;
        snapshot.botGuid = bot->GetGUID().GetRawValue();
        snapshot.botName = bot->GetName();
        snapshot.pos = Position3{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        snapshot.orientation = bot->GetOrientation();
        snapshot.mapId = bot->GetMapId();
        snapshot.zoneId = bot->GetZoneId();
        snapshot.areaId = bot->GetAreaId();
        snapshot.inCombat = bot->IsInCombat();
        snapshot.isMoving = bot->isMoving();
        snapshot.level = bot->GetLevel();
        BotMissionState missionState = BotMissionRegistry::Instance().Get(bot->GetGUID().GetRawValue());
        snapshot.mission = missionState.mission;
        snapshot.missionRevision = missionState.revision;
        snapshot.lifecycleGeneration = GetBotLifecycleGeneration(bot);
        snapshot.topNeed = AssessBotNeeds(bot);
        snapshot.lifecycle = AssessBotLifecycle(bot, ai, snapshot.topNeed);
        snapshot.lifecycleActive = HasActiveBotLifecycleOverlay(bot) || HasPendingBotLifecycleService(bot);
        {
            std::ostringstream missionText;
            missionText << snapshot.mission.KindName();
            if (!snapshot.mission.target.empty()) missionText << " " << snapshot.mission.target;
            if (!snapshot.mission.role.empty()) missionText << " (" << snapshot.mission.role << ")";
            std::string activityText = snapshot.lifecycle.LaneName();
            if (snapshot.lifecycle.lane == BotLifecycleLane::Maintenance && snapshot.lifecycle.maintenance != BotMaintenanceKind::None)
                activityText += std::string(":") + snapshot.lifecycle.MaintenanceName();
            AmigoMindUpdateGameplay(snapshot.botGuid, missionText.str(), activityText);
        }
        snapshot.navCandidates = BuildNavCandidates(bot);
        snapshot.questGiversInRange = BuildQuestGiversInRange(bot, ai);
        snapshot.nearbyEntities = BuildNearbyEntities(bot, ai);
        AppendQuestGiverNavCandidates(bot, snapshot.nearbyEntities, snapshot.navCandidates);
        snapshot.questPois = BuildQuestPois(bot);
        for (auto const& poi : snapshot.questPois)
        {
            if (!poi.isTurnIn || !poi.hasZ)
                continue;
            WorldPosition destination(poi.mapId, poi.pos.x, poi.pos.y, poi.pos.z);
            if (!WorldChecks::CanReach(bot, destination))
                continue;
            BotSnapshot::NavCandidate candidate;
            candidate.label = "quest_turn_in:" + std::to_string(poi.questId);
            candidate.pos = poi.pos;
            candidate.reachable = true;
            candidate.hasLOS = WorldChecks::IsWithinLOS(bot, destination);
            candidate.distance2d = Distance2d(snapshot.pos, poi.pos);
            candidate.bearingDeg = BearingDegrees(snapshot.pos, poi.pos);
            candidate.direction = DirectionLabelFromBearing(candidate.bearingDeg);
            snapshot.navCandidates.push_back(std::move(candidate));
        }

        // Gear / equipment signal (planner + control context).
        snapshot.avgItemLevel = bot->GetAverageItemLevel();
        snapshot.expectedAvgItemLevel = ExpectedAvgItemLevelForLevel(static_cast<uint8>(bot->GetLevel()));
        float expected = snapshot.expectedAvgItemLevel;
        if (expected > 0.0f)
        {
            float lowCut = expected * 0.85f;
            float highCut = expected * 1.15f;
            if (snapshot.avgItemLevel < lowCut)
                snapshot.gearBand = "low";
            else if (snapshot.avgItemLevel > highCut)
                snapshot.gearBand = "high";
            else
                snapshot.gearBand = "medium";
        }
        else
        {
            snapshot.gearBand = "unknown";
        }

        // Identify weak slots relative to expected average. Keep the list small.
        constexpr size_t kMaxLowSlots = 4;
        uint8 level = bot->GetLevel();
        float slotLowCut = expected > 0.0f ? std::max(0.0f, expected - 12.0f) : 0.0f;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_TABARD || slot == EQUIPMENT_SLOT_BODY)
                continue;
            if (slot == EQUIPMENT_SLOT_RANGED || slot == EQUIPMENT_SLOT_OFFHAND)
                continue;

            Item *item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            uint32 ilvl = 0;
            std::string name = "none";
            if (item && item->GetTemplate())
            {
                ilvl = item->GetTemplate()->GetItemLevelIncludingQuality(level);
                name = item->GetTemplate()->Name1;
            }
            bool isLow = (name == "none") || (expected > 0.0f && static_cast<float>(ilvl) < slotLowCut);
            if (isLow && snapshot.lowGearSlots.size() < kMaxLowSlots)
            {
                snapshot.lowGearSlots.push_back(BotSnapshot::GearSlot{SlotName(slot), name, ilvl});
            }
        }

        if (bot->GetMaxHealth() > 0)
        {
            snapshot.hpPct = (static_cast<float>(bot->GetHealth()) / bot->GetMaxHealth()) * 100.0f;
        }

        uint32 maxMana = bot->GetMaxPower(POWER_MANA);
        if (maxMana > 0)
        {
            snapshot.manaPct = (static_cast<float>(bot->GetPower(POWER_MANA)) / maxMana) * 100.0f;
        }

        auto weaponSubClassLabel = [](uint8 subClass) -> char const*
        {
            switch (subClass)
            {
            case ITEM_SUBCLASS_WEAPON_AXE:
                return "axe";
            case ITEM_SUBCLASS_WEAPON_AXE2:
                return "axe_2h";
            case ITEM_SUBCLASS_WEAPON_BOW:
                return "bow";
            case ITEM_SUBCLASS_WEAPON_GUN:
                return "gun";
            case ITEM_SUBCLASS_WEAPON_MACE:
                return "mace";
            case ITEM_SUBCLASS_WEAPON_MACE2:
                return "mace_2h";
            case ITEM_SUBCLASS_WEAPON_POLEARM:
                return "polearm";
            case ITEM_SUBCLASS_WEAPON_SWORD:
                return "sword";
            case ITEM_SUBCLASS_WEAPON_SWORD2:
                return "sword_2h";
            case ITEM_SUBCLASS_WEAPON_STAFF:
                return "staff";
            case ITEM_SUBCLASS_WEAPON_FIST:
                return "fist";
            case ITEM_SUBCLASS_WEAPON_DAGGER:
                return "dagger";
            case ITEM_SUBCLASS_WEAPON_THROWN:
                return "thrown";
            case ITEM_SUBCLASS_WEAPON_SPEAR:
                return "spear";
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return "crossbow";
            case ITEM_SUBCLASS_WEAPON_WAND:
                return "wand";
            case ITEM_SUBCLASS_WEAPON_FISHING_POLE:
                return "fishing_pole";
            case ITEM_SUBCLASS_WEAPON_EXOTIC:
            case ITEM_SUBCLASS_WEAPON_EXOTIC2:
                return "exotic";
            case ITEM_SUBCLASS_WEAPON_MISC:
                return "misc";
            default:
                return nullptr;
            }
        };

        auto addWeaponTypeIfWeapon = [&](uint8 slot)
        {
            Item *item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
            {
                return;
            }

            ItemTemplate const *proto = item->GetTemplate();
            if (!proto || !proto->IsWeapon())
            {
                return;
            }

            char const* label = weaponSubClassLabel(proto->SubClass);
            if (!label || label[0] == '\0')
            {
                return;
            }

            snapshot.weaponTypes.emplace_back(label);
        };

        addWeaponTypeIfWeapon(EQUIPMENT_SLOT_MAINHAND);
        addWeaponTypeIfWeapon(EQUIPMENT_SLOT_OFFHAND);
        addWeaponTypeIfWeapon(EQUIPMENT_SLOT_RANGED);

        if (!snapshot.weaponTypes.empty())
        {
            std::sort(snapshot.weaponTypes.begin(), snapshot.weaponTypes.end());
            snapshot.weaponTypes.erase(std::unique(snapshot.weaponTypes.begin(), snapshot.weaponTypes.end()),
                                       snapshot.weaponTypes.end());
            snapshot.hasWeapon = true;
        }
        else
        {
            snapshot.hasWeapon = false;
        }

        auto addSkill = [&](uint32 skillId, char const* label)
        {
            if (!label || label[0] == '\0')
            {
                return;
            }

            uint32 value = bot->GetSkillValue(skillId);
            if (value == 0)
            {
                return;
            }

            uint32 maxValue = bot->GetMaxSkillValue(skillId);
            if (maxValue == 0)
            {
                maxValue = value;
            }

            std::ostringstream skill;
            skill << label << " " << value << "/" << maxValue;
            snapshot.professions.emplace_back(skill.str());
        };

        addSkill(SKILL_ALCHEMY, "alchemy");
        addSkill(SKILL_BLACKSMITHING, "blacksmithing");
        addSkill(SKILL_ENCHANTING, "enchanting");
        addSkill(SKILL_ENGINEERING, "engineering");
        addSkill(SKILL_HERBALISM, "herbalism");
        addSkill(SKILL_INSCRIPTION, "inscription");
        addSkill(SKILL_JEWELCRAFTING, "jewelcrafting");
        addSkill(SKILL_LEATHERWORKING, "leatherworking");
        addSkill(SKILL_MINING, "mining");
        addSkill(SKILL_SKINNING, "skinning");
        addSkill(SKILL_TAILORING, "tailoring");
        addSkill(SKILL_COOKING, "cooking");
        addSkill(SKILL_FIRST_AID, "first aid");
        addSkill(SKILL_FISHING, "fishing");

        if (!snapshot.professions.empty())
        {
            std::sort(snapshot.professions.begin(), snapshot.professions.end());
        }

        std::string currentActivity;
        std::string activityReason;
        if (TryGetActivityState(bot, currentActivity, activityReason))
        {
            snapshot.grindMode = (NormalizeCommandToken(currentActivity) == "grind");
        }

        const bool canMove = !snapshot.inCombat && !snapshot.grindMode && !snapshot.isMoving;
        for (auto &candidate : snapshot.navCandidates)
        {
            // canMove is the high-level gate (combat/grind/moving) AND physical reachability.
            candidate.canMove = canMove && candidate.reachable;
        }

        for (auto const &entry : bot->getQuestStatusMap())
        {
            QuestStatus status = entry.second.Status;
            if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
            {
                snapshot.activeQuestIds.push_back(entry.first);
                BotSnapshot::QuestProgress progress;
                progress.questId = entry.first;
                progress.status = status;
                progress.explored = entry.second.Explored;

                if (Quest const *quest = sObjectMgr->GetQuestTemplate(entry.first))
                {
                    progress.title = quest->GetTitle();
                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    {
                        if (quest->RequiredItemCount[i] > 0)
                        {
                            std::string itemName;
                            if (ItemTemplate const *item = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]))
                            {
                                itemName = item->Name1;
                            }
                            progress.objectives.push_back(BotSnapshot::QuestObjectiveProgress{
                                "item",
                                static_cast<int32>(quest->RequiredItemId[i]),
                                itemName,
                                entry.second.ItemCount[i],
                                quest->RequiredItemCount[i]});
                        }
                    }

                    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    {
                        if (quest->RequiredNpcOrGoCount[i] > 0)
                        {
                            std::string targetName;
                            int32 target = quest->RequiredNpcOrGo[i];
                            if (target > 0)
                            {
                                if (CreatureTemplate const *creature = sObjectMgr->GetCreatureTemplate(static_cast<uint32>(target)))
                                {
                                    targetName = creature->Name;
                                }
                            }
                            else if (target < 0)
                            {
                                uint32 entryId = static_cast<uint32>(-target);
                                if (GameObjectTemplate const *go = sObjectMgr->GetGameObjectTemplate(entryId))
                                {
                                    targetName = go->name;
                                }
                            }
                            progress.objectives.push_back(BotSnapshot::QuestObjectiveProgress{
                                target > 0 ? "creature" : "game_object",
                                quest->RequiredNpcOrGo[i],
                                targetName,
                                entry.second.CreatureOrGOCount[i],
                                quest->RequiredNpcOrGoCount[i]});
                        }
                    }

                    if (quest->GetPlayersSlain() > 0)
                    {
                        progress.objectives.push_back(BotSnapshot::QuestObjectiveProgress{
                            "player",
                            0,
                            std::string("players"),
                            entry.second.PlayerCount,
                            quest->GetPlayersSlain()});
                    }
                }

                snapshot.activeQuests.push_back(std::move(progress));
            }
        }

        // Build one structured set of legal choices for every mission type.
        // The formatted list remains a prompt compatibility view only.
        auto addDecisionOption = [&](ControlDecisionOption option, std::string formatted)
        {
            snapshot.decisionOptions.push_back(std::move(option));
            snapshot.postGrindOptions.push_back(std::move(formatted));
        };

        bool completedQuestNeedsTurnIn = false;
        for (auto const &quest : snapshot.activeQuests)
        {
            if (quest.status != QUEST_STATUS_COMPLETE)
                continue;

            completedQuestNeedsTurnIn = true;
            if (snapshot.grindMode)
            {
                ControlDecisionOption option;
                option.action = "request_stop_grind";
                option.questId = quest.questId;
                option.destinationType = "quest_turn_in";
                option.reason = "stop grind before quest turn-in";
                addDecisionOption(std::move(option), "stop_grind_for_turn_in");
                continue;
            }
            for (auto const &giver : snapshot.questGiversInRange)
            {
                if (std::find(giver.turnInQuestIds.begin(), giver.turnInQuestIds.end(), quest.questId) == giver.turnInQuestIds.end())
                    continue;

                ControlDecisionOption option;
                option.action = "request_talk_to_quest_giver";
                option.questId = quest.questId;
                option.targetName = giver.name;
                option.reason = "completed quest is ready to turn in";
                option.distance = giver.distance;
                option.local = true;
                option.destinationType = "quest_giver";
                option.destinationEntry = giver.entryId;
                addDecisionOption(std::move(option), "turn_in_quest:" + std::to_string(quest.questId) +
                                  " | " + giver.name + " | " + DistanceText(giver.distance));
            }

            for (auto const &entity : snapshot.nearbyEntities)
            {
                if (entity.type != "npc" ||
                    std::find(entity.turnInQuestIds.begin(), entity.turnInQuestIds.end(), quest.questId) == entity.turnInQuestIds.end())
                    continue;

                ControlDecisionOption option;
                option.action = "request_move_hop_npc";
                option.questId = quest.questId;
                option.entryId = entity.entryId;
                option.targetName = entity.name;
                option.reason = "move to the server-identified quest turn-in NPC";
                option.distance = entity.distance;
                option.local = false;
                option.destinationType = "quest_giver";
                option.destinationEntry = entity.entryId;
                addDecisionOption(std::move(option), "move_to_turn_in:" + std::to_string(entity.entryId) +
                                  " | " + entity.name + " | " + DistanceText(entity.distance));
            }

            for (auto const &poi : snapshot.questPois)
            {
                if (poi.questId != quest.questId || !poi.isTurnIn || poi.mapId != snapshot.mapId)
                    continue;

                size_t bestIndex = snapshot.navCandidates.size();
                float bestDistance = Distance2d(snapshot.pos, poi.pos);
                for (size_t index = 0; index < snapshot.navCandidates.size(); ++index)
                {
                    auto const &candidate = snapshot.navCandidates[index];
                    if (!candidate.canMove || !candidate.reachable ||
                        Distance2d(candidate.pos, poi.pos) >= bestDistance)
                        continue;
                    bestIndex = index;
                    bestDistance = Distance2d(candidate.pos, poi.pos);
                }
                if (bestIndex != snapshot.navCandidates.size())
                {
                    auto const& candidate = snapshot.navCandidates[bestIndex];
                    ControlDecisionOption option;
                    option.action = "request_move_hop";
                    option.questId = quest.questId;
                    option.reason = "move toward the server-generated quest turn-in POI";
                    option.distance = candidate.distance2d;
                    option.local = false;
                    option.destinationType = "quest_turn_in_poi";
                    option.navigationDirection = candidate.direction;
                    option.candidateId = "nav_" + std::to_string(bestIndex);
                    addDecisionOption(std::move(option), "move_to_turn_in_poi | " + candidate.direction +
                                      " | " + DistanceText(candidate.distance2d));
                }
            }
        }

        if (!completedQuestNeedsTurnIn)
        {
            for (auto const& entity : snapshot.nearbyEntities)
            {
                bool gather = snapshot.mission.kind == BotMissionKind::Gather && entity.type == "game_object";
                bool grind = snapshot.mission.kind == BotMissionKind::Grind && entity.type == "npc" &&
                             snapshot.activeQuests.empty();
                if ((!gather && !grind) || !snapshot.mission.MatchesTargetName(entity.name))
                    continue;
                ControlDecisionOption option;
                option.action = gather ? "request_gather_target" : "request_attack_target";
                option.entryId = entity.entryId;
                option.targetName = entity.name;
                option.distance = entity.distance;
                option.reason = "exact configured mission target";
                option.objectiveType = gather ? "game_object" : "creature";
                addDecisionOption(std::move(option), "exact_mission_target | " + entity.name);
            }
            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.status != QUEST_STATUS_INCOMPLETE)
                    continue;
                for (auto const &objective : quest.objectives)
                {
                    if (objective.current >= objective.required || objective.targetName.empty())
                        continue;

                    for (auto const &entity : snapshot.nearbyEntities)
                    {
                        if (entity.type != "npc" || entity.name != objective.targetName || objective.type != "creature")
                            continue;
                        if (snapshot.mission.kind != BotMissionKind::Quest &&
                            (snapshot.mission.kind != BotMissionKind::Grind ||
                             !snapshot.mission.MatchesTargetName(entity.name)))
                            continue;

                        ControlDecisionOption option;
                        option.action = "request_attack_target";
                        option.questId = quest.questId;
                        option.entryId = entity.entryId;
                        option.targetName = objective.targetName;
                        option.reason = "exact incomplete creature objective";
                        option.distance = entity.distance;
                        option.objectiveType = objective.type;
                        option.objectiveTargetId = objective.targetId;
                        addDecisionOption(std::move(option), "attack_objective:" + objective.targetName +
                                          " | " + DistanceText(entity.distance));
                        break;
                    }
                }
            }

            for (auto const &giver : snapshot.questGiversInRange)
            {
                for (uint32 questId : giver.availableNewQuestIds)
                {
                    ControlDecisionOption option;
                    option.action = "request_talk_to_quest_giver";
                    option.questId = questId;
                    option.targetName = giver.name;
                    option.reason = "available quest from a nearby quest giver";
                    option.distance = giver.distance;
                    option.local = true;
                    addDecisionOption(std::move(option), "accept_quest:" + std::to_string(questId));
                }
            }

            for (auto const& definition : GetControlActionCatalog())
            {
                if (!definition.lifecycleOwned)
                    continue;
                BotMaintenanceKind kind = BotMaintenanceKind::None;
                switch (definition.capability)
                {
                    case ControlAction::Capability::VendorSell: kind = BotMaintenanceKind::BagSpace; break;
                    case ControlAction::Capability::VendorBuyUseful: kind = BotMaintenanceKind::Supplies; break;
                    case ControlAction::Capability::Repair: kind = BotMaintenanceKind::Repair; break;
                    case ControlAction::Capability::Trainer: kind = BotMaintenanceKind::Training; break;
                    default: break;
                }
                auto availability = AssessBotServiceAvailability(bot, ai, kind);
                snapshot.serviceAvailability[definition.name] = availability;
                if (!availability.local && !availability.travel)
                    continue;
                ControlDecisionOption option;
                option.action = definition.name;
                option.reason = availability.reason;
                option.local = availability.local;
                option.lifecycleTravel = true;
                addDecisionOption(std::move(option), std::string(definition.name) + " | " + availability.reason);
            }

            if (snapshot.mission.kind == BotMissionKind::Grind && snapshot.mission.target.empty() &&
                snapshot.activeQuests.empty() && snapshot.decisionOptions.empty())
            {
                ControlDecisionOption option;
                option.action = "request_enter_grind";
                option.reason = "continue the configured grind mission";
                addDecisionOption(std::move(option), "continue_grind");
            }
        }

        snapshot.postGrindDecision = !snapshot.decisionOptions.empty() || completedQuestNeedsTurnIn;
        snapshot.actionableGoal = snapshot.mission.PlannerGoal();
        for (auto const& quest : snapshot.activeQuests)
        {
            if (quest.status == QUEST_STATUS_COMPLETE)
            {
                snapshot.actionableGoal = "Turn in the completed quest " + quest.title + ".";
                break;
            }
        }
        if (!completedQuestNeedsTurnIn && snapshot.lifecycleActive)
            snapshot.actionableGoal = "Complete the active " + snapshot.lifecycle.MaintenanceName() + " service.";
        std::sort(snapshot.postGrindOptions.begin(), snapshot.postGrindOptions.end());
        snapshot.postGrindOptions.erase(std::unique(snapshot.postGrindOptions.begin(), snapshot.postGrindOptions.end()),
                                        snapshot.postGrindOptions.end());

        return snapshot;
    }

    WorldSnapshot BuildWorldSnapshot(Player *bot)
    {
        // Resolve zone and area names for readability.
        WorldSnapshot snapshot;
        uint32 zoneId = bot->GetZoneId();
        uint32 areaId = bot->GetAreaId();

        if (AreaTableEntry const *zoneEntry = sAreaTableStore.LookupEntry(zoneId))
        {
            if (zoneEntry->area_name[LOCALE_enUS] && zoneEntry->area_name[LOCALE_enUS][0] != '\0')
            {
                snapshot.zone = zoneEntry->area_name[LOCALE_enUS];
            }
        }

        if (AreaTableEntry const *areaEntry = sAreaTableStore.LookupEntry(areaId))
        {
            if (areaEntry->area_name[LOCALE_enUS] && areaEntry->area_name[LOCALE_enUS][0] != '\0')
            {
                snapshot.area = areaEntry->area_name[LOCALE_enUS];
            }
        }

        if (snapshot.zone.empty())
        {
            snapshot.zone = "unknown";
        }
        if (snapshot.area.empty())
        {
            snapshot.area = "unknown";
        }

        return snapshot;
    }

    struct LocalAreaProfile
    {
        // Heuristic profile for area risk and pacing.
        std::string areaRole;
        uint32 levelBandMin = 1;
        uint32 levelBandMax = 1;
        std::string mobDensity;
        std::string respawnRate;
        std::string competitionLevel;
        std::string deathRisk;
        std::string corpseRunSeverity;
        std::string pullComplexity;
        std::string navigationComplexity;
        std::string obstacleFrequency;
        std::string roamingRequired;
    };

    LocalAreaProfile DeriveLocalAreaProfile(uint32 level)
    {
        // Rough defaults that scale with player level.
        LocalAreaProfile profile;
        if (level <= 5)
        {
            profile.areaRole = "starter_zone";
            profile.levelBandMin = 1;
            profile.levelBandMax = 5;
            profile.mobDensity = "high";
            profile.respawnRate = "fast";
            profile.competitionLevel = "low";
            profile.deathRisk = "low";
            profile.corpseRunSeverity = "minimal";
            profile.pullComplexity = "simple";
            profile.navigationComplexity = "simple";
            profile.obstacleFrequency = "low";
            profile.roamingRequired = "minimal";
            return profile;
        }

        if (level <= 10)
        {
            profile.areaRole = "low_level_zone";
            profile.levelBandMin = 6;
            profile.levelBandMax = 10;
            profile.mobDensity = "medium";
            profile.respawnRate = "normal";
            profile.competitionLevel = "low";
            profile.deathRisk = "low";
            profile.corpseRunSeverity = "low";
            profile.pullComplexity = "simple";
            profile.navigationComplexity = "simple";
            profile.obstacleFrequency = "low";
            profile.roamingRequired = "low";
            return profile;
        }

        if (level <= 20)
        {
            profile.areaRole = "adventuring_zone";
            profile.levelBandMin = 11;
            profile.levelBandMax = 20;
            profile.mobDensity = "medium";
            profile.respawnRate = "normal";
            profile.competitionLevel = "medium";
            profile.deathRisk = "medium";
            profile.corpseRunSeverity = "low";
            profile.pullComplexity = "moderate";
            profile.navigationComplexity = "moderate";
            profile.obstacleFrequency = "medium";
            profile.roamingRequired = "moderate";
            return profile;
        }

        profile.areaRole = "adventuring_zone";
        profile.levelBandMin = std::max<uint32>(1, level > 5 ? level - 5 : 1);
        profile.levelBandMax = level + 5;
        profile.mobDensity = "medium";
        profile.respawnRate = "normal";
        profile.competitionLevel = "medium";
        profile.deathRisk = "medium";
        profile.corpseRunSeverity = "moderate";
        profile.pullComplexity = "moderate";
        profile.navigationComplexity = "moderate";
        profile.obstacleFrequency = "medium";
        profile.roamingRequired = "moderate";
        return profile;
    }

    nlohmann::json BuildWorldModelJson()
    {
        // Static world model assumptions for LLM context.
        return {
            {"continent", "eastern_kingdoms"},
            {"faction_control", "alliance"},
            {"expansion_tier", "vanilla"},
            {"danger_profile", {{"baseline_threat", "low"}, {"elite_density", "none"}, {"pvp_risk", "none"}}},
            {"mob_ecology", {{"dominant_creature_types", {"humanoid", "beast"}}, {"average_mob_level_delta", 0}, {"mob_social_behavior", "loose_groups"}}},
            {"travel_characteristics", {{"terrain_openness", "open"}, {"line_of_sight", "long"}, {"verticality", "low"}}}};
    }

    nlohmann::json BuildSnapshotJson(BotSnapshot const &bot, WorldSnapshot const &world, Goal const *goal, LlmView view)
    {
        // Serialize the bot/world state for LLM prompts.
        LocalAreaProfile localProfile = DeriveLocalAreaProfile(bot.level);
        std::string normalizedZone = NormalizeAreaToken(world.zone);
        std::string normalizedArea = NormalizeAreaToken(world.area);
        std::string densityBand = localProfile.mobDensity;
        constexpr float kPi = 3.14159265f;
        float facingDeg = bot.orientation * 180.0f / kPi;
        if (facingDeg < 0.0f)
        {
            facingDeg += 360.0f;
        }
        std::string facingDirection = DirectionLabelFromBearing(facingDeg);
        nlohmann::json json;
        nlohmann::json questList = nlohmann::json::array();
        for (auto const &quest : bot.activeQuests)
        {
            bool eligibleForWorldActivity = quest.status == QUEST_STATUS_INCOMPLETE;
            bool needsTurnIn = quest.status == QUEST_STATUS_COMPLETE;
            bool isBlocked = quest.status == QUEST_STATUS_FAILED || quest.status == QUEST_STATUS_REWARDED || quest.status == QUEST_STATUS_NONE;
            uint32 totalRequired = 0;
            bool requiresKills = false;
            bool requiresItems = false;
            std::vector<std::string> objectiveTypes;
            nlohmann::json objectives = nlohmann::json::array();
            nlohmann::json questJson = {
                {"id", quest.questId},
                {"title", quest.title},
                {"status", QuestStatusToString(quest.status)},
                {"explored", quest.explored},
                {"eligible_for_world_activity", eligibleForWorldActivity},
                {"needs_turn_in", needsTurnIn}};
            nlohmann::json poiList = nlohmann::json::array();
            for (auto const &poi : bot.questPois)
            {
                if (poi.questId != quest.questId)
                {
                    continue;
                }

                nlohmann::json poiJson = {
                    {"objective_index", poi.objectiveIndex},
                    {"objective_type", poi.isTurnIn ? "turn_in"
                                                    : (poi.objectiveIndex >= QUEST_OBJECTIVES_COUNT ? "item" : "npc_or_go")},
                    {"map_id", poi.mapId},
                    {"area_id", poi.areaId},
                    {"is_turn_in", poi.isTurnIn}};

                if (poi.mapId == bot.mapId)
                {
                    float distance = Distance2d(bot.pos, poi.pos);
                    float bearing = BearingDegrees(bot.pos, poi.pos);
                    poiJson["distance_band"] = DistanceBandLabelForDistance(distance);
                    poiJson["direction"] = DirectionLabelFromBearing(bearing);
                }

                poiList.push_back(std::move(poiJson));
            }
            if (!poiList.empty())
            {
                questJson["poi"] = poiList;
            }
            if (!quest.objectives.empty())
            {
                for (auto const &objective : quest.objectives)
                {
                    totalRequired += objective.required;
                    if (objective.type == "npc_or_go" || objective.type == "player")
                    {
                        requiresKills = true;
                    }
                    if (objective.type == "item")
                    {
                        requiresItems = true;
                    }
                    if (std::find(objectiveTypes.begin(), objectiveTypes.end(), objective.type) == objectiveTypes.end())
                    {
                        objectiveTypes.push_back(objective.type);
                    }
                    objectives.push_back({{"type", objective.type},
                                          {"target_name", objective.targetName},
                                          {"current", objective.current},
                                          {"required", objective.required}});
                }
                questJson["objectives"] = objectives;
            }
            bool multiObjective = quest.objectives.size() > 1;
            bool parallelizable = multiObjective;
            bool satisfiableInCurrentArea = eligibleForWorldActivity && !isBlocked;
            std::string expectedTime = "short";
            if (totalRequired > 12)
            {
                expectedTime = "long";
            }
            else if (totalRequired > 5)
            {
                expectedTime = "medium";
            }
            std::string expectedCombatStyle = requiresKills ? "short_repeated" : "minimal";
            std::string movementStyle = (requiresKills || requiresItems) ? "local_wandering" : "localized";
            std::string overpullRisk = (densityBand == "high" && requiresKills) ? "medium" : "low";
            std::string expectedFriction = localProfile.deathRisk == "low" ? "low" : "medium";
            questJson["affordances"] = {
                {"lifecycle", {{"eligible_for_world_activity", eligibleForWorldActivity}, {"needs_turn_in", needsTurnIn}, {"is_blocked", isBlocked}}},
                {"objective_analysis", {{"objective_types", objectiveTypes}, {"requires_kills", requiresKills}, {"requires_items", requiresItems}, {"multi_objective", multiObjective}, {"parallelizable", parallelizable}}},
                {"world_footprint", {{"known_activity_regions", {{{"zone", normalizedZone}, {"area", normalizedArea}, {"confidence", 0.7}, {"proximity_band", "near"}, {"mob_density_band", densityBand}, {"mob_type_mix", {"humanoid", "beast"}}, {"expected_combat_style", expectedCombatStyle}, {"expected_movement_style", movementStyle}}}}, {"aggregate_proximity", "near"}, {"aggregate_density", densityBand}, {"satisfiable_in_current_area", satisfiableInCurrentArea}}},
                {"activity_expectations", {{"is_grind_friendly", requiresKills}, {"is_travel_heavy", false}, {"is_wait_gated", false}, {"expected_time_to_complete", expectedTime}, {"expected_friction", expectedFriction}}},
                {"risk_profile", {{"threat_level", localProfile.deathRisk}, {"overpull_risk", overpullRisk}, {"death_penalty_severity", localProfile.corpseRunSeverity}}}};
            questList.push_back(questJson);
        }

        if (view == LlmView::Planner)
        {
            nlohmann::json planner;
            planner["bot"] = {
                {"level", bot.level},
                {"in_combat", bot.inCombat},
                {"is_moving", bot.isMoving},
                {"grind_mode", bot.grindMode},
                {"mission", {{"kind", bot.mission.KindName()}, {"target", bot.mission.target}, {"role", bot.mission.role}, {"revision", bot.missionRevision}, {"planner_goal", bot.mission.PlannerGoal()}}},
                {"lifecycle_generation", bot.lifecycleGeneration},
                {"top_need", {{"kind", bot.topNeed.KindName()}, {"urgent", bot.topNeed.urgent}, {"score", bot.topNeed.score}, {"reason", bot.topNeed.reason}}},
                {"lifecycle", {{"lane", bot.lifecycle.LaneName()}, {"maintenance", bot.lifecycle.MaintenanceName()}, {"urgent", bot.lifecycle.urgent}, {"score", bot.lifecycle.score}, {"reason", bot.lifecycle.reason}, {"bag_space_pct", bot.lifecycle.bagSpacePct}, {"durability_pct", bot.lifecycle.durabilityPct}, {"food_count", bot.lifecycle.foodCount}, {"drink_count", bot.lifecycle.drinkCount}, {"ammo_count", bot.lifecycle.ammoCount}}},
                {"active_quest_ids", bot.activeQuestIds},
                {"gear", {{"avg_item_level", std::round(bot.avgItemLevel * 10.0f) / 10.0f},
                          {"expected_avg_item_level", std::round(bot.expectedAvgItemLevel * 10.0f) / 10.0f},
                          {"band", bot.gearBand}}}};
            planner["world_model"] = BuildWorldModelJson();
            if (goal)
                planner["current_goal"] = goal->ToJson();
            return planner;
        }

        json["bot"] = {
            {"orientation_rad", std::round(bot.orientation * 1000.0f) / 1000.0f},
            {"facing_deg", std::round(facingDeg * 10.0f) / 10.0f},
            {"facing_direction", facingDirection},
            {"map_id", bot.mapId},
            {"zone_id", bot.zoneId},
            {"area_id", bot.areaId},
            {"in_combat", bot.inCombat},
            {"is_moving", bot.isMoving},
            {"grind_mode", bot.grindMode},
            {"idle_cycles", bot.idleCycles},
            {"hp_pct", std::round(bot.hpPct * 10.0f) / 10.0f},
            {"mana_pct", std::round(bot.manaPct * 10.0f) / 10.0f},
            {"level", bot.level},
            {"mission", {{"kind", bot.mission.KindName()}, {"target", bot.mission.target}, {"role", bot.mission.role}, {"revision", bot.missionRevision}, {"planner_goal", bot.mission.PlannerGoal()}}},
            {"lifecycle_generation", bot.lifecycleGeneration},
            {"top_need", {{"kind", bot.topNeed.KindName()}, {"urgent", bot.topNeed.urgent}, {"score", bot.topNeed.score}, {"reason", bot.topNeed.reason}}},
            {"lifecycle", {{"lane", bot.lifecycle.LaneName()}, {"maintenance", bot.lifecycle.MaintenanceName()}, {"urgent", bot.lifecycle.urgent}, {"score", bot.lifecycle.score}, {"reason", bot.lifecycle.reason}, {"bag_space_pct", bot.lifecycle.bagSpacePct}, {"durability_pct", bot.lifecycle.durabilityPct}, {"food_count", bot.lifecycle.foodCount}, {"drink_count", bot.lifecycle.drinkCount}, {"ammo_count", bot.lifecycle.ammoCount}}},
            {"gear", {{"avg_item_level", std::round(bot.avgItemLevel * 10.0f) / 10.0f},
                      {"expected_avg_item_level", std::round(bot.expectedAvgItemLevel * 10.0f) / 10.0f},
                      {"band", bot.gearBand}}},
            {"travel", {{"active", bot.travelActive}, {"label", bot.travelLabel}, {"radius", std::round(bot.travelRadius * 10.0f) / 10.0f}, {"last_result", (bot.travelLastResult == TravelResult::Reached) ? "reached" : (bot.travelLastResult == TravelResult::TimedOut) ? "timed_out"
                                                                                                                                                                                                                      : (bot.travelLastResult == TravelResult::Aborted)    ? "aborted"
                                                                                                                                                                                                                                                                           : "none"},
                        {"last_change_ms", bot.travelLastChangeMs}}},
            {"profession", {{"active", bot.professionActive}, {"activity", (bot.professionActivity == ProfessionActivity::Fishing) ? "fishing" : "none"}, {"last_result", (bot.professionLastResult == ProfessionResult::Succeeded) ? "succeeded" : (bot.professionLastResult == ProfessionResult::TimedOut)      ? "timed_out"
                                                                                                                                                                                                                                                : (bot.professionLastResult == ProfessionResult::Aborted)         ? "aborted"
                                                                                                                                                                                                                                                : (bot.professionLastResult == ProfessionResult::FailedPermanent) ? "failed_permanent"
                                                                                                                                                                                                                                                : (bot.professionLastResult == ProfessionResult::FailedTemporary) ? "failed_temporary"
                                                                                                                                                                                                                                                : (bot.professionLastResult == ProfessionResult::Started)         ? "started"
                                                                                                                                                                                                                                                                                                                  : "none"},
                            {"last_change_ms", bot.professionLastChangeMs}}},
            {"debug", {{"control_cooldown_remaining_ms", bot.controlCooldownRemainingMs}, {"ollama_backoff_ms", bot.controlOllamaBackoffMs}, {"memory_pending_writes", bot.memoryPendingWrites}, {"memory_next_flush_ms", bot.memoryNextFlushMs}}},
            {"active_quest_ids", bot.activeQuestIds},
            {"post_grind_decision", bot.postGrindDecision},
            {"post_grind_options", bot.postGrindOptions},
            {"active_quests", questList}};
        nlohmann::json decisionOptions = nlohmann::json::array();
        for (auto const& option : bot.decisionOptions)
        {
            nlohmann::json arguments = nlohmann::json::object();
            if (option.action == "request_talk_to_quest_giver")
                arguments["quest_id"] = option.questId;
            else if (option.action == "request_move_hop")
            {
                arguments["nav_epoch"] = bot.navEpoch;
                arguments["candidate_id"] = option.candidateId;
            }
            else if (option.entryId)
                arguments["entry_id"] = option.entryId;
            bool turnIn = option.questId && std::any_of(bot.activeQuests.begin(), bot.activeQuests.end(),
                [&](auto const& quest) { return quest.questId == option.questId && quest.status == QUEST_STATUS_COMPLETE; });
            int priority = turnIn ? (option.action == "request_stop_grind" ? 0 : option.local ? 1 : 2) :
                option.lifecycleTravel ? 10 : option.objectiveType.empty() ? 30 : 20;
            decisionOptions.push_back({
                {"action", option.action},
                {"arguments", arguments},
                {"turn_in", turnIn},
                {"priority", priority},
                {"quest_id", option.questId},
                {"entry_id", option.entryId},
                {"target_name", option.targetName},
                {"reason", option.reason},
                {"distance", option.distance},
                {"local", option.local},
                {"lifecycle_travel", option.lifecycleTravel},
                {"destination_type", option.destinationType},
                {"destination_entry", option.destinationEntry},
                {"navigation_direction", option.navigationDirection},
                {"candidate_id", option.candidateId},
                {"objective_type", option.objectiveType},
                {"objective_target_id", option.objectiveTargetId},
                {"source_target", option.sourceTarget}
            });
        }
        json["decision_options"] = std::move(decisionOptions);
        json["bot"]["lifecycle_active"] = bot.lifecycleActive;
        json["bot"]["actionable_goal"] = bot.actionableGoal;
        json["bot"]["mission_goal_stale"] = bot.actionableGoal != bot.mission.PlannerGoal();
        json["services"] = nlohmann::json::object();
        for (auto const& [name, availability] : bot.serviceAvailability)
            json["services"][name] = {{"local", availability.local}, {"travel", availability.travel},
                                      {"needed", availability.needed}, {"reason", availability.reason}};
        json["world_model"] = BuildWorldModelJson();
        json["local_area_model"] = {
            {"zone", normalizedZone},
            {"area", normalizedArea},
            {"area_role", localProfile.areaRole},
            {"recommended_level_band", {localProfile.levelBandMin, localProfile.levelBandMax}},
            {"population_model", {{"mob_density", localProfile.mobDensity}, {"respawn_rate", localProfile.respawnRate}, {"competition_level", localProfile.competitionLevel}}},
            {"activity_affordances", {{"supports_grinding", true}, {"supports_questing", true}, {"supports_exploration", true}, {"supports_safe_idle", localProfile.deathRisk == "low"}}},
            {"risk_model", {{"death_risk", localProfile.deathRisk}, {"corpse_run_severity", localProfile.corpseRunSeverity}, {"pull_complexity", localProfile.pullComplexity}}},
            {"movement_characteristics", {{"navigation_complexity", localProfile.navigationComplexity}, {"obstacle_frequency", localProfile.obstacleFrequency}, {"roaming_required", localProfile.roamingRequired}}}};
        nlohmann::json navCandidates = nlohmann::json::array();
        for (size_t i = 0; i < bot.navCandidates.size(); ++i)
        {
            navCandidates.push_back({{"candidate_id", std::string("nav_") + std::to_string(i)},
                                     {"label", bot.navCandidates[i].label},
                                     {"direction", bot.navCandidates[i].direction},
                                     {"distance_band", DistanceBandLabelForDistance(bot.navCandidates[i].distance2d)},
                                     {"has_los", bot.navCandidates[i].hasLOS},
                                     {"reachable", bot.navCandidates[i].reachable},
                                     {"can_move", bot.navCandidates[i].canMove},
                                     {"temporarily_blocked", bot.navCandidates[i].temporarilyBlocked},
                                     {"penalty_remaining_ms", bot.navCandidates[i].penaltyRemainingMs}});
        }
        nlohmann::json distanceBands = nlohmann::json::array();
        for (auto const &band : kMoveHopDistanceBands)
        {
            distanceBands.push_back({{"label", band.label}});
        }
        json["nav"] = {
            {"nav_epoch", bot.navEpoch},
            {"candidates", navCandidates},
            {"distance_bands", distanceBands}};
        nlohmann::json questGivers = nlohmann::json::array();
        for (auto const &giver : bot.questGiversInRange)
        {
            float bearing = BearingDegrees(bot.pos, giver.pos);
            questGivers.push_back({{"name", giver.name},
                                   {"type", giver.type},
                                   {"entry_id", giver.entryId},
                                   {"distance_band", DistanceBandLabelForDistance(giver.distance)},
                                   {"direction", DirectionLabelFromBearing(bearing)},
                                   {"quest_marker", giver.questMarker},
                                   {"available_quest_ids", giver.availableQuestIds},
                                   {"turn_in_quest_ids", giver.turnInQuestIds},
                                   {"available_new_quest_ids", giver.availableNewQuestIds},
                                   {"turn_in_active_quest_ids", giver.turnInActiveQuestIds}});
        }
        json["quest_givers_in_range"] = questGivers;
        if (!bot.lowGearSlots.empty())
        {
            nlohmann::json lowSlots = nlohmann::json::array();
            for (auto const &slot : bot.lowGearSlots)
            {
                lowSlots.push_back({{"slot", slot.slot}, {"item", slot.item}, {"item_level", slot.itemLevel}});
            }
            json["bot"]["gear"]["low_slots"] = lowSlots;
        }
        nlohmann::json nearbyEntities = nlohmann::json::array();
        std::vector<BotSnapshot::NearbyEntity> orderedEntities = bot.nearbyEntities;
        std::stable_partition(orderedEntities.begin(), orderedEntities.end(),
                              [](BotSnapshot::NearbyEntity const &entity)
                              {
                                  return entity.type != "game_object";
                              });
        for (auto const &entity : orderedEntities)
        {
            float bearing = BearingDegrees(bot.pos, entity.pos);
            float distance = Distance2d(bot.pos, entity.pos);
            nearbyEntities.push_back({{"name", entity.name},
                                      {"type", entity.type},
                                      {"entry_id", entity.entryId},
                                      {"distance_band", DistanceBandLabelForDistance(distance)},
                                      {"direction", DirectionLabelFromBearing(bearing)},
                                      {"is_quest_giver", entity.isQuestGiver},
                                      {"quest_marker", entity.questMarker},
                                      {"is_vendor", entity.isVendor},
                                      {"is_trainer", entity.isTrainer},
                                      {"is_repair", entity.isRepair},
                                      {"is_flight_master", entity.isFlightMaster},
                                      {"visible", true}});
            // Include quest relations beyond interaction range so control can
            // approach the correct turn-in NPC without guessing its identity.
            nlohmann::json turnInIds = nlohmann::json::array();
            if (entity.type == "npc" && entity.isQuestGiver)
            {
                auto bounds = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(entity.entryId);
                for (auto it = bounds.first; it != bounds.second; ++it)
                {
                    for (auto const& quest : bot.activeQuests)
                    {
                        if (quest.questId == it->second && quest.status == QUEST_STATUS_COMPLETE)
                            turnInIds.push_back(quest.questId);
                    }
                }
            }
            nearbyEntities.back()["turn_in_quest_ids"] = std::move(turnInIds);
        }
        json["nearby_entities"] = nearbyEntities;
        if (goal)
        {
            json["current_goal"] = goal->ToJson();
        }

        auto ageMs = [&](uint32 at) -> uint32
        {
            if (bot.nowMs == 0 || at == 0)
                return 0;
            return (bot.nowMs >= at) ? (bot.nowMs - at) : 0;
        };

        nlohmann::json recent;
        if (!bot.recentMovementAttempts.empty())
        {
            nlohmann::json arr = nlohmann::json::array();
            for (auto const& e : bot.recentMovementAttempts)
            {
                arr.push_back({
                    {"age_ms", ageMs(e.atMs)},
                    {"tool", BotRecentHistory::MovementToolName(e.tool)},
                    {"accepted", e.accepted},
                    {"reason", e.reason},
                    {"nav_epoch", e.navEpoch},
                    {"candidate_id", e.candidateId},
                    {"cand_reachable", e.candReachable},
                    {"cand_has_los", e.candHasLOS},
                    {"cand_can_move", e.candCanMove},
                    {"engine_reachable", e.engineReachable},
                    {"engine_has_los", e.engineHasLOS},
                });
            }
            recent["movement_attempts"] = std::move(arr);
        }
        if (!bot.recentMovementResults.empty())
        {
            nlohmann::json arr = nlohmann::json::array();
            for (auto const& e : bot.recentMovementResults)
            {
                arr.push_back({
                    {"age_ms", ageMs(e.atMs)},
                    {"travel_key", e.travelKey},
                    {"result", e.result},
                });
            }
            recent["movement_results"] = std::move(arr);
        }
        if (!bot.recentLongTermGoalChanges.empty())
        {
            nlohmann::json arr = nlohmann::json::array();
            for (auto const& e : bot.recentLongTermGoalChanges)
            {
                arr.push_back({
                    {"age_ms", ageMs(e.atMs)},
                    {"from", e.from},
                    {"to", e.to},
                });
            }
            recent["long_term_goal_changes"] = std::move(arr);
        }
        if (!bot.recentShortTermGoalsChanges.empty())
        {
            nlohmann::json arr = nlohmann::json::array();
            for (auto const& e : bot.recentShortTermGoalsChanges)
            {
                arr.push_back({
                    {"age_ms", ageMs(e.atMs)},
                    {"long_term_goal", e.longTermGoal},
                    {"goals", e.goals},
                });
            }
            recent["short_term_goal_changes"] = std::move(arr);
        }
        if (!bot.recentShortTermGoalCompletions.empty())
        {
            nlohmann::json arr = nlohmann::json::array();
            for (auto const& e : bot.recentShortTermGoalCompletions)
            {
                arr.push_back({
                    {"age_ms", ageMs(e.atMs)},
                    {"goal", e.goal},
                    {"travel_key", e.travelKey},
                });
            }
            recent["short_term_goal_completions"] = std::move(arr);
        }

        if (!recent.empty())
        {
            json["recent"] = std::move(recent);
        }
        return json;
    }

    std::string BuildPlannerStateSummary(BotSnapshot const &bot, WorldSnapshot const &world)
    {
        // Natural-language summary of state for the planner (no JSON).
        auto questTitleForId = [](uint32 questId) -> std::string
        {
            if (Quest const *quest = sObjectMgr->GetQuestTemplate(questId))
            {
                if (!quest->GetTitle().empty())
                {
                    return quest->GetTitle();
                }
            }
            std::ostringstream fallback;
            fallback << "Quest " << questId;
            return fallback.str();
        };

        std::ostringstream oss;
        oss << "Location: ";
        if (!world.zone.empty())
        {
            oss << world.zone;
        }
        else
        {
            oss << "unknown zone";
        }
        if (!world.area.empty())
        {
            oss << " / " << world.area;
        }
        oss << ". ";
        oss << "Level " << bot.level
            << ", HP " << std::round(bot.hpPct * 10.0f) / 10.0f << "%, Mana "
            << std::round(bot.manaPct * 10.0f) / 10.0f << "%.";
        oss << " Combat: " << (bot.inCombat ? "yes" : "no")
            << ", moving: " << (bot.isMoving ? "yes" : "no")
            << ", grind mode: " << (bot.grindMode ? "yes" : "no")
            << ", idle cycles: " << bot.idleCycles << ".\n";
        oss << "Actionable goal: " << bot.actionableGoal << "\n";
        oss << "Mission: " << bot.mission.KindName() << " (revision " << bot.missionRevision << ")";
        if (!bot.mission.target.empty())
            oss << ", target: " << bot.mission.target;
        if (!bot.mission.role.empty())
            oss << ", role: " << bot.mission.role;
        oss << ".\n";
        if (bot.topNeed.kind != BotNeedKind::None)
        {
            oss << "Top need: " << bot.topNeed.KindName() << ", score " << bot.topNeed.score
                << ", urgent: " << (bot.topNeed.urgent ? "yes" : "no")
                << ", reason: " << bot.topNeed.reason << ".\n";
        }
        oss << "Travel: " << (bot.travelActive ? "active" : "inactive");
        if (bot.travelActive && !bot.travelLabel.empty())
        {
            oss << " (" << bot.travelLabel << ")";
        }
        if (bot.travelRadius > 0.0f)
        {
            oss << ", radius " << std::round(bot.travelRadius * 10.0f) / 10.0f << "m";
        }
        oss << ".\n";
        oss << "Profession: " << (bot.professionActive ? "active" : "inactive");
        if (bot.professionActivity == ProfessionActivity::Fishing)
        {
            oss << " (fishing)";
        }
        oss << ".\n";

        auto ageMs = [&](uint32 at) -> uint32
        {
            if (bot.nowMs == 0 || at == 0)
                return 0;
            return (bot.nowMs >= at) ? (bot.nowMs - at) : 0;
        };

        bool hasRecent = !bot.recentMovementAttempts.empty() ||
                         !bot.recentMovementResults.empty() ||
                         !bot.recentShortTermGoalCompletions.empty();
        if (hasRecent)
        {
            oss << "Recent: ";
            bool first = true;

            // Movement attempts (recent failures).
            uint32 failuresShown = 0;
            for (auto it = bot.recentMovementAttempts.rbegin(); it != bot.recentMovementAttempts.rend() && failuresShown < 3; ++it)
            {
                if (it->accepted)
                    continue;
                if (!first)
                    oss << " | ";
                first = false;
                ++failuresShown;
                oss << "move rejected (" << it->reason << ", " << ageMs(it->atMs) / 1000 << "s ago)";
            }

            // Travel outcomes (timeouts/aborts).
            uint32 resultsShown = 0;
            for (auto it = bot.recentMovementResults.rbegin(); it != bot.recentMovementResults.rend() && resultsShown < 2; ++it)
            {
                if (it->result == "reached")
                    continue;
                if (!first)
                    oss << " | ";
                first = false;
                ++resultsShown;
                oss << "travel " << it->result << " (" << ageMs(it->atMs) / 1000 << "s ago)";
            }

            // Short-term completions.
            uint32 goalsShown = 0;
            for (auto it = bot.recentShortTermGoalCompletions.rbegin(); it != bot.recentShortTermGoalCompletions.rend() && goalsShown < 2; ++it)
            {
                if (it->goal.empty())
                    continue;
                if (!first)
                    oss << " | ";
                first = false;
                ++goalsShown;
                oss << "completed ST goal (" << ageMs(it->atMs) / 1000 << "s ago): " << it->goal;
            }

            oss << ".\n";
        }

        oss << "Weapons: ";
        if (!bot.weaponTypes.empty())
        {
            for (size_t i = 0; i < bot.weaponTypes.size(); ++i)
            {
                if (i > 0)
                {
                    oss << ", ";
                }
                oss << bot.weaponTypes[i];
            }
        }
        else
        {
            oss << "none";
        }
        oss << ".\n";

        oss << "Skills: ";
        if (!bot.professions.empty())
        {
            for (size_t i = 0; i < bot.professions.size(); ++i)
            {
                if (i > 0)
                {
                    oss << ", ";
                }
                oss << bot.professions[i];
            }
        }
        else
        {
            oss << "none";
        }
        oss << ".\n";

        oss << "Gear: avg item level " << std::round(bot.avgItemLevel * 10.0f) / 10.0f;
        if (bot.expectedAvgItemLevel > 0.0f)
        {
            oss << " (expected ~" << std::round(bot.expectedAvgItemLevel * 10.0f) / 10.0f
                << ", " << bot.gearBand << ").\n";
        }
        else
        {
            oss << ".\n";
        }
        if (!bot.lowGearSlots.empty())
        {
            oss << "Low gear slots: ";
            for (size_t i = 0; i < bot.lowGearSlots.size(); ++i)
            {
                if (i > 0)
                {
                    oss << ", ";
                }
                oss << bot.lowGearSlots[i].slot << " (" << bot.lowGearSlots[i].item;
                if (bot.lowGearSlots[i].itemLevel > 0)
                {
                    oss << " ilvl " << bot.lowGearSlots[i].itemLevel;
                }
                oss << ")";
            }
            oss << ".\n";
        }

        if (bot.activeQuests.empty())
        {
            oss << "Active quests: none.\n";
        }
        else
        {
            oss << "Active quests:\n";
            for (auto const &quest : bot.activeQuests)
            {
                bool eligibleForWorldActivity = quest.status == QUEST_STATUS_INCOMPLETE;
                bool needsTurnIn = quest.status == QUEST_STATUS_COMPLETE;
                std::string title = !quest.title.empty() ? quest.title : questTitleForId(quest.questId);
                oss << "- " << title << " (" << QuestStatusToString(quest.status) << ")";
                oss << ", eligible_for_world_activity: " << (eligibleForWorldActivity ? "yes" : "no")
                    << ", needs_turn_in: " << (needsTurnIn ? "yes" : "no") << ".";
                if (!quest.objectives.empty())
                {
                    oss << " Objectives: ";
                    bool first = true;
                    for (auto const &objective : quest.objectives)
                    {
                        if (!first)
                        {
                            oss << "; ";
                        }
                        first = false;
                        oss << objective.type;
                        if (!objective.targetName.empty())
                        {
                            oss << " " << objective.targetName;
                        }
                        else if (objective.targetId != 0)
                        {
                            oss << " " << objective.targetId;
                        }
                        oss << " " << objective.current << "/" << objective.required;
                    }
                    oss << ".";
                }
                oss << "\n";
            }
        }

        if (bot.questGiversInRange.empty())
        {
            oss << "Quest givers in range: none.\n";
        }
        else
        {
            oss << "Quest givers in range:\n";
            // Prefer turn-ins for active quests first (these are usually the most relevant).
            std::vector<BotSnapshot::QuestGiverInRange> ordered = bot.questGiversInRange;
            std::stable_sort(ordered.begin(), ordered.end(),
                             [](BotSnapshot::QuestGiverInRange const &a, BotSnapshot::QuestGiverInRange const &b)
                             {
                                 bool aTurnIn = !a.turnInActiveQuestIds.empty();
                                 bool bTurnIn = !b.turnInActiveQuestIds.empty();
                                 if (aTurnIn != bTurnIn)
                                     return aTurnIn > bTurnIn;
                                 return a.distance < b.distance;
                             });
            for (auto const &giver : ordered)
            {
                oss << "- " << giver.name;
                if (!giver.questMarker.empty())
                {
                    oss << " " << giver.questMarker;
                }
                if (!giver.turnInActiveQuestIds.empty())
                {
                    oss << " [turn_in_active]";
                }
                else if (!giver.availableNewQuestIds.empty())
                {
                    oss << " [available_new]";
                }
                oss << " (" << giver.type << "), distance "
                    << std::round(giver.distance * 10.0f) / 10.0f << "m, available quests: ";
                if (giver.availableQuestIds.empty())
                {
                    oss << "none";
                }
                else
                {
                    for (size_t i = 0; i < giver.availableQuestIds.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << ", ";
                        }
                        oss << questTitleForId(giver.availableQuestIds[i]);
                    }
                }
                oss << ", turn-in quests: ";
                if (giver.turnInQuestIds.empty())
                {
                    oss << "none";
                }
                else
                {
                    for (size_t i = 0; i < giver.turnInQuestIds.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << ", ";
                        }
                        oss << questTitleForId(giver.turnInQuestIds[i]);
                    }
                }
                oss << ".\n";
            }
        }

        // Nearby quest givers with visible markers, even if not currently interactable.
        // This helps planners recognize quest NPCs slightly outside interaction range.
        size_t nearbyQuestGiverCount = 0;
        if (!bot.nearbyEntities.empty())
        {
            for (auto const& entity : bot.nearbyEntities)
            {
                if (!entity.isQuestGiver || entity.questMarker.empty())
                {
                    continue;
                }
                // Skip those already captured as "in range".
                bool alreadyInRange = false;
                for (auto const& inRange : bot.questGiversInRange)
                {
                    if (inRange.entryId == entity.entryId && inRange.name == entity.name)
                    {
                        alreadyInRange = true;
                        break;
                    }
                }
                if (alreadyInRange)
                {
                    continue;
                }

                if (nearbyQuestGiverCount == 0)
                {
                    oss << "Quest givers nearby (not in interact range):\n";
                }
                if (nearbyQuestGiverCount >= 5)
                {
                    break;
                }
                oss << "- " << entity.name << " " << entity.questMarker
                    << " (npc), distance " << std::round(entity.distance * 10.0f) / 10.0f << "m.\n";
                nearbyQuestGiverCount++;
            }
        }
        if (nearbyQuestGiverCount == 0)
        {
            oss << "Quest givers nearby (not in interact range): none.\n";
        }

        if (!bot.questPois.empty())
        {
            oss << "Quest POIs:\n";
            size_t count = 0;
            for (auto const &poi : bot.questPois)
            {
                if (count >= 5)
                {
                    break;
                }
                float distance = Distance2d(bot.pos, poi.pos);
                float bearing = BearingDegrees(bot.pos, poi.pos);
                oss << "- " << questTitleForId(poi.questId)
                    << " objective " << poi.objectiveIndex
                    << " (" << (poi.isTurnIn ? "turn_in" : "objective") << ")"
                    << ", distance " << std::round(distance * 10.0f) / 10.0f << "m"
                    << ", direction " << DirectionLabelFromBearing(bearing) << ".\n";
                count += 1;
            }
            if (bot.questPois.size() > count)
            {
                oss << "- ...and " << (bot.questPois.size() - count) << " more POIs.\n";
            }
        }

        if (bot.navCandidates.empty())
        {
            oss << "Navigation options: none.\n";
        }
        else
        {
            oss << "Navigation options:\n";
            for (auto const &candidate : bot.navCandidates)
            {
                oss << "- " << candidate.label
                    << " (" << candidate.direction << "), distance " << std::round(candidate.distance2d * 10.0f) / 10.0f
                    << "m, los: " << (candidate.hasLOS ? "yes" : "no")
                    << ", reachable: " << (candidate.reachable ? "yes" : "no")
                    << ", can_move: " << (candidate.canMove ? "yes" : "no") << ".\n";
            }
        }

        if (!bot.nearbyEntities.empty())
        {
            oss << "Nearby entities:\n";
            size_t count = 0;
            std::vector<BotSnapshot::NearbyEntity> orderedEntities = bot.nearbyEntities;
            std::stable_partition(orderedEntities.begin(), orderedEntities.end(),
                                  [](BotSnapshot::NearbyEntity const &entity)
                                  {
                                      return entity.type != "game_object";
                                  });
            for (auto const &entity : orderedEntities)
            {
                if (count >= 5)
                {
                    break;
                }
                float distance = Distance2d(bot.pos, entity.pos);
                float bearing = BearingDegrees(bot.pos, entity.pos);
                oss << "- " << entity.name;
                if (!entity.questMarker.empty())
                {
                    oss << " " << entity.questMarker;
                }
                oss << " (" << entity.type << "), distance "
                    << std::round(distance * 10.0f) / 10.0f << "m";
                oss << ", direction " << DirectionLabelFromBearing(bearing);
                if (entity.isQuestGiver)
                {
                    oss << ", quest giver";
                }
                oss << ".\n";
                count += 1;
            }
            if (bot.nearbyEntities.size() > count)
            {
                oss << "- ...and " << (bot.nearbyEntities.size() - count) << " more nearby entities.\n";
            }
        }

        return oss.str();
    }

    std::string BuildPlannerContext(BotSnapshot const &bot, WorldSnapshot const &world)
    {
        // Shared context header for planner prompts.
        const OllamaSettings settings = GetOllamaSettings();
        std::string const& systemPrompt = GetPrompt(LLMRole::Planner, settings);

        std::ostringstream oss;
        if (!systemPrompt.empty())
        {
            oss << systemPrompt << "\n\n";
        }
        oss << "STATE_SUMMARY\n";
        oss << BuildPlannerStateSummary(bot, world) << "\n\n";
        return oss.str();
    }

    void AppendPlannerStateSummary(std::string const& botName, std::string const& summary)
    {
        if (!g_EnableOllamaBotPlannerStateSummaryLog || g_OllamaBotPlannerStateSummaryLogPath.empty())
        {
            return;
        }
        std::lock_guard<std::mutex> lock(plannerSummaryLogMutex);
        std::ofstream out(g_OllamaBotPlannerStateSummaryLogPath, std::ios::out | std::ios::app);
        if (!out.is_open())
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Failed to open planner state summary log file.");
            return;
        }
        out << "bot=" << botName << " ts_ms=" << GetNowMs() << "\n";
        out << summary << "\n";
        out << "----\n";
    }

    std::string BuildLongTermGoalReviewPrompt(BotSnapshot const &bot, WorldSnapshot const &world, std::string const &proposedGoal)
    {
        // Ask the planner to confirm or revise a long-term goal.
        std::ostringstream oss;
        oss << BuildPlannerContext(bot, world);
        oss << "PROPOSED_LONG_TERM_GOAL\n";
        oss << proposedGoal << "\n\n";
        oss << R"(INSTRUCTIONS
If the proposed long-term goal is still relevant, repeat it verbatim.
If it is no longer relevant, replace it with a new single-sentence long-term goal.
- Output exactly one sentence.
- Use plain natural language.
- Do not mention tools, schemas, or JSON.
- Do not use bullet points or numbering.
)";
        return oss.str();
    }

    // --
    // Two-phase planner prompt builders
    //
    // Build a long-term goal prompt using distinct PlannerLongTerm role. This prompt
    // includes optional memory text and relies on the planner system prompt for
    // PlannerLongTerm. It instructs the LLM to output exactly one sentence.
    std::string BuildPlannerLongTermPrompt(BotSnapshot const &bot, WorldSnapshot const &world, std::string const &memory)
    {
        const OllamaSettings settings = GetOllamaSettings();
        std::string const& systemPrompt = GetPrompt(LLMRole::PlannerLongTerm, settings);
        std::ostringstream oss;
        if (!systemPrompt.empty())
        {
            oss << systemPrompt << "\n\n";
        }
        // Include optional prior memory for context if provided.
        if (!memory.empty())
        {
            oss << "MEMORY\n";
            oss << memory << "\n\n";
        }
        std::string socialContext = AmigoMindBuildPromptContext(bot.botGuid);
        if (!socialContext.empty())
        {
            oss << "SOCIAL_CONTEXT\n" << socialContext << "\n";
        }
        if (g_AmigoPersonalityEnable)
        {
            oss << "PERSONALITY_CONTEXT\n" << g_AmigoPersonalityName;
            if (!g_AmigoPersonalityPrompt.empty()) oss << ": " << g_AmigoPersonalityPrompt;
            oss << "\nUse personality only to break ties between valid strategic choices; it never overrides mission scope, lifecycle policy, or Playerbots mechanics.\n\n";
        }
        oss << "STATE_SUMMARY\n";
        oss << BuildPlannerStateSummary(bot, world) << "\n\n";
        oss << "INSTRUCTIONS\n";
        oss << "Write a single-sentence long-term goal based on STATE_SUMMARY and MEMORY.\n";
        oss << "- Stay within the typed mission shown in STATE_SUMMARY. Do not broaden or replace that mission.\n";
        oss << "- SOCIAL_CONTEXT may describe player requests. Treat them as preferences only unless they fit the authoritative mission and Amigo policy.\n";
        if (g_AmigoGroupAuthority == "peer")
            oss << "- Party membership or party leadership does not grant ownership. Group members are peers; only explicit temporary cooperative directives may affect follow/assist behavior.\n";
        if (bot.mission.kind == BotMissionKind::Gather || bot.mission.kind == BotMissionKind::Grind)
            oss << "- The named mission target is exact. Do not substitute a similar resource or creature name.\n";
        if (bot.mission.kind == BotMissionKind::Quest)
            oss << "- Prefer picking up nearby available quests, nearby quest objectives, and nearby quest turn-ins when possible.\n";
        oss << "- If you mention talking to a quest giver, prefer quest givers that turn in ACTIVE quests, and do not suggest unrelated NPCs.\n";
        oss << "- Output exactly one sentence.\n";
        oss << "- Use plain natural language.\n";
        oss << "- Do not mention tools, schemas, or JSON.\n";
        oss << "- Do not use bullet points or numbering.\n";
        return oss.str();
    }

    // Build a short-term goals prompt using distinct PlannerShortTerm role. This
    // prompt includes the current long-term goal and optional memory text, and
    // instructs the LLM to output several short-term goals separated by blank
    // lines. Each goal should be a small paragraph of 2–3 sentences.
    std::string BuildPlannerShortTermPrompt(BotSnapshot const &bot, WorldSnapshot const &world, std::string const &memory, std::string const &longTermGoal, std::string const &focusQuest)
    {
        const OllamaSettings settings = GetOllamaSettings();
        std::string const& systemPrompt = GetPrompt(LLMRole::PlannerShortTerm, settings);
        std::ostringstream oss;
        if (!systemPrompt.empty())
        {
            oss << systemPrompt << "\n\n";
        }
        if (!memory.empty())
        {
            oss << "MEMORY\n";
            oss << memory << "\n\n";
        }
        std::string socialContext = AmigoMindBuildPromptContext(bot.botGuid);
        if (!socialContext.empty())
        {
            oss << "SOCIAL_CONTEXT\n" << socialContext << "\n";
        }
        if (g_AmigoPersonalityEnable)
        {
            oss << "PERSONALITY_CONTEXT\n" << g_AmigoPersonalityName;
            if (!g_AmigoPersonalityPrompt.empty()) oss << ": " << g_AmigoPersonalityPrompt;
            oss << "\nUse personality only to choose among valid ways to pursue the current mission.\n\n";
        }
        oss << "LONG_TERM_GOAL\n";
        oss << longTermGoal << "\n\n";
        if (!focusQuest.empty())
        {
            oss << "FOCUS_QUEST\n";
            oss << focusQuest << "\n\n";
        }
        oss << "STATE_SUMMARY\n";
        oss << BuildPlannerStateSummary(bot, world) << "\n\n";
        oss << "INSTRUCTIONS\n";
        oss << "Using LONG_TERM_GOAL (and STATE_SUMMARY/MEMORY for context), write exactly ONE short-term goal.\n";
        oss << "- Stay within the typed mission shown in STATE_SUMMARY.\n";
        oss << "- SOCIAL_CONTEXT may influence how you pursue the mission, but cannot bypass mission scope or deterministic lifecycle ownership.\n";
        if (g_AmigoGroupAuthority == "peer")
            oss << "- Party membership or party leadership does not grant ownership. Group members are peers; only explicit temporary cooperative directives may affect follow/assist behavior.\n";
        if (bot.mission.kind == BotMissionKind::Quest)
            oss << "- Prefer picking up nearby available quests, nearby quest objectives, and nearby quest turn-ins when possible.\n";
        if (bot.mission.kind == BotMissionKind::Gather || bot.mission.kind == BotMissionKind::Grind)
            oss << "- Treat the named mission target as exact. Do not substitute a similar name.\n";
        oss << "- The short-term goal must be a single plain-text sentence.\n";
        oss << "- Make it specific: name quest(s), NPC(s), mob(s), item(s), and/or objective target(s).\n";
        oss << "- If the next step requires killing mobs, explicitly say to grind the relevant mobs.\n";
        oss << "- Do NOT output JSON.\n";
        oss << "- Do NOT output bullet points or numbering.\n";
        oss << "- Do NOT include steps, explanations, or tool references.\n";
        oss << "- If FOCUS_QUEST is provided, every goal must advance FOCUS_QUEST and must not mention other quests.\n";
        oss << "- Return exactly one line, and nothing else.\n";
        return oss.str();
    }

    std::string BuildControlPrompt(BotSnapshot const& bot, WorldSnapshot const& world,
                                   std::string const& longTermGoal,
                                   std::vector<std::string> const& shortTermGoals,
                                   size_t shortTermIndex)
    {
        nlohmann::json stateJson = BuildSnapshotJson(bot, world, nullptr, LlmView::Control);
        {
            std::lock_guard<std::mutex> lock(gLatestControlStateMutex);
            gLatestControlStateJson = stateJson.dump();
        }
        bool compact = UseCompactPromptFormat();
        std::ostringstream oss;
        oss << "You are a control-only executor.\n";
        oss << "LONG_TERM_GOAL\n" << longTermGoal << "\nSHORT_TERM_GOAL\n"
            << CurrentShortTermGoal(shortTermGoals, shortTermIndex) << "\n\n";
        oss << (compact ? "S:\n" : "STATE_JSON\n") << stateJson.dump(compact ? -1 : 2);
        oss << R"(

INSTRUCTIONS
- Choose only an action and its exact arguments from decision_options.
- Prefer the lowest priority number. Completed quests require turn-in before further combat.
- Combat, loot, recovery, active travel, and active maintenance already own execution: return no output.
- If no legal action exists or no new action is needed, return no output and preserve the current owner.
- Never request idle to represent no output. Waiting does not authorize random movement.
- Service options own their travel and can start during unrelated movement.
- A remote quest route is movement only. Talk only when a local quest-talk option exists.
- Item objectives do not authorize creature combat without an explicit source mapping.
- Output one <tool_call>{"name":"action","arguments":{}}</tool_call>, or no output.

)";
        oss << BuildControlToolInstructions(compact ? "S" : "STATE_JSON");
        return oss.str();
    }

    bool HasQuestGiverForQuestId(BotSnapshot const &snapshot, uint32 questId)
    {
        // Validate a quest talk command against nearby quest givers.
        for (auto const &giver : snapshot.questGiversInRange)
        {
            if (std::find(giver.availableQuestIds.begin(), giver.availableQuestIds.end(), questId) != giver.availableQuestIds.end())
            {
                return true;
            }
            if (std::find(giver.turnInQuestIds.begin(), giver.turnInQuestIds.end(), questId) != giver.turnInQuestIds.end())
            {
                return true;
            }
        }
        return false;
    }

    bool HasRemainingMissionObjective(BotSnapshot const& snapshot)
    {
        for (auto const& quest : snapshot.activeQuests)
            if (quest.status == QUEST_STATUS_COMPLETE)
                return false;
        bool foundMatchingObjective = false;
        for (auto const& quest : snapshot.activeQuests)
        {
            if (quest.status == QUEST_STATUS_COMPLETE)
                return false;
            for (auto const& objective : quest.objectives)
            {
                if (objective.type != "creature" ||
                    (snapshot.mission.kind != BotMissionKind::Quest && objective.targetName != snapshot.mission.target))
                    continue;

                foundMatchingObjective = true;
                if (quest.status == QUEST_STATUS_INCOMPLETE && objective.current < objective.required)
                    return true;
            }
        }

        return !foundMatchingObjective && snapshot.activeQuests.empty();
    }

    bool TryParseNavCandidateIndex(std::string const &candidateId, size_t &outIndex)
    {
        // Current candidate IDs are of the form "nav_<index>".
        // This helper is intentionally strict to avoid accidental acceptance of
        // geometry-bearing IDs.
        constexpr char const* kPrefix = "nav_";
        if (candidateId.rfind(kPrefix, 0) != 0)
        {
            return false;
        }
        std::string suffix = candidateId.substr(std::strlen(kPrefix));
        if (suffix.empty())
        {
            return false;
        }
        for (unsigned char c : suffix)
        {
            if (!std::isdigit(c))
            {
                return false;
            }
        }
        try
        {
            outIndex = static_cast<size_t>(std::stoul(suffix));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseMoveHopNavArguments(nlohmann::json const& args, uint32& outNavEpoch, std::string& outCandidateId)
    {
        if (!args.is_object())
        {
            return false;
        }
        if (!args.contains("nav_epoch"))
        {
            return false;
        }
        if (!args.contains("candidate_id"))
        {
            return false;
        }

        if (args["nav_epoch"].is_number_unsigned())
        {
            outNavEpoch = args["nav_epoch"].get<uint32>();
        }
        else if (args["nav_epoch"].is_number_integer())
        {
            int64 epoch = args["nav_epoch"].get<int64>();
            if (epoch < 0 || epoch > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            outNavEpoch = static_cast<uint32>(epoch);
        }
        else
        {
            return false;
        }

        outCandidateId.clear();
        if (args["candidate_id"].is_string())
        {
            outCandidateId = TrimCopy(args["candidate_id"].get<std::string>());
        }
        else if (args["candidate_id"].is_number_unsigned())
        {
            outCandidateId = "nav_" + std::to_string(args["candidate_id"].get<uint64>());
        }
        else if (args["candidate_id"].is_number_integer())
        {
            int64 idx = args["candidate_id"].get<int64>();
            if (idx < 0)
            {
                return false;
            }
            outCandidateId = "nav_" + std::to_string(static_cast<uint64>(idx));
        }
        else
        {
            return false;
        }

        std::transform(outCandidateId.begin(), outCandidateId.end(), outCandidateId.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!outCandidateId.empty() &&
            std::all_of(outCandidateId.begin(), outCandidateId.end(),
                        [](unsigned char c) { return std::isdigit(c); }))
        {
            outCandidateId = "nav_" + outCandidateId;
        }

        return !outCandidateId.empty();
    }

    bool ParseQuestIdArguments(nlohmann::json const& args, uint32& questId)
    {
        if (!args.is_object())
        {
            return false;
        }
        if (!args.contains("quest_id"))
        {
            return false;
        }

        if (args["quest_id"].is_number_unsigned())
        {
            questId = args["quest_id"].get<uint32>();
        }
        else if (args["quest_id"].is_number_integer())
        {
            int64 parsed = args["quest_id"].get<int64>();
            if (parsed <= 0 || parsed > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            questId = static_cast<uint32>(parsed);
        }
        else
        {
            return false;
        }

        return questId != 0;
    }

    bool ParseEntryIdArguments(nlohmann::json const& args, uint32& entryId)
    {
        if (!args.is_object())
        {
            return false;
        }
        if (!args.contains("entry_id"))
        {
            return false;
        }

        if (args["entry_id"].is_number_unsigned())
        {
            entryId = args["entry_id"].get<uint32>();
        }
        else if (args["entry_id"].is_number_integer())
        {
            int64 parsed = args["entry_id"].get<int64>();
            if (parsed <= 0 || parsed > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            entryId = static_cast<uint32>(parsed);
        }
        else
        {
            return false;
        }

        return entryId != 0;
    }

    void LogControlToolAccepted(std::string const &name, ControlAction::Capability capability, std::string const &reason)
    {
        // Structured logging for accepted tool calls.
        LOG_INFO("server.loading", "[ControlTool] name={}", name);
        LOG_INFO("server.loading", "[ControlTool] resolved={}", CapabilityName(capability));
        LOG_INFO("server.loading", "[ControlTool] gated=true reason={}", reason);
    }

    void LogControlToolRejected(std::string const &name, std::string const &reason)
    {
        // Structured logging for rejected tool calls.
        LOG_INFO("server.loading", "[ControlTool] rejected name={} reason={}", name, reason);
    }

    struct LlmBotState
    {
        // Per-bot LLM runtime state for throttling and context.
        enum class ControlState : uint8
        {
            Idle,
            Waiting,
            FailureHold,
            Cooldown
        };

        ThinkScheduler scheduler;
        // Tracks the per-bot startup delay window and ensures we force a planner refresh
        // exactly once when the delay expires.
        uint32 startupDelayUntilMs = 0;
        bool startupPlannerTriggered = false;
        std::string configuredMissionSignature;
        std::string longTermGoal;
        std::vector<std::string> shortTermGoals;
        std::atomic<size_t> shortTermIndex{0};
        std::atomic<bool> strategicBusy{false};
        std::atomic<bool> controlBusy{false};
        std::atomic<bool> promptInFlight{false};
        // Control planner (Ollama) backpressure.
        // These are atomic because the control request runs in a detached thread.
        std::atomic<ControlState> controlState{ControlState::Idle};
        std::atomic<uint32> nextAllowedAttemptMs{0};
        std::atomic<uint32> nextPlannerShortTickMs{0};
        std::atomic<uint32> nextPlannerLongTickMs{0};
        std::atomic<uint32> nextStrategicAllowedMs{0};
        std::atomic<uint32> failureHoldUntilMs{0};
        std::atomic<uint32> ollamaCooldownMs{kOllamaBaseCooldownMs};
        uint32 lastGoalChangeMs = 0;
        bool hasStrategicResult = false;
        std::atomic<bool> loggedStrategicParseError{false};
        std::atomic<bool> loggedControlParseError{false};
        bool hasLastPosition = false;
        Position3 lastPosition;
        uint32 idleCycles = 0;

        // Monotonic nav epoch for navigation candidates.
        uint32 navEpoch = 0;

        // Movement is owned per-bot and must be ticked before any LLM/AI logic.
        BotMovement movement;

        // Travel semantics (completion/failure) for the last requested destination.
        BotTravel travel;

        // Persistent memory (two-tier cache + DB backing), read-only to LLM.
        BotMemory memory;

        // Professions (execution-only) such as fishing.
        BotProfession profession;

        // Short recent buffers for movement outcomes and goal transitions.
        BotRecentHistory recentHistory;

        // Temporary avoidance for navigation candidates that timed out.
        BotNavigationPenalty navigationPenalty;

        // Guard to record travel outcomes into memory once.
        uint32 lastTravelRecordedMs = 0;
        uint32 lastTravelAdvanceMs = 0;
        std::atomic<uint8> lastControlCapability{static_cast<uint8>(ControlAction::Capability::Idle)};
        std::atomic<bool> forceControl{false};
        std::atomic<bool> forceStrategic{false};

        // Track quest completion transitions to force planner refresh (dedup per quest id).
        std::unordered_set<uint32> notifiedCompletedQuestIds;

        // Guard to record profession outcomes into memory once.
        uint32 lastProfessionRecordedMs = 0;
    };

    std::unordered_map<uint64, std::shared_ptr<LlmBotState>> botStates;
}

static std::mutex sPlannerRefreshMutex;
static std::unordered_map<uint64, uint32> sPendingLongTermPlannerRefreshMs;

static uint32 ConsumeLongTermPlannerRefresh(uint64 guid)
{
    std::lock_guard<std::mutex> lock(sPlannerRefreshMutex);
    auto it = sPendingLongTermPlannerRefreshMs.find(guid);
    if (it == sPendingLongTermPlannerRefreshMs.end())
    {
        return 0;
    }
    uint32 at = it->second;
    sPendingLongTermPlannerRefreshMs.erase(it);
    return at;
}

std::string GetAmigoLatestControlStateJson()
{
    std::lock_guard<std::mutex> lock(gLatestControlStateMutex);
    return gLatestControlStateJson;
}

OllamaBotControlLoop::OllamaBotControlLoop() : WorldScript("OllamaBotControlLoop") {}

void RequestLongTermPlannerRefresh(uint64 guid, uint32 nowMs)
{
    if (guid == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sPlannerRefreshMutex);
    sPendingLongTermPlannerRefreshMs[guid] = nowMs;
}

static void EnqueueStrategicUpdate(uint64 guid, PendingStrategicUpdate update)
{
    // Stage the planner result until the main thread applies it.
    std::lock_guard<std::mutex> lock(pendingMutex);
    pendingStrategicUpdates[guid] = std::move(update);
}

void OllamaBotControlLoop::OnUpdate(uint32 diff)
{
    // Deliver worker completions on the world thread before reading live bot state.
    AmigoLlmDispatchDrainCompletions();
    // Main update loop: manage LLM planning and control per bot.
    if (!g_OllamaBotRuntime.enable_control)
    {
        return;
    }

    for (auto const &itr : ObjectAccessor::GetPlayers())
    {
        Player *bot = itr.second;
        if (!bot || !bot->IsInWorld())
        {
            continue;
        }

        PlayerbotAI *ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai || !ai->IsBotAI())
        {
            continue;
        }

        if (!g_OllamaBotControlBotName.empty())
        {
            // Optional bot-name allowlist (comma separated).
            bool allowed = false;
            std::stringstream ss(g_OllamaBotControlBotName);
            std::string name;

            while (std::getline(ss, name, ','))
            {
                if (bot->GetName() == name)
                {
                    allowed = true;
                    break;
                }
            }

            if (!allowed)
                continue;
        }

        // Autonomous login uses Playerbots' random-bot path. Remove its
        // background movement before Amigo evaluates the next control action.
        SuppressAutonomousMovement(bot, ai);

        uint32 nowMs = getMSTime();
        uint64 guid = bot->GetGUID().GetRawValue();
        auto &statePtr = botStates[guid];
        if (!statePtr)
        {
            statePtr = std::make_shared<LlmBotState>();

            // Expose the per-bot movement instance to other scripts...
            BotMovementRegistry::Register(guid, &statePtr->movement);
            BotTravelRegistry::Register(guid, &statePtr->travel);
            BotMemoryRegistry::Register(guid, &statePtr->memory);
            BotProfessionRegistry::Register(guid, &statePtr->profession);
            BotRecentHistoryRegistry::Register(guid, &statePtr->recentHistory);
            statePtr->memory.Initialize(guid, nowMs);

            uint32 delayUntilMs = nowMs;
            if (g_OllamaBotRuntime.control_startup_delay_ms > 0)
            {
                delayUntilMs = nowMs + static_cast<uint32>(g_OllamaBotRuntime.control_startup_delay_ms);
                statePtr->controlState.store(LlmBotState::ControlState::Cooldown, std::memory_order_relaxed);
                statePtr->nextAllowedAttemptMs.store(delayUntilMs, std::memory_order_relaxed);
            }

            statePtr->startupDelayUntilMs = delayUntilMs;
            statePtr->startupPlannerTriggered = false;
            statePtr->nextPlannerShortTickMs.store(delayUntilMs, std::memory_order_relaxed);
            statePtr->nextPlannerLongTickMs.store(delayUntilMs, std::memory_order_relaxed);
            statePtr->nextStrategicAllowedMs.store(delayUntilMs, std::memory_order_relaxed);
        }
        LlmBotState &state = *statePtr;

        BotMission configuredMission = BuildConfiguredMission();
        std::string configuredSignature = MissionSignature(configuredMission);
        if (state.configuredMissionSignature != configuredSignature)
        {
            BotMissionState currentMission = BotMissionRegistry::Instance().Get(guid);
            std::string currentSignature = MissionSignature(currentMission.mission);
            if (currentSignature != configuredSignature)
            {
                uint64 revision = BotMissionRegistry::Instance().Replace(guid, configuredMission);
                state.forceStrategic.store(true, std::memory_order_relaxed);
                state.forceControl.store(false, std::memory_order_relaxed);
                state.longTermGoal.clear();
                state.shortTermGoals.clear();
                state.shortTermIndex.store(0, std::memory_order_relaxed);
                state.hasStrategicResult = false;
                LOG_INFO("server.loading", "[OllamaBotAmigo] Mission assigned for {}: kind={} target='{}' role='{}' revision={}",
                         bot->GetName(), configuredMission.KindName(), configuredMission.target, configuredMission.role, revision);
            }
            state.configuredMissionSignature = configuredSignature;
        }

        // Tick movement first; travel completion is checked every tick.
        state.movement.Update(diff);
        state.travel.Update(bot, nowMs);

        if (g_OllamaBotControlClearGoalsOnConfigLoad)
        {
            state.longTermGoal.clear();
            state.shortTermGoals.clear();
            state.shortTermIndex.store(0, std::memory_order_relaxed);
            state.hasStrategicResult = false;
            state.lastGoalChangeMs = 0;
            state.loggedStrategicParseError.store(false, std::memory_order_relaxed);
            state.loggedControlParseError.store(false, std::memory_order_relaxed);
            state.recentHistory.ClearGoalHistory();
        }

        // Tick professions (non-combat execution). Uses Playerbots actions but no movement.
        state.profession.Update(bot, ai, nowMs);

        if (state.travel.LastResult() == TravelResult::Reached &&
            state.travel.LastChangeMs() > state.lastTravelAdvanceMs)
        {
            state.lastTravelAdvanceMs = state.travel.LastChangeMs();
            if (!state.shortTermGoals.empty())
            {
                size_t currentIndex = state.shortTermIndex.load(std::memory_order_relaxed);
                std::string completed = CurrentShortTermGoal(state.shortTermGoals, currentIndex);
                if (!completed.empty())
                {
                    std::string travelKey;
                    if (auto cur = state.travel.Current())
                    {
                        travelKey = cur->key;
                    }
                    state.recentHistory.RecordShortTermGoalCompleted(nowMs, std::move(completed), std::move(travelKey));
                }
                size_t nextIndex = (currentIndex + 1) % state.shortTermGoals.size();
                state.shortTermIndex.store(nextIndex, std::memory_order_relaxed);
            }
            uint8 lastCap = state.lastControlCapability.load(std::memory_order_relaxed);
            if (lastCap == static_cast<uint8>(ControlAction::Capability::MoveHop) ||
                lastCap == static_cast<uint8>(ControlAction::Capability::MoveHopNpc))
            {
                state.forceControl.store(true, std::memory_order_relaxed);
            }
        }

        // Update memory (write-behind flushes are rate-limited internally).
        state.memory.Update(nowMs);

        // Tie travel outcomes into memory to reduce thrash and improve stability.
        if (state.travel.LastResult() != TravelResult::None && state.travel.LastChangeMs() > state.lastTravelRecordedMs)
        {
            state.lastTravelRecordedMs = state.travel.LastChangeMs();
            std::string key = "travel:unknown";
            if (auto cur = state.travel.Current())
            {
                if (!cur->key.empty())
                    key = "travel:" + cur->key;
            }

            {
                std::string travelKey;
                if (auto cur = state.travel.Current())
                {
                    travelKey = cur->key;
                }
                std::string result;
                switch (state.travel.LastResult())
                {
                case TravelResult::Reached:
                    result = "reached";
                    break;
                case TravelResult::TimedOut:
                    result = "timed_out";
                    break;
                case TravelResult::Aborted:
                    result = "aborted";
                    break;
                default:
                    result = "unknown";
                    break;
                }
                state.recentHistory.RecordMovementResult(nowMs, std::move(travelKey), std::move(result));
            }

            switch (state.travel.LastResult())
            {
            case TravelResult::Reached:
                if (auto cur = state.travel.Current(); cur && !cur->retryKey.empty())
                    state.navigationPenalty.Clear(cur->retryKey);
                state.memory.ClearFailures(key);
                break;
            case TravelResult::TimedOut:
                if (auto cur = state.travel.Current(); cur && !cur->retryKey.empty())
                {
                    state.navigationPenalty.Penalize(cur->retryKey, nowMs, kNavigationTimeoutPenaltyMs);
                    state.forceControl.store(true, std::memory_order_relaxed);
                    LOG_INFO("server.loading",
                             "[OllamaBotAmigo] Temporarily blocked navigation candidate for {} after travel timeout: candidate={} duration_ms={}",
                             bot->GetName(), cur->retryKey, kNavigationTimeoutPenaltyMs);
                }
                state.memory.RecordFailure(key, FailureType::Retryable, nowMs);
                break;
            case TravelResult::Aborted:
                state.memory.RecordFailure(key, FailureType::Temporary, nowMs);
                break;
            default:
                break;
            }
        }

        // Tie profession outcomes into memory. This prevents spammy retries and gives the controller
        // realistic cooldown behavior.
        if (state.profession.LastResult() != ProfessionResult::None &&
            state.profession.LastChangeMs() > state.lastProfessionRecordedMs &&
            !state.profession.Active())
        {
            state.lastProfessionRecordedMs = state.profession.LastChangeMs();
            std::string key = "profession:fishing";

            switch (state.profession.LastResult())
            {
            case ProfessionResult::Succeeded:
                state.memory.ClearFailures(key);
                break;
            case ProfessionResult::TimedOut:
                state.memory.RecordFailure(key, FailureType::Retryable, nowMs);
                break;
            case ProfessionResult::Aborted:
                state.memory.RecordFailure(key, FailureType::Temporary, nowMs);
                break;
            case ProfessionResult::FailedPermanent:
                state.memory.RecordFailure(key, FailureType::Permanent, nowMs);
                break;
            case ProfessionResult::FailedTemporary:
                state.memory.RecordFailure(key, FailureType::Temporary, nowMs);
                break;
            default:
                break;
            }
        }
        if (state.movement.IsMoving())
        {
            continue;
        }

        if (state.profession.Active())
        {
            // While a profession session is running, do not invoke the LLM/controller.
            continue;
        }
        if (HasAmigoPendingControl(guid))
            continue;

        // Deterministic lifecycle work runs before every LLM pause/cooldown gate.
        // Existing Amigo movement/profession operations are allowed to finish, then
        // Needs/Maintenance can own execution without rewriting BotMission.
        {
            BotMissionState lifecycleMission = BotMissionRegistry::Instance().Get(guid);
            const bool optionalMaintenanceWindow = !bot->IsInCombat() &&
                                                   !bot->isMoving() &&
                                                   !state.travel.Active();
            if (ServiceDeterministicLifecycle(bot, ai, lifecycleMission.revision, optionalMaintenanceWindow))
                continue;
        }

        if (state.promptInFlight.load(std::memory_order_relaxed))
        {
            continue;
        }
        uint32 globalPauseUntil = globalControlPauseUntilMs.load(std::memory_order_relaxed);
        if (globalPauseUntil > 0 && nowMs < globalPauseUntil)
        {
            continue;
        }
        if (globalPauseUntil > 0 && nowMs >= globalPauseUntil)
        {
            uint32 expected = globalPauseUntil;
            if (globalControlPauseUntilMs.compare_exchange_strong(expected, 0u))
            {
                globalControlResumeBaseMs.store(nowMs, std::memory_order_relaxed);
            }
        }
        uint32 resumeBaseMs = globalControlResumeBaseMs.load(std::memory_order_relaxed);
        LlmBotState::ControlState controlState = state.controlState.load(std::memory_order_relaxed);
        if (controlState == LlmBotState::ControlState::Waiting)
        {
            continue;
        }
        if (controlState == LlmBotState::ControlState::FailureHold)
        {
            uint32 holdUntil = state.failureHoldUntilMs.load(std::memory_order_relaxed);
            if (nowMs < holdUntil)
            {
                continue;
            }
            state.controlState.store(LlmBotState::ControlState::Cooldown, std::memory_order_relaxed);
            controlState = LlmBotState::ControlState::Cooldown;
        }
        if (controlState == LlmBotState::ControlState::Cooldown)
        {
            uint32 nextAttempt = state.nextAllowedAttemptMs.load(std::memory_order_relaxed);
            if (nowMs < nextAttempt)
            {
                continue;
            }
            state.controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
            controlState = LlmBotState::ControlState::Idle;
        }

        // When the startup delay expires, force a long-term planner refresh immediately
        // (once per bot state lifetime). This ensures the bot has a long-term goal as
        // soon as it becomes eligible to run.
        if (!state.startupPlannerTriggered && state.startupDelayUntilMs > 0 && nowMs >= state.startupDelayUntilMs)
        {
            state.startupPlannerTriggered = true;
            state.forceStrategic.store(true, std::memory_order_relaxed);
            state.nextPlannerShortTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextPlannerLongTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextStrategicAllowedMs.store(0u, std::memory_order_relaxed);
            state.hasStrategicResult = false;
        }

        uint32 nextShortTick = state.nextPlannerShortTickMs.load(std::memory_order_relaxed);
        uint32 nextLongTick = state.nextPlannerLongTickMs.load(std::memory_order_relaxed);
        uint32 nextDueTick = std::min(nextShortTick, nextLongTick);

        if (resumeBaseMs > 0 && nextDueTick < resumeBaseMs)
        {
            uint32 jitterMs = static_cast<uint32>(guid % kGlobalResumeSpreadMs);
            uint32 shifted = resumeBaseMs + jitterMs;
            if (nextShortTick < shifted)
            {
                nextShortTick = shifted;
                state.nextPlannerShortTickMs.store(nextShortTick, std::memory_order_relaxed);
            }
            if (nextLongTick < shifted)
            {
                nextLongTick = shifted;
                state.nextPlannerLongTickMs.store(nextLongTick, std::memory_order_relaxed);
            }
            nextDueTick = std::min(nextShortTick, nextLongTick);
        }

        // If any quest just transitioned to COMPLETE, force a strategic refresh immediately
        // (short-term + long-term) regardless of normal planner tick delays.
        bool newlyCompletedQuest = false;
        std::unordered_set<uint32> completedNow;
        for (auto const& entry : bot->getQuestStatusMap())
        {
            if (entry.second.Status == QUEST_STATUS_COMPLETE)
            {
                completedNow.insert(entry.first);
                if (state.notifiedCompletedQuestIds.insert(entry.first).second)
                {
                    newlyCompletedQuest = true;
                }
            }
        }
        for (auto it = state.notifiedCompletedQuestIds.begin(); it != state.notifiedCompletedQuestIds.end();)
        {
            if (completedNow.find(*it) == completedNow.end())
            {
                it = state.notifiedCompletedQuestIds.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (newlyCompletedQuest)
        {
            // Ask the control model for the next bounded action immediately.
            // The snapshot will contain the server-generated post-grind options.
            state.forceControl.store(true, std::memory_order_relaxed);
            state.forceStrategic.store(true, std::memory_order_relaxed);
            state.nextPlannerShortTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextPlannerLongTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextStrategicAllowedMs.store(0u, std::memory_order_relaxed);
            state.hasStrategicResult = false;
            nextShortTick = nowMs;
            nextLongTick = nowMs;
            nextDueTick = nowMs;
        }

        if (nowMs < nextDueTick)
        {
            continue;
        }
        BotSnapshot snapshot = BuildBotSnapshot(bot, ai);
        snapshot.nowMs = nowMs;

        // A timed-out hop must not be immediately selected again when the
        // navigation epoch changes. Candidate IDs are stable for the generated
        // directional set, so the penalty follows the same candidate across
        // snapshots while other directions remain available.
        state.navigationPenalty.Prune(nowMs);
        for (size_t i = 0; i < snapshot.navCandidates.size(); ++i)
        {
            auto& candidate = snapshot.navCandidates[i];
            std::string candidateId = "nav_" + std::to_string(i);
            uint32 penaltyRemaining = state.navigationPenalty.Remaining(candidateId, nowMs);
            if (penaltyRemaining == 0)
                continue;

            candidate.temporarilyBlocked = true;
            candidate.penaltyRemainingMs = penaltyRemaining;
            candidate.canMove = false;
        }

        // Publish internal navigation candidates for controller resolution (not serialized to the LLM).
        {
            BotNavState navState;
            uint32 navEpoch = ++state.navEpoch;
            snapshot.navEpoch = navEpoch;
            navState.navEpoch = navEpoch;
            navState.candidates.reserve(snapshot.navCandidates.size());
            for (size_t i = 0; i < snapshot.navCandidates.size(); ++i)
            {
                auto const &c = snapshot.navCandidates[i];
                NavCandidateInternal internal;
                internal.candidateId = "nav_" + std::to_string(i);
                internal.mapId = snapshot.mapId;
                internal.x = c.pos.x;
                internal.y = c.pos.y;
                internal.z = c.pos.z;
                internal.reachable = c.reachable;
                internal.hasLOS = c.hasLOS;
                internal.canMove = c.canMove;
                navState.candidates.push_back(std::move(internal));
            }
            BotNavStateRegistry::SetState(guid, navState);
        }
        // Attach travel status for the controller LLM.
        snapshot.travelActive = state.travel.Active();
        snapshot.travelLastResult = state.travel.LastResult();
        snapshot.travelLastChangeMs = state.travel.LastChangeMs();
        if (auto cur = state.travel.Current())
        {
            snapshot.travelRadius = cur->radius;
            snapshot.travelLabel = "movement";
        }

        snapshot.professionActive = state.profession.Active();
        snapshot.professionActivity = state.profession.Activity();
        snapshot.professionLastResult = state.profession.LastResult();
        snapshot.professionLastChangeMs = state.profession.LastChangeMs();

        snapshot.memoryPendingWrites = state.memory.PendingWrites();
        snapshot.memoryNextFlushMs = state.memory.NextDbFlushInMs(nowMs);
        uint32 nextAllowed = state.nextAllowedAttemptMs.load(std::memory_order_relaxed);
        snapshot.controlCooldownRemainingMs = (nowMs < nextAllowed) ? (nextAllowed - nowMs) : 0;
        snapshot.controlOllamaBackoffMs = state.ollamaCooldownMs.load(std::memory_order_relaxed);

        snapshot.recentMovementAttempts = state.recentHistory.GetMovementAttempts(8);
        snapshot.recentMovementResults = state.recentHistory.GetMovementResults(6);
        snapshot.recentLongTermGoalChanges = state.recentHistory.GetLongTermGoalChanges(4);
        snapshot.recentShortTermGoalsChanges = state.recentHistory.GetShortTermGoalsChanges(4);
        snapshot.recentShortTermGoalCompletions = state.recentHistory.GetShortTermGoalCompletions(6);
        WorldSnapshot world = BuildWorldSnapshot(bot);
        bool isIdleCandidate = !snapshot.inCombat && !snapshot.isMoving;
        if (isIdleCandidate)
        {
            if (state.hasLastPosition)
            {
                float distance = Distance(snapshot.pos, state.lastPosition);
                if (distance < kIdlePositionEpsilon)
                {
                    state.idleCycles += 1;
                }
                else
                {
                    state.idleCycles = 0;
                }
            }
            else
            {
                state.idleCycles = 0;
            }
        }
        else
        {
            state.idleCycles = 0;
        }
        state.lastPosition = snapshot.pos;
        state.hasLastPosition = true;
        snapshot.idleCycles = state.idleCycles;

        PendingStrategicUpdate strategicUpdate;
        bool hasStrategicUpdate = false;

        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto stratIt = pendingStrategicUpdates.find(guid);
            if (stratIt != pendingStrategicUpdates.end())
            {
                strategicUpdate = stratIt->second;
                pendingStrategicUpdates.erase(stratIt);
                hasStrategicUpdate = true;
            }
        }

        if (hasStrategicUpdate && strategicUpdate.hasUpdate)
        {
            if (!BotMissionRegistry::Instance().RevisionMatches(guid, strategicUpdate.missionRevision))
            {
                LOG_INFO("server.loading", "[OllamaBotAmigo] Discarded stale planner result for {}: mission revision {} no longer current", bot->GetName(), strategicUpdate.missionRevision);
                hasStrategicUpdate = false;
            }
            else if (GetBotLifecycleGeneration(guid) != strategicUpdate.lifecycleGeneration)
            {
                LOG_INFO("server.loading", "[OllamaBotAmigo] Discarded stale planner result for {}: lifecycle generation {} no longer current", bot->GetName(), strategicUpdate.lifecycleGeneration);
                hasStrategicUpdate = false;
            }
        }

        if (hasStrategicUpdate && strategicUpdate.hasUpdate)
        {
            if (snapshot.actionableGoal != snapshot.mission.PlannerGoal())
            {
                strategicUpdate.plan.longTermGoal = snapshot.actionableGoal;
                strategicUpdate.plan.shortTermGoals = {snapshot.actionableGoal};
                strategicUpdate.refreshedShortTermGoals = true;
            }
            std::string previousLongTermGoalForHistory = state.longTermGoal;
            bool hasLongTermGoal = !state.longTermGoal.empty();
            bool canGenerateGoal = true;

            if (!hasLongTermGoal && state.lastGoalChangeMs > 0 && nowMs - state.lastGoalChangeMs < kStrategicGoalChangeCooldownMs)
            {
                canGenerateGoal = false;
            }

            bool longTermChanged = strategicUpdate.plan.longTermGoal != state.longTermGoal;

            if (longTermChanged)
            {
                if (hasLongTermGoal || canGenerateGoal)
                {
                    state.longTermGoal = strategicUpdate.plan.longTermGoal;
                    state.shortTermIndex.store(0, std::memory_order_relaxed);
                    if (strategicUpdate.refreshedShortTermGoals)
                    {
                        state.shortTermGoals = std::move(strategicUpdate.plan.shortTermGoals);
                    }
                    state.lastGoalChangeMs = nowMs;
                    state.recentHistory.RecordLongTermGoalChange(nowMs, previousLongTermGoalForHistory, state.longTermGoal);
                    if (strategicUpdate.refreshedShortTermGoals)
                    {
                        state.recentHistory.RecordShortTermGoalsChange(nowMs, state.longTermGoal, state.shortTermGoals);
                    }
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Long-term goal updated for {}: {}", bot->GetName(), state.longTermGoal);
                    state.nextStrategicAllowedMs.store(nowMs + kStrategicGoalChangeCooldownMs, std::memory_order_relaxed);
                }
                else if (g_EnableOllamaBotAmigoDebug && !canGenerateGoal)
                {
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Planner update ignored due to cooldown for {}", bot->GetName());
                }
            }
            else if (strategicUpdate.refreshedShortTermGoals && canGenerateGoal)
            {
                state.longTermGoal = strategicUpdate.plan.longTermGoal;
                state.shortTermGoals = std::move(strategicUpdate.plan.shortTermGoals);
                state.shortTermIndex.store(0, std::memory_order_relaxed);
                state.lastGoalChangeMs = nowMs;
                state.recentHistory.RecordShortTermGoalsChange(nowMs, state.longTermGoal, state.shortTermGoals);
                LOG_INFO("server.loading", "[OllamaBotAmigo] Short-term goals refreshed for {}", bot->GetName());
                state.nextStrategicAllowedMs.store(nowMs + kStrategicGoalChangeCooldownMs, std::memory_order_relaxed);
            }

            if (!state.longTermGoal.empty())
            {
                std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
                BotLLMContext &ctx = GetBotLLMContext()[guid];
                ctx.lastPlan = BuildPlanSummary(state.longTermGoal, state.shortTermGoals,
                                                state.shortTermIndex.load(std::memory_order_relaxed));
            }

            state.hasStrategicResult = true;
        }

        // Out-of-band planner refresh request (e.g., after quest turn-ins).
        // This is guarded at the request site to avoid spamming.
        if (ConsumeLongTermPlannerRefresh(guid) > 0)
        {
            state.forceStrategic.store(true, std::memory_order_relaxed);
            state.nextPlannerLongTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextPlannerShortTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextStrategicAllowedMs.store(0u, std::memory_order_relaxed);
            state.hasStrategicResult = false;
        }

        // Seed next due times if unset (prevents immediate repeated replans after restart).
        if (state.nextPlannerLongTickMs.load(std::memory_order_relaxed) == 0)
        {
            state.nextPlannerLongTickMs.store(nowMs + GetPlannerLongTermDelayMs(), std::memory_order_relaxed);
        }
        if (state.nextPlannerShortTickMs.load(std::memory_order_relaxed) == 0)
        {
            state.nextPlannerShortTickMs.store(nowMs + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
        }

        uint32 nextShortPlanner = state.nextPlannerShortTickMs.load(std::memory_order_relaxed);
        uint32 nextLongPlanner = state.nextPlannerLongTickMs.load(std::memory_order_relaxed);

        bool longTermDue = (!state.hasStrategicResult) || (nowMs >= nextLongPlanner) || state.longTermGoal.empty();
        bool shortTermDue = (!state.hasStrategicResult) || (nowMs >= nextShortPlanner) || state.shortTermGoals.empty();

        // Run planner work only when either layer is due, and keep the lightweight scheduler jitter
        // to avoid thundering herds.
        bool forceStrategic = state.forceStrategic.load(std::memory_order_relaxed);
        bool shouldRunStrategic = g_EnableOllamaBotPlanner &&
                                  (longTermDue || shortTermDue) &&
                                  (forceStrategic || !state.hasStrategicResult || state.scheduler.ShouldRunStrategic(nowMs));
        uint32 nextStrategicAllowedMs = state.nextStrategicAllowedMs.load(std::memory_order_relaxed);
        if (!snapshot.inCombat && shouldRunStrategic && (forceStrategic || nowMs >= nextStrategicAllowedMs) && !state.strategicBusy.exchange(true))
        {
            // Planner runs in a detached thread to avoid blocking the world loop.
            state.forceStrategic.store(false, std::memory_order_relaxed);
            state.promptInFlight.store(true, std::memory_order_relaxed);
            std::string botName = bot->GetName();
            std::string previousLongTermGoal = state.longTermGoal;
            bool hasShortTermGoals = !state.shortTermGoals.empty();
            std::shared_ptr<LlmBotState> stateRef = statePtr;
            bool runLongTerm = longTermDue;
            bool runShortTerm = shortTermDue;

            if (!AmigoLlmDispatchSubmit([guid, snapshot, world, botName, previousLongTermGoal, hasShortTermGoals, stateRef, runLongTerm, runShortTerm]()
                        {
                            // Planner worker thread.
                            bool loggedSummary = false;
                            auto clearBusy = [&]() {
                                stateRef->strategicBusy.store(false);
                                stateRef->promptInFlight.store(false, std::memory_order_relaxed);
                            };
                            auto rejectAndBackoff = [&](char const* msg) {
                                bool expected = false;
                                if (stateRef->loggedStrategicParseError.compare_exchange_strong(expected, true))
                                {
                                    LOG_ERROR("server.loading", "[OllamaBotAmigo] Planner reply rejected: {}.", msg);
                                }
                                uint32 now = getMSTime();
                                stateRef->nextPlannerShortTickMs.store(now + kPlannerFailureDelayMs, std::memory_order_relaxed);
                                stateRef->nextPlannerLongTickMs.store(now + kPlannerFailureDelayMs, std::memory_order_relaxed);
                                clearBusy();
                            };
                            auto rejectAndBackoffShort = [&](char const* msg)
                            {
                                bool expected = false;
                                if (stateRef->loggedStrategicParseError.compare_exchange_strong(expected, true))
                                    LOG_ERROR("server.loading", "[OllamaBotAmigo] Planner reply rejected: {}.", msg);

                                uint32 now = getMSTime();
                                stateRef->nextPlannerShortTickMs.store(now + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                                clearBusy();
                            };

                            PendingStrategicUpdate update;
                            update.missionRevision = snapshot.missionRevision;
                            update.lifecycleGeneration = snapshot.lifecycleGeneration;

                            // If only short-term goals are due, reuse the existing long-term goal and refresh short-term goals only.
                            std::string longTermGoal;
                            bool needsShortTermGoals = false;

                            if (snapshot.actionableGoal != snapshot.mission.PlannerGoal())
                            {
                                longTermGoal = snapshot.actionableGoal;
                                update.plan.longTermGoal = longTermGoal;
                                update.plan.shortTermGoals = {longTermGoal};
                                update.refreshedShortTermGoals = true;
                                update.hasUpdate = true;
                            }
                            else if (!runLongTerm && runShortTerm && !previousLongTermGoal.empty())
                            {
                                longTermGoal = previousLongTermGoal;
                                update.plan.longTermGoal = longTermGoal;
                                update.hasUpdate = true;
                                needsShortTermGoals = true;
                            }
                            else
                            {
                                // Long-term planning path (also refreshes short-term goals).
                                // Explicit typed missions own semantic intent. The LLM may decompose
                                // them, but it may not replace them with a different activity.
                                if (snapshot.mission.kind != BotMissionKind::Quest)
                                {
                                    longTermGoal = snapshot.mission.PlannerGoal();
                                    if (longTermGoal.empty())
                                    {
                                        rejectAndBackoff("typed mission has no target");
                                        return;
                                    }
                                    update.plan.longTermGoal = longTermGoal;
                                    update.hasUpdate = true;
                                    needsShortTermGoals = !hasShortTermGoals || longTermGoal != previousLongTermGoal;
                                }
                                else if (!g_OllamaBotControlForcedLongTermGoal.empty())
                                {
                                    longTermGoal = g_OllamaBotControlForcedLongTermGoal;

                                    std::string reason;
                                    if (!ValidatePlannerSentence(longTermGoal, reason))
                                    {
                                        rejectAndBackoff("invalid forced long-term goal");
                                        return;
                                    }

                                    update.plan.longTermGoal = longTermGoal;
                                    update.hasUpdate = true;
                                    needsShortTermGoals = !hasShortTermGoals || longTermGoal != previousLongTermGoal;
                                }
                                else
                                {
                                    std::string summary = BuildPlannerStateSummary(snapshot, world);
                                    AppendPlannerStateSummary(botName, summary);
                                    loggedSummary = true;
                                    std::string longTermPrompt = BuildPlannerLongTermPrompt(snapshot, world, std::string());
                                    std::string longTermReply = QueryOllamaLLMOnce(longTermPrompt, g_OllamaBotControlPlannerLongTermModel, g_AmigoThinkPlanner);
                                    std::string longTermDraft = ExtractPlannerSentence(longTermReply);

                                    if (g_EnableOllamaBotAmigoDebug || g_EnableOllamaBotPlannerDebug)
                                    {
                                        LOG_INFO("server.loading", "[OllamaBotAmigo] Planner long-term draft for '{}':{}", botName, longTermReply);
                                    }

                                    if (longTermDraft.empty())
                                    {
                                        rejectAndBackoff("missing long-term goal sentence");
                                        return;
                                    }

                                    {
                                        std::string reason;
                                        if (!ValidatePlannerSentence(longTermDraft, reason))
                                        {
                                            rejectAndBackoff("invalid long-term draft");
                                            return;
                                        }
                                    }

                                    std::string reviewPrompt = BuildLongTermGoalReviewPrompt(snapshot, world, longTermDraft);
                                    std::string reviewReply = QueryOllamaLLMOnce(reviewPrompt, g_OllamaBotControlPlannerLongTermModel, g_AmigoThinkPlanner);
                                    longTermGoal = ExtractPlannerSentence(reviewReply);

                                    if (g_EnableOllamaBotAmigoDebug || g_EnableOllamaBotPlannerDebug)
                                    {
                                        LOG_INFO("server.loading", "[OllamaBotAmigo] Planner long-term review for '{}':\\n{}", botName, reviewReply);
                                    }

                                    if (longTermGoal.empty())
                                    {
                                        rejectAndBackoff("missing long-term goal");
                                        return;
                                    }

                                    {
                                        std::string reason;
                                        if (!ValidatePlannerSentence(longTermGoal, reason))
                                        {
                                            rejectAndBackoff("invalid long-term goal");
                                            return;
                                        }
                                    }

                                    update.plan.longTermGoal = longTermGoal;
                                    update.hasUpdate = true;
                                    needsShortTermGoals = !hasShortTermGoals || longTermGoal != previousLongTermGoal;
                                }
                            }

                            if (needsShortTermGoals)
                            {
                                if (!loggedSummary)
                                {
                                    std::string summary = BuildPlannerStateSummary(snapshot, world);
                                    AppendPlannerStateSummary(botName, summary);
                                    loggedSummary = true;
                                }
                                BotSnapshot::QuestProgress const *focusQuest = FindFocusQuest(snapshot, longTermGoal);
                                std::string focusQuestBlock;
                                if (focusQuest)
                                {
                                    focusQuestBlock = BuildFocusQuestBlock(*focusQuest);
                                }
                                std::string shortTermPrompt = BuildPlannerShortTermPrompt(snapshot, world, std::string(), longTermGoal, focusQuestBlock);
                                std::string shortTermReply = QueryOllamaLLMOnce(shortTermPrompt, g_OllamaBotControlPlannerShortTermModel, g_AmigoThinkPlanner);

                                if (g_EnableOllamaBotAmigoDebug || g_EnableOllamaBotPlannerDebug)
                                {
                                    LOG_INFO("server.loading", "[OllamaBotAmigo] Planner short-term goals for '{}':\\n{}", botName, shortTermReply);
                                }

                                    std::string goal = ParseShortTermGoal(shortTermReply);
                                    std::string reason;
                                    if (focusQuest && MentionsOtherQuest(goal, snapshot.activeQuests, focusQuest->title))
                                    {
                                        rejectAndBackoffShort("short-term goal mentions other quest");
                                        return;
                                    }
                                    if (!ValidateShortTermGoal(goal, reason))
                                    {
                                        rejectAndBackoffShort("invalid short-term goal");
                                        return;
                                    }
                                    update.plan.shortTermGoals = {goal};
                                    update.refreshedShortTermGoals = true;
                                }

                            // Schedule next planner ticks (separate long vs short intervals).
                            uint32 nowTick = getMSTime();
                            stateRef->nextPlannerShortTickMs.store(nowTick + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                            if (runLongTerm)
                            {
                                stateRef->nextPlannerLongTickMs.store(nowTick + GetPlannerLongTermDelayMs(), std::memory_order_relaxed);
                            }

                            stateRef->loggedStrategicParseError.store(false);
                            if (BotMissionRegistry::Instance().RevisionMatches(guid, update.missionRevision) &&
                                GetBotLifecycleGeneration(guid) == update.lifecycleGeneration)
                            {
                                EnqueueStrategicUpdate(guid, std::move(update));
                            }
                            else
                            {
                                LOG_INFO("server.loading", "[OllamaBotAmigo] Planner result became stale before enqueue for '{}' (mission revision {}, lifecycle generation {})", botName, update.missionRevision, update.lifecycleGeneration);
                            }
                            clearBusy(); }))
            {
                stateRef->strategicBusy.store(false, std::memory_order_release);
                stateRef->promptInFlight.store(false, std::memory_order_relaxed);
                stateRef->nextPlannerShortTickMs.store(getMSTime() + kPlannerFailureDelayMs, std::memory_order_relaxed);
                stateRef->nextPlannerLongTickMs.store(getMSTime() + kPlannerFailureDelayMs, std::memory_order_relaxed);
                LOG_INFO("server.loading", "[OllamaBotAmigo] Planner request dropped for '{}' because the bounded LLM queue is full.", botName);
            }
        }
        // HARD WAIT: if a control request is in flight for this bot, do nothing this tick.
        if (state.controlBusy.load(std::memory_order_relaxed))
        {
            // DO NOT clear or mutate controlBusy here.
            // controlBusy is owned by the response thread only.
            continue;
        }

        bool forceControl = state.forceControl.load(std::memory_order_relaxed);

        if (!g_EnableOllamaBotControl || state.shortTermGoals.empty() ||
            (!forceControl && !state.scheduler.ShouldRunControl(nowMs, guid)))
        {
            continue;
        }

        if (!snapshot.inCombat)
        {
            // Do not plan while already moving (let the movement complete), except allow
            // a stop-grind request so the bot can exit grind mode promptly.
            if (snapshot.isMoving && !snapshot.grindMode)
            {
                continue;
            }

            // If following correctly, avoid unnecessary replans.
            std::string currentActivity;
            std::string activityReason;
            if (TryGetActivityState(bot, currentActivity, activityReason))
            {
                if (NormalizeCommandToken(currentActivity) == "follow" && IsFollowingCorrectly(bot, ai))
                {
                    continue;
                }
            }

            // Cooldown / backoff gate (applies only when not busy).
            uint32 nowAttemptMs = getMSTime();
            uint32 nextAttempt = state.nextAllowedAttemptMs.load(std::memory_order_relaxed);
            if (nowAttemptMs < nextAttempt)
            {
                continue;
            }

            // Set busy ONCE: from here until the response thread clears it, do not plan again.
            if (state.controlBusy.exchange(true, std::memory_order_acq_rel))
            {
                continue; // already waiting on Ollama
            }

            if (forceControl)
            {
                state.forceControl.store(false, std::memory_order_relaxed);
            }

            state.controlState.store(LlmBotState::ControlState::Waiting, std::memory_order_relaxed);
            state.promptInFlight.store(true, std::memory_order_relaxed);

            std::string shortTermGoal = CurrentShortTermGoal(state.shortTermGoals,
                                                             state.shortTermIndex.load(std::memory_order_relaxed));
            std::string prompt = BuildControlPrompt(snapshot, world, state.longTermGoal, state.shortTermGoals,
                                                    state.shortTermIndex.load(std::memory_order_relaxed));
            std::string botName = bot->GetName();
            bool isStopped = ai->HasStrategy("stay", BOT_STATE_NON_COMBAT);
            std::shared_ptr<LlmBotState> stateRef = statePtr;
            size_t shortTermGoalCount = state.shortTermGoals.size();

            if (!AmigoLlmDispatchSubmit([guid, prompt, botName, snapshot, isStopped, stateRef, shortTermGoalCount]()
                        {
                // Control worker thread that parses tool calls.
                // SINGLE EXIT: all paths funnel through this guard
                auto clearBusy = [&]()
                {
                    stateRef->controlBusy.store(false, std::memory_order_release);
                    stateRef->promptInFlight.store(false, std::memory_order_relaxed);
                };

                auto recordGlobalFailure = [stateRef]()
                {
                    uint32 nowMs = getMSTime();
                    std::lock_guard<std::mutex> lock(globalControlMutex);
                    if (nowMs - globalFailureWindowStartMs > kGlobalFailureWindowMs)
                    {
                        globalFailureWindowStartMs = nowMs;
                        globalFailureCount = 0;
                    }
                    globalFailureCount += 1;
                    if (globalFailureCount >= kGlobalFailureThreshold)
                    {
                        globalControlPauseUntilMs.store(nowMs + kGlobalControlPauseMs, std::memory_order_relaxed);
                        globalFailureWindowStartMs = nowMs;
                        globalFailureCount = 0;
                    }
                };

                auto applyFailureBackoff = [stateRef, recordGlobalFailure, &clearBusy]()
                {
                    uint32 nowMs = getMSTime();
                    uint32 prev = stateRef->ollamaCooldownMs.load(std::memory_order_relaxed);
                    uint32 next = std::min(prev * 2u, kOllamaMaxCooldownMs);
                    stateRef->ollamaCooldownMs.store(std::max(next, kOllamaBaseCooldownMs), std::memory_order_relaxed);
                    uint32 cooldownMs = stateRef->ollamaCooldownMs.load(std::memory_order_relaxed);
                    stateRef->nextAllowedAttemptMs.store(nowMs + cooldownMs, std::memory_order_relaxed);
                    stateRef->failureHoldUntilMs.store(nowMs + kOllamaFailureHoldMs, std::memory_order_relaxed);
                    stateRef->controlState.store(LlmBotState::ControlState::FailureHold, std::memory_order_relaxed);
                    stateRef->nextPlannerShortTickMs.store(nowMs + kPlannerFailureDelayMs, std::memory_order_relaxed);
                    stateRef->nextPlannerLongTickMs.store(nowMs + kPlannerFailureDelayMs, std::memory_order_relaxed);
                    recordGlobalFailure();
                    clearBusy();
                };

                AmigoOllamaResult llmResult = QueryOllamaLLMEx(
                    g_OllamaBotControlControlModel, prompt, g_AmigoThinkControl);

                // A provider failure is different from an intentional control
                // no-op. Only failures use parser/network backoff.
                if (!llmResult.ok)
                {
                    applyFailureBackoff();
                    return;
                }

                if (llmResult.noOp)
                {
                    if (g_EnableOllamaBotControlDebug || g_EnableOllamaBotAmigoDebug)
                    {
                        LOG_INFO("server.loading",
                                 "[OllamaBotAmigo] Mock control no-op for {}: no server-approved decision option is currently available.",
                                 snapshot.botName);
                    }
                    stateRef->nextPlannerShortTickMs.store(
                        getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                    stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                    clearBusy();
                    return;
                }

                std::string llmReply = llmResult.text;

                ControlActionState actionState;
                actionState.missionRevision = snapshot.missionRevision;
                actionState.lifecycleGeneration = snapshot.lifecycleGeneration;
                bool hasAction = false;
                std::string trimmed = llmReply;
                size_t start = trimmed.find_first_not_of(" \t\r\n");
                size_t end = trimmed.find_last_not_of(" \t\r\n");
                if (start != std::string::npos && end != std::string::npos)
                {
                    trimmed = trimmed.substr(start, end - start + 1);
                }
                else
                {
                    trimmed.clear();
                }

                if (trimmed.empty())
                {
                    // Treat empty output as a failure and back off.
                    applyFailureBackoff();
                    return;
                }

                ToolCall toolCall;
                std::string toolJson;
                if (!TryExtractSingleToolCall(trimmed, toolCall, toolJson))
                {
                    bool expected = false;
                    if (stateRef->loggedControlParseError.compare_exchange_strong(expected, true))
                    {
                        LOG_ERROR("server.loading", "[OllamaBotAmigo] Control reply rejected: output must be a single <tool_call> block.");
                    }
                    // Parser failures should not retry at tick speed.
                    applyFailureBackoff();
                    return;
                }

                if (g_EnableOllamaBotAmigoDebug || g_EnableOllamaBotControlDebug)
                {
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Control LLM reply for '{}':\n{}", botName, llmReply);
                }

                ControlToolDefinition definition;
                if (!FindControlToolDefinition(toolCall.name, definition))
                {
                    LogControlToolRejected(toolCall.name, "unknown_tool");
                    stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                    clearBusy();
                    return;
                }
                std::string argumentError;
                if (!ValidateControlArguments(definition, toolCall.arguments, argumentError))
                {
                    LogControlToolRejected(toolCall.name, argumentError);
                    stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs());
                    clearBusy();
                    return;
                }
                if (!definition.requiresDirection && !definition.requiresDistance && !definition.requiresQuestId && !definition.requiresEntryId &&
                    !definition.requiresSkill && !definition.requiresIntent && !definition.requiresMessage &&
                    !definition.requiresNavEpoch && !definition.requiresCandidateId &&
                    !toolCall.arguments.empty())
                {
                    LogControlToolRejected(toolCall.name, "unexpected_arguments");
                    stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                    clearBusy();
                    return;
                }

                std::string gateReason = "allowed";
                bool accepted = false;
                std::string missionGateReason;
                if (!MissionAllowsControlCapability(snapshot.mission, definition.capability, missionGateReason,
                                                    snapshot.postGrindDecision))
                {
                    LogControlToolRejected(toolCall.name, missionGateReason);
                    stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                    clearBusy();
                    return;
                }

                ControlAction action;
                action.capability = definition.capability;
                bool optionMatched = false;
                for (auto const& option : snapshot.decisionOptions)
                {
                    if (option.action != toolCall.name)
                        continue;
                    if (definition.requiresQuestId && toolCall.arguments.at("quest_id") != option.questId)
                        continue;
                    if (definition.requiresEntryId && toolCall.arguments.at("entry_id") != option.entryId)
                        continue;
                    if (definition.requiresCandidateId && toolCall.arguments.at("candidate_id") != option.candidateId)
                        continue;
                    optionMatched = true;
                    action.questId = option.questId;
                    break;
                }
                bool explicitMock = IsAmigoLlmMockEnabled() && GetAmigoLlmMockControlTool() != "auto";
                if (!optionMatched && (!explicitMock || definition.capability == ControlAction::Capability::EnterAttackPull ||
                    definition.capability == ControlAction::Capability::EnterGrind))
                {
                    LogControlToolRejected(toolCall.name, "action_not_in_server_options");
                    stateRef->controlState.store(LlmBotState::ControlState::Idle);
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs());
                    clearBusy();
                    return;
                }

                if (definition.capability == ControlAction::Capability::Idle)
                {
                    accepted = true;
                    gateReason = "no_action";
                }
                else if (definition.capability == ControlAction::Capability::MoveHop)
                {
                    uint32 navEpoch = 0;
                    std::string candidateId;
                    if (!ParseMoveHopNavArguments(toolCall.arguments, navEpoch, candidateId))
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "in_grind");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.isMoving)
                    {
                        LogControlToolRejected(toolCall.name, "already_moving");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.travelActive)
                    {
                        LogControlToolRejected(toolCall.name, "travel_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.professionActive)
                    {
                        LogControlToolRejected(toolCall.name, "profession_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (navEpoch != snapshot.navEpoch)
                    {
                        LogControlToolRejected(toolCall.name, "stale_nav_epoch");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    size_t candidateIndex = 0;
                    if (!TryParseNavCandidateIndex(candidateId, candidateIndex) ||
                        candidateIndex >= snapshot.navCandidates.size())
                    {
                        LogControlToolRejected(toolCall.name, "unknown_candidate");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    auto const &cand = snapshot.navCandidates[candidateIndex];
                    if (!cand.canMove)
                    {
                        LogControlToolRejected(toolCall.name, "cannot_move");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (!cand.reachable)
                    {
                        LogControlToolRejected(toolCall.name, "unreachable");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    action.navEpoch = navEpoch;
                    action.navCandidateId = candidateId;
                    accepted = true;
                    gateReason = "out_of_combat";
                }
                else if (definition.capability == ControlAction::Capability::MoveHopNpc)
                {
                    uint32 entryId = 0;
                    if (!ParseEntryIdArguments(toolCall.arguments, entryId))
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "in_grind");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.isMoving)
                    {
                        LogControlToolRejected(toolCall.name, "already_moving");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.travelActive)
                    {
                        LogControlToolRejected(toolCall.name, "travel_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.professionActive)
                    {
                        LogControlToolRejected(toolCall.name, "profession_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    bool found = false;
                    bool actionable = false;
                    for (auto const& e : snapshot.nearbyEntities)
                    {
                        if (e.type == "npc" && e.entryId == entryId)
                        {
                            found = true;
                            actionable = e.isQuestGiver || e.isVendor || e.isTrainer || e.isRepair || e.isFlightMaster;
                            break;
                        }
                    }
                    if (!found)
                    {
                        for (auto const& g : snapshot.questGiversInRange)
                        {
                            if (g.entryId == entryId)
                            {
                                found = true;
                                actionable = true; // quest giver is always actionable
                                break;
                            }
                        }
                    }
                    if (!found)
                    {
                        LogControlToolRejected(toolCall.name, "npc_not_nearby");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (!actionable)
                    {
                        LogControlToolRejected(toolCall.name, "npc_not_actionable");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    action.npcEntryId = entryId;
                    accepted = true;
                    gateReason = "nearby_npc";
                }
                else if (definition.capability == ControlAction::Capability::EnterAttackPull)
                {
                    if (!HasRemainingMissionObjective(snapshot))
                    {
                        LogControlToolRejected(toolCall.name, "mission_objective_complete");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    uint32 entryId = 0;
                    if (!ParseEntryIdArguments(toolCall.arguments, entryId))
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    bool foundExactTarget = false;
                    for (auto const& entity : snapshot.nearbyEntities)
                    {
                        if (entity.entryId == entryId && snapshot.mission.MatchesTargetName(entity.name))
                        {
                            foundExactTarget = true;
                            break;
                        }
                    }
                    if (!foundExactTarget)
                    {
                        LogControlToolRejected(toolCall.name, "mission_target_mismatch");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    action.npcEntryId = entryId;
                    accepted = true;
                    gateReason = "exact_mission_target";
                }
                else if (definition.capability == ControlAction::Capability::GatherTarget)
                {
                    uint32 entryId = 0;
                    if (!ParseEntryIdArguments(toolCall.arguments, entryId))
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    bool foundExactTarget = false;
                    for (auto const& entity : snapshot.nearbyEntities)
                    {
                        if (entity.type == "game_object" && entity.entryId == entryId &&
                            snapshot.mission.MatchesTargetName(entity.name))
                        {
                            foundExactTarget = true;
                            break;
                        }
                    }
                    if (!foundExactTarget)
                    {
                        LogControlToolRejected(toolCall.name, "mission_target_mismatch");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    action.gameObjectEntryId = entryId;
                    accepted = true;
                    gateReason = "exact_gather_target";
                }
                else if (definition.capability == ControlAction::Capability::EnterGrind)
                {
                    if (!HasRemainingMissionObjective(snapshot))
                    {
                        LogControlToolRejected(toolCall.name, "quest_objective_does_not_allow_grind");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "already_grinding");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "enter_grind";
                }
                else if (definition.capability == ControlAction::Capability::StopGrind)
                {
                    if (!snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "not_grinding");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "stop_grind";
                }
                else if (definition.capability == ControlAction::Capability::Stay)
                {
                    if (isStopped)
                    {
                        LogControlToolRejected(toolCall.name, "already_stopped");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "stay";
                }
                else if (definition.capability == ControlAction::Capability::Unstay)
                {
                    if (!isStopped)
                    {
                        LogControlToolRejected(toolCall.name, "not_stopped");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "unstay";
                }
                else if (definition.capability == ControlAction::Capability::TalkToQuestGiver)
                {
                    uint32 questId = 0;
                    if (!ParseQuestIdArguments(toolCall.arguments, questId))
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (!HasQuestGiverForQuestId(snapshot, questId))
                    {
                        LogControlToolRejected(toolCall.name, "quest_giver_not_in_range");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    action.questId = questId;
                    accepted = true;
                    gateReason = "quest_giver_in_range";
                }
                else if (definition.capability == ControlAction::Capability::VendorSell ||
                         definition.capability == ControlAction::Capability::VendorBuyUseful ||
                         definition.capability == ControlAction::Capability::Repair ||
                         definition.capability == ControlAction::Capability::Trainer ||
                         definition.capability == ControlAction::Capability::Taxi ||
                         definition.capability == ControlAction::Capability::Hearthstone ||
                         definition.capability == ControlAction::Capability::Loot ||
                         definition.capability == ControlAction::Capability::Food ||
                         definition.capability == ControlAction::Capability::Drink ||
                         definition.capability == ControlAction::Capability::Maintenance)
                {
                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (snapshot.isMoving && !definition.lifecycleOwned)
                    {
                        LogControlToolRejected(toolCall.name, "already_moving");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (definition.requiresNearbyTarget)
                    {
                        bool matchingNpc = false;
                        for (auto const& entity : snapshot.nearbyEntities)
                        {
                            if (entity.type == "npc" && entity.isFlightMaster)
                            {
                                matchingNpc = true;
                                break;
                            }
                        }
                        if (!matchingNpc)
                        {
                            LogControlToolRejected(toolCall.name, "required_service_npc_not_nearby");
                            stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                            stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                            clearBusy();
                            return;
                        }
                    }

                    if (definition.lifecycleOwned)
                    {
                        auto service = snapshot.serviceAvailability.find(toolCall.name);
                        if (service == snapshot.serviceAvailability.end() ||
                            (!service->second.local && !service->second.travel))
                        {
                            LogControlToolRejected(toolCall.name, service == snapshot.serviceAvailability.end() ?
                                "service_destination_unavailable" : service->second.reason);
                            stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                            stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs());
                            clearBusy();
                            return;
                        }
                    }
                    accepted = true;
                    gateReason = definition.lifecycleOwned ? "lifecycle_service_travel" : "native_playerbots_service";
                }
                else if (definition.capability == ControlAction::Capability::Fish)
                {
                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "in_grind");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.isMoving)
                    {
                        LogControlToolRejected(toolCall.name, "already_moving");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.travelActive)
                    {
                        LogControlToolRejected(toolCall.name, "travel_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.professionActive)
                    {
                        LogControlToolRejected(toolCall.name, "profession_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    // Respect memory cooldowns to avoid spamming fishing attempts.
                    if (BotMemory *mem = BotMemoryRegistry::Get(guid))
                    {
                        FailureStats stats = mem->GetFailureStats("profession:fishing", getMSTime());
                        if (stats.CooldownRemainingMs(getMSTime()) > 0)
                        {
                            LogControlToolRejected(toolCall.name, "cooldown");
                            stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                            stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                            clearBusy();
                            return;
                        }
                        accepted = true;
                        gateReason = "out_of_combat";
                    }
                }
                else if (definition.capability == ControlAction::Capability::UseProfession)
                {
                    std::string skill;
                    std::string intent;
                    if (!ParseProfessionArguments(toolCall.arguments, skill, intent) || skill != "fishing" || intent != "fish")
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    action.professionSkill = skill;
                    action.professionIntent = intent;
                    accepted = true;
                    gateReason = "profession_request";
                }
                else if (definition.capability == ControlAction::Capability::TurnLeft90 ||
                         definition.capability == ControlAction::Capability::TurnRight90 ||
                         definition.capability == ControlAction::Capability::TurnAround)
                {
                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "in_grind");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.isMoving)
                    {
                        LogControlToolRejected(toolCall.name, "already_moving");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.travelActive)
                    {
                        LogControlToolRejected(toolCall.name, "travel_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.professionActive)
                    {
                        LogControlToolRejected(toolCall.name, "profession_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    accepted = true;
                    gateReason = "turn";
                }

                if (accepted)
                {
                    actionState.action = action;
                    actionState.reasoning = "";
                    hasAction = true;
                    stateRef->loggedControlParseError.store(false);
                    stateRef->lastControlCapability.store(
                        static_cast<uint8>(action.capability), std::memory_order_relaxed);
                    LogControlToolAccepted(toolCall.name, action.capability, gateReason);

                    // Successful parse/accept: reset backoff.
                    uint32 nowMs = getMSTime();
                    stateRef->ollamaCooldownMs.store(kOllamaBaseCooldownMs, std::memory_order_relaxed);
                    if (action.capability == ControlAction::Capability::EnterGrind)
                    {
                        stateRef->nextAllowedAttemptMs.store(nowMs + kPostEnterGrindControlDelayMs, std::memory_order_relaxed);
                        stateRef->controlState.store(LlmBotState::ControlState::Cooldown, std::memory_order_relaxed);
                    }
                    else
                    {
                        stateRef->nextAllowedAttemptMs.store(0, std::memory_order_relaxed);
                    }
                    stateRef->nextPlannerShortTickMs.store(nowMs + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                }

                if (hasAction && actionState.action.capability != ControlAction::Capability::Idle)
                {
                    {
                        std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
                        BotLLMContext &ctx = GetBotLLMContext()[guid];
                        ctx.lastControlSummary = SummarizeControlAction(actionState.action);
                        ctx.lastControlAtMs = GetNowMs();
                    }
                    if (shortTermGoalCount > 0 &&
                        actionState.action.capability != ControlAction::Capability::MoveHop &&
                        actionState.action.capability != ControlAction::Capability::MoveHopNpc)
                    {
                        size_t currentIndex = stateRef->shortTermIndex.load(std::memory_order_relaxed);
                        size_t nextIndex = (currentIndex + 1) % shortTermGoalCount;
                        stateRef->shortTermIndex.store(nextIndex, std::memory_order_relaxed);
                    }
                    if (BotMissionRegistry::Instance().RevisionMatches(guid, actionState.missionRevision) &&
                        GetBotLifecycleGeneration(guid) == actionState.lifecycleGeneration)
                    {
                        ControlActionRegistry::Instance().Enqueue(guid, actionState);
                    }
                    else
                    {
                        LOG_INFO("server.loading", "[OllamaBotAmigo] Discarded stale control result for '{}' (mission revision {}, lifecycle generation {})", botName, actionState.missionRevision, actionState.lifecycleGeneration);
                    }
                }
                if (!hasAction)
                {
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                }

                // Clear busy ONLY here (response thread).
                stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                clearBusy();
            }))
            {
                stateRef->controlBusy.store(false, std::memory_order_release);
                stateRef->promptInFlight.store(false, std::memory_order_relaxed);
                stateRef->controlState.store(LlmBotState::ControlState::FailureHold, std::memory_order_relaxed);
                stateRef->nextAllowedAttemptMs.store(getMSTime() + kOllamaBaseCooldownMs, std::memory_order_relaxed);
                LOG_INFO("server.loading", "[OllamaBotAmigo] Control request dropped for '{}' because the bounded LLM queue is full.", botName);
            }
        }
    }

    if (g_OllamaBotControlClearGoalsOnConfigLoad)
    {
        {
            std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
            auto &ctxMap = GetBotLLMContext();
            for (auto &entry : ctxMap)
            {
                BotLLMContext &ctx = entry.second;
                ctx.longTermGoal.clear();
                ctx.shortTermGoals.clear();
                ctx.shortTermIndex = 0;
                ctx.hasActivePlan = false;
                ctx.lastPlanTimeMs = 0;
                ctx.controlStepsForCurrentGoal = 0;
            }
        }
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pendingStrategicUpdates.clear();
        }
        g_OllamaBotControlClearGoalsOnConfigLoad = false;
    }
}
