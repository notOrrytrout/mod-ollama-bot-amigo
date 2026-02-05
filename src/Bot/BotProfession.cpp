#include "Bot/BotProfession.h"

#include "Util/PlayerbotsCompat.h"

#include "Player.h"
#include "Log.h"

namespace
{
    // Playerbots action names (see ActionContext.h).
    constexpr char const* kActionGoFishing = "go fishing";
    constexpr char const* kActionUseBobber = "use fishing bobber";
    constexpr char const* kActionRemoveBobber = "remove bobber strategy";

    // Tick cadence for bobber checks.
    constexpr uint32_t kBobberPollIntervalMs = 1000;

    // Safety timeout for a fishing cycle (cast + wait for bite).
    // In practice, fishing bites are usually quicker; this prevents hanging.
    constexpr uint32_t kFishingTimeoutMs = 60000;
}

bool BotProfession::ShouldAbort(Player* bot) const
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive())
        return true;

    // We cannot reliably fish during combat.
    if (bot->IsInCombat())
        return true;

    // Avoid fishing while swimming.
    if (bot->isSwimming())
        return true;

    return false;
}

void BotProfession::ClearBobberStrategy(PlayerbotAI* ai)
{
    if (!ai)
        return;

    // Clear the "+use bobber" strategy toggled by FishingAction.
    ai->DoSpecificAction(kActionRemoveBobber, Event(), true);
}

bool BotProfession::StartFishing(Player* bot, PlayerbotAI* ai, uint32_t nowMs)
{
    if (active_ || !bot || !ai)
        return false;

    if (ShouldAbort(bot))
        return false;

    // Prime Playerbots' fishing spot value so the "go fishing" action is "useful".
    // We intentionally do *not* call Playerbots movement actions.
    if (AiObjectContext* ctx = ai->GetAiObjectContext())
    {
        if (auto* value = ctx->GetValue<WorldPosition>("fishing spot"))
        {
            value->Set(WorldPosition(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));
        }
    }

    // Start the fishing cast. This uses Playerbots FishingAction (no MotionMaster).
    if (!ai->DoSpecificAction(kActionGoFishing, Event(), true))
    {
        // Make sure any partial strategies are cleaned up.
        ClearBobberStrategy(ai);
        return false;
    }

    active_ = true;
    activity_ = ProfessionActivity::Fishing;
    lastResult_ = ProfessionResult::Started;
    startMs_ = nowMs;
    lastStepMs_ = nowMs;
    lastChangeMs_ = nowMs;

    LOG_INFO("server.loading", "[OllamaBotAmigo] Profession started: fishing");
    return true;
}

void BotProfession::Update(Player* bot, PlayerbotAI* ai, uint32_t nowMs)
{
    if (!active_)
        return;

    if (ShouldAbort(bot))
    {
        Abort(bot, ai, nowMs);
        return;
    }

    if (activity_ != ProfessionActivity::Fishing)
        return;

    if (nowMs - startMs_ > kFishingTimeoutMs)
    {
        ClearBobberStrategy(ai);
        active_ = false;
        lastResult_ = ProfessionResult::TimedOut;
        lastChangeMs_ = nowMs;
        return;
    }

    if (nowMs - lastStepMs_ < kBobberPollIntervalMs)
        return;

    lastStepMs_ = nowMs;

    // Attempt to use the bobber when it becomes ready.
    // Playerbots internally throttles checks based on bobber respawn time.
    if (ai && ai->DoSpecificAction(kActionUseBobber, Event(), true))
    {
        ClearBobberStrategy(ai);
        active_ = false;
        lastResult_ = ProfessionResult::Succeeded;
        lastChangeMs_ = nowMs;
    }
}

void BotProfession::Abort(Player* bot, PlayerbotAI* ai, uint32_t nowMs)
{
    if (!active_)
        return;

    ClearBobberStrategy(ai);
    active_ = false;
    activity_ = ProfessionActivity::None;
    lastResult_ = ProfessionResult::Aborted;
    lastChangeMs_ = nowMs;
}

std::mutex BotProfessionRegistry::mutex_;
std::unordered_map<uint64_t, BotProfession*> BotProfessionRegistry::byGuid_;

void BotProfessionRegistry::Register(uint64_t guid, BotProfession* prof)
{
    std::lock_guard<std::mutex> lock(mutex_);
    byGuid_[guid] = prof;
}

void BotProfessionRegistry::Unregister(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    byGuid_.erase(guid);
}

BotProfession* BotProfessionRegistry::Get(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byGuid_.find(guid);
    return it == byGuid_.end() ? nullptr : it->second;
}
