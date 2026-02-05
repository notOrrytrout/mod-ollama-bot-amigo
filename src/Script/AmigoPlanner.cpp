#include "Script/AmigoPlanner.h"
#include "Bot/BotControlApi.h"
#include "Script/OllamaBotConfig.h"
#include "Ai/OllamaRuntime.h"
#include "Log.h"
#include "Util/PlayerbotsCompat.h"
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

void AmigoPlannerRegistry::Enqueue(Player* bot, const AmigoPlannerState& plan)
{
    // Queue a plan using a Player pointer.
    if (!bot)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    plans_[bot->GetGUID().GetRawValue()].push_back(plan);
}

void AmigoPlannerRegistry::Enqueue(uint64 botGuid, const AmigoPlannerState& plan)
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

    const bool applied = HandleBotControlCommandTracked(player, plan.command);
    if (applied && plan.command.type == BotControlCommandType::PlayerbotCommand &&
        !plan.command.args.empty())
    {
        // Update activity state for planner/control context.
        const std::string& commandText = plan.command.args[0];
        if (commandText == "grind")
        {
            UpdateActivityState(player, "grind", plan.reasoning);
        }
        else if (commandText == "follow" || commandText == "stay")
        {
            UpdateActivityState(player, "", plan.reasoning);
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

    if (!g_OllamaBotControlBotName.empty() && player->GetName() != g_OllamaBotControlBotName)
    {
        return;
    }

    ai->ResetStrategies();

    if (g_EnableOllamaBotAmigoDebug)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Reset bot strategies on login for {}", player->GetName());
    }
}
