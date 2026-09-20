#include "Bot/BotTravel.h"

#include "Player.h"
#include "Log.h"
#include "Bot/BotMission.h"
#include "Bot/BotMovement.h"

std::mutex BotTravelRegistry::mutex_;
std::unordered_map<uint64_t, BotTravel*> BotTravelRegistry::travelByGuid_;

void BotTravel::Begin(AmigoTravelTarget const& target, uint32_t nowMs)
{
    target_ = target;
    active_ = true;
    startMs_ = nowMs;
    lastChangeMs_ = nowMs;
    lastResult_ = TravelResult::None;
}

void BotTravel::Abort(uint32_t nowMs)
{
    if (!active_)
        return;
    active_ = false;
    lastResult_ = TravelResult::Aborted;
    lastChangeMs_ = nowMs;
}

void BotTravel::Clear()
{
    active_ = false;
    target_.reset();
    lastResult_ = TravelResult::None;
    startMs_ = 0;
    lastChangeMs_ = 0;
}

bool BotTravel::Reached(Player* bot) const
{
    if (!bot || !target_)
        return false;

    WorldPosition cur(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    // Playerbots WorldPosition distance is map-aware. Use that.
    float d = cur.distance(target_->dest);
    return d <= target_->radius;
}

void BotTravel::Update(Player* bot, uint32_t nowMs)
{
    if (!active_ || !bot || !target_)
        return;

    auto finish = [&](TravelResult result, char const* reason)
    {
        active_ = false;
        lastResult_ = result;
        lastChangeMs_ = nowMs;
        if (auto* movement = BotMovementRegistry::Get(bot->GetGUID().GetRawValue()))
            movement->Abort(MoveReason::Travel);
        bot->StopMoving();
        LOG_INFO("server.loading", "[OllamaBotAmigo] Travel completion for {}: key={} result={}",
                 bot->GetName(), target_->key, reason);
    };
    if ((target_->missionRevision && !BotMissionRegistry::Instance().RevisionMatches(
            bot->GetGUID().GetRawValue(), target_->missionRevision)) ||
        (target_->turnInQuestId && bot->GetQuestStatus(target_->turnInQuestId) != QUEST_STATUS_COMPLETE))
    {
        finish(TravelResult::Aborted, "quest_or_mission_invalid");
        return;
    }

    if (!bot->IsAlive())
    {
        finish(TravelResult::Aborted, "bot_dead");
        return;
    }

    if (Reached(bot))
    {
        finish(TravelResult::Reached, "arrival_verified");
        return;
    }

    if (nowMs - startMs_ > target_->timeoutMs)
    {
        finish(TravelResult::TimedOut, "travel_timeout");
        return;
    }
}

void BotTravelRegistry::Register(uint64_t guid, BotTravel* travel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    travelByGuid_[guid] = travel;
}

void BotTravelRegistry::Unregister(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    travelByGuid_.erase(guid);
}

BotTravel* BotTravelRegistry::Get(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = travelByGuid_.find(guid);
    return it == travelByGuid_.end() ? nullptr : it->second;
}
