#include "Script/AmigoPlanner.h"
#include "Bot/BotControlApi.h"
#include "Bot/BotMission.h"
#include "Bot/BotNeeds.h"
#include "Bot/BotLifecycle.h"
#include "Script/OllamaBotConfig.h"
#include "Ai/OllamaRuntime.h"
#include "Script/OllamaBotControlLoop.h"
#include "Log.h"
#include "Util/PlayerbotsCompat.h"
#include "Util/AmigoBotNames.h"
#include "Timer.h"

namespace
{
    // Prevent planner commands from firing too frequently.
    constexpr uint32 kMinPlannerIntervalMs = 900;
    std::unordered_map<uint64, uint32> lastAppliedMs;
    std::mutex lastAppliedMutex;
}

AmigoPlannerRegistry& AmigoPlannerRegistry::Instance()
{
    // Shared planner queue registry.
    static AmigoPlannerRegistry instance;
    return instance;
}

void AmigoPlannerRegistry::Enqueue(Player* bot, AmigoPlannerState const& plan)
{
    // Queue a plan using a Player pointer.
    if (!bot)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    plans_[bot->GetGUID().GetRawValue()].push_back(plan);
}

void AmigoPlannerRegistry::Enqueue(uint64 botGuid, AmigoPlannerState const& plan)
{
    // Queue a plan using a raw GUID.
    if (!botGuid)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    plans_[botGuid].push_back(plan);
}

bool AmigoPlannerRegistry::TryDequeue(uint64 botGuid, AmigoPlannerState& out)
{
    // Pop the next plan for a bot (FIFO).
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = plans_.find(botGuid);
    if (it == plans_.end() || it->second.empty())
    {
        return false;
    }

    out = it->second.front();
    it->second.pop_front();
    return true;
}

void AmigoPlannerRegistry::Clear(uint64 botGuid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    plans_.erase(botGuid);
}

AmigoPlannerApplierScript::AmigoPlannerApplierScript()
    : PlayerScript("AmigoPlannerApplierScript")
{
}

void AmigoPlannerApplierScript::OnPlayerAfterUpdate(Player* player, uint32 /*diff*/)
{
    // Apply the next queued plan and update activity tracking.
    if (!player || !player->IsInWorld())
    {
        return;
    }

    PollPendingStrategyLogs(player);

    const uint64 botGuid = player->GetGUID().GetRawValue();
    const uint32 nowMs = getMSTime();

    {
        std::lock_guard<std::mutex> lock(lastAppliedMutex);
        auto it = lastAppliedMs.find(botGuid);
        if (it != lastAppliedMs.end() && nowMs - it->second < kMinPlannerIntervalMs)
        {
            return;
        }
    }

    AmigoPlannerState plan;
    if (!AmigoPlannerRegistry::Instance().TryDequeue(botGuid, plan))
    {
        return;
    }

    if (!BotMissionRegistry::Instance().RevisionMatches(botGuid, plan.missionRevision))
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Ignored queued Playerbot command for {} because mission revision {} is stale",
                 player->GetName(), plan.missionRevision);
        return;
    }

    if (GetBotLifecycleGeneration(botGuid) != plan.lifecycleGeneration)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Ignored queued Playerbot command for {} because lifecycle generation {} is stale",
                 player->GetName(), plan.lifecycleGeneration);
        return;
    }

    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(player);
    BotNeedAssessment queuedNeed = AssessBotNeeds(player);
    BotLifecycleAssessment queuedLifecycle = AssessBotLifecycle(player, ai, queuedNeed);
    if (queuedLifecycle.lane == BotLifecycleLane::Combat ||
        queuedLifecycle.lane == BotLifecycleLane::Needs ||
        queuedLifecycle.lane == BotLifecycleLane::Loot ||
        HasActiveBotLifecycleOverlay(player) ||
        (queuedLifecycle.lane == BotLifecycleLane::Maintenance && queuedLifecycle.urgent))
    {
        LOG_INFO("server.loading",
                 "[OllamaBotAmigo] Ignored queued Playerbot command for {} because lifecycle lane '{}' owns execution (maintenance='{}', reason='{}')",
                 player->GetName(),
                 queuedLifecycle.LaneName(),
                 queuedLifecycle.MaintenanceName(),
                 queuedLifecycle.reason);
        return;
    }

    const bool applied = HandleBotControlCommandTracked(player, plan.command);
    if (applied && !plan.command.args.empty())
    {
        // Update activity state for planner/control context. Native strategy changes
        // replace the old grind/follow/stay chat commands.
        if (plan.command.type == BotControlCommandType::PlayerbotStrategy)
        {
            bool entersGrind = false;
            bool leavesGrind = false;
            for (std::string const& spec : plan.command.args)
            {
                entersGrind = entersGrind || spec.find("+grind") != std::string::npos;
                leavesGrind = leavesGrind || spec.find("-grind") != std::string::npos;
            }
            if (entersGrind)
            {
                UpdateActivityState(player, "grind", plan.reasoning);
            }
            else if (leavesGrind)
            {
                UpdateActivityState(player, "", plan.reasoning);
            }
        }
        else if (plan.command.type == BotControlCommandType::PlayerbotCommand)
        {
            std::string const& commandText = plan.command.args[0];
            if (commandText == "grind")
            {
                UpdateActivityState(player, "grind", plan.reasoning);
            }
            else if (commandText == "follow" || commandText == "stay")
            {
                UpdateActivityState(player, "", plan.reasoning);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(lastAppliedMutex);
        lastAppliedMs[botGuid] = nowMs;
    }

}

AmigoBotLoginScript::AmigoBotLoginScript()
    : PlayerScript("AmigoBotLoginScript")
{
}

void AmigoBotLoginScript::OnPlayerLogin(Player* player)
{
    // Reset bot strategies when the module is enabled.
    if (!player || !g_OllamaBotRuntime.enable_control)
    {
        return;
    }

    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(player);
    if (!ai || !ai->IsBotAI())
    {
        return;
    }

    if (!IsAmigoBotNameAllowed(g_OllamaBotControlBotName, player->GetName()))
    {
        return;
    }

    ResetBotLifecycle(player);
    ai->ResetStrategies();

    if (g_EnableOllamaBotAmigoDebug)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Reset bot strategies on login for {}", player->GetName());
    }
}

void AmigoBotLoginScript::OnPlayerBeforeLogout(Player* player)
{
    RetireAmigoBotState(player);
}
