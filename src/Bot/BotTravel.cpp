#include "Bot/BotTravel.h"

#include "Player.h"
#include "Log.h"
#include "Bot/BotMission.h"
#include "Bot/BotMovement.h"
#include "Creature.h"
#include "ObjectAccessor.h"

#include <algorithm>
#include <cmath>

std::mutex BotTravelRegistry::mutex_;
std::unordered_map<uint64_t, BotTravel*> BotTravelRegistry::travelByGuid_;

bool BotTravel::Start(Player* bot, PlayerbotAI* ai, BotMovement* movement, AmigoTravelTarget const& target,
                      MoveReason reason, uint32_t nowMs)
{
    if (!bot || !ai || !movement || active_)
        return false;
    if (!bot->IsAlive() || bot->IsInCombat())
        return false;
    if (!movement->StartPathMove(bot, ai, target.dest, reason))
        return false;
    Begin(target, nowMs);
    if (!target_->missionRevision)
        target_->missionRevision = BotMissionRegistry::Instance().Get(bot->GetGUID().GetRawValue()).revision;
    movement_ = movement;
    progress_.Reset(movement->RemainingRoute(), nowMs);
    return true;
}

void BotTravel::Begin(AmigoTravelTarget const& target, uint32_t nowMs)
{
    target_ = target;
    active_ = true;
    startMs_ = nowMs;
    lastChangeMs_ = nowMs;
    lastResult_ = TravelResult::None;
}

void BotTravel::Abort(uint32_t nowMs, BotMovement* movement)
{
    if (!active_)
        return;
    if (!movement)
        movement = movement_;
    if (movement && movement->IsMoving())
        movement->Abort(MoveReason::Travel);
    active_ = false;
    lastResult_ = TravelResult::Aborted;
    lastChangeMs_ = nowMs;
}

void BotTravel::Clear()
{
    Abort(0, movement_);
    active_ = false;
    target_.reset();
    lastResult_ = TravelResult::None;
    startMs_ = 0;
    lastChangeMs_ = 0;
    movement_ = nullptr;
}

bool BotTravel::Reached(Player* bot) const
{
    if (!bot || !target_)
        return false;

    if (target_->completion == AmigoCompletionCondition::LiveTarget)
    {
        Creature* target = ObjectAccessor::GetCreature(*bot, ObjectGuid(target_->targetGuid));
        if (!target || !target->IsInWorld() || !target->IsAlive())
            return false;
        return bot->GetDistance(target) <= INTERACTION_DISTANCE && bot->IsWithinLOSInMap(target);
    }

    WorldPosition cur(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
    // Playerbots WorldPosition distance is map-aware. Use that.
    WorldPosition target = target_->dest;
    if (cur.GetMapId() != target.GetMapId())
        return false;

    float dx = cur.GetPositionX() - target.GetPositionX();
    float dy = cur.GetPositionY() - target.GetPositionY();
    float dz = cur.GetPositionZ() - target.GetPositionZ();
    float horizontal = std::sqrt(dx * dx + dy * dy);
    return horizontal <= target_->radius && std::fabs(dz) <= std::max(1.5f, target_->radius);
}

void BotTravel::Update(Player* bot, uint32_t nowMs, BotMovement* movement)
{
    if (!active_ || !bot || !target_)
        return;
    if (!movement)
        movement = movement_;

    auto finish = [&](TravelResult result, char const* reason)
    {
        active_ = false;
        lastResult_ = result;
        lastChangeMs_ = nowMs;
        bool ownsMovement = movement && movement->IsMoving();
        if (ownsMovement)
        {
            movement->Abort(MoveReason::Travel);

        }
        LOG_INFO("server.loading", "[OllamaBotAmigo] Travel completion for {}: key={} result={}",
                 bot->GetName(), target_->key, reason);
    };
    if (movement && movement->ConsumeInterruption())
    {
        finish(TravelResult::Aborted, "movement_owner_changed");
        return;
    }
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

    // Combat owns movement execution. End the semantic request at the same
    // tick that the movement wrapper stops it; a stale travel request must not
    // remain active until its timeout.
    if (bot->IsInCombat())
    {
        finish(TravelResult::Aborted, "combat_interruption");
        return;
    }

    if (target_->completion == AmigoCompletionCondition::LiveTarget)
    {
        Creature* live = ObjectAccessor::GetCreature(*bot, ObjectGuid(target_->targetGuid));
        if (!live || !live->IsAlive() || live->IsHostileTo(bot))
        {
            finish(TravelResult::Aborted, "live_target_invalid");
            return;
        }
        target_->dest = WorldPosition(live);
    }

    if (Reached(bot))
    {
        finish(TravelResult::Reached, "arrival_verified");
        return;
    }

    if (movement)
    {
        progress_.Observe(movement->RemainingRoute(), nowMs);
        if (progress_.Stalled(nowMs))
        {
            if (progress_.Exhausted())
            {
                finish(TravelResult::TimedOut, "movement_stalled");
                return;
            }
            movement->Abort(MoveReason::Travel);
            movement->StartPathMove(bot, movement->GetPlayerbotAI(), target_->dest, MoveReason::Travel);
            progress_.Retry(movement->RemainingRoute(), nowMs);
        }
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
