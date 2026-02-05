#include "Bot/BotTravel.h"

#include "Player.h"
#include "Log.h"

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

    if (!bot->IsAlive())
    {
        active_ = false;
        lastResult_ = TravelResult::Aborted;
        lastChangeMs_ = nowMs;
        return;
    }

    if (Reached(bot))
    {
        active_ = false;
        lastResult_ = TravelResult::Reached;
        lastChangeMs_ = nowMs;
        return;
    }

    if (nowMs - startMs_ > target_->timeoutMs)
    {
        active_ = false;
        lastResult_ = TravelResult::TimedOut;
        lastChangeMs_ = nowMs;
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
