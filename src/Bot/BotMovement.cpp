#include "Bot/BotMovement.h"
#include "Bot/BotTaskProgress.h"

#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "LastMovementValue.h"
#include "Timer.h"
#include "Util/WorldPositionCompat.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
    // Rate-limit MovePoint calls to avoid spamming and re-entrancy.
    // With the additional `bot_->isMoving()` gate, this mostly controls how quickly we can
    // enqueue the *next* point after the previous point finishes.
    constexpr uint32 kMinMovePointIntervalMs = 150;

    // Consider destination reached when within this radius (2D).
    constexpr float kReachedEpsilon = 1.0f;

    float Dist2D(float ax, float ay, float bx, float by)
    {
        float dx = ax - bx;
        float dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    struct RegistryState
    {
        std::mutex mutex;
        std::unordered_map<uint64, BotMovement*> byGuid;
    };

    RegistryState& GetRegistry()
    {
        static RegistryState state;
        return state;
    }
} // namespace

bool BotMovement::StartPathMove(Player* bot, PlayerbotAI* ai, WorldPosition const& dest, MoveReason reason)
{
    if (!bot || !ai)
        return false;

    // If already active, allow higher-priority moves to interrupt lower-priority.
    if (active_)
    {
        // Simple priority: Combat/Flee/Script override Travel.
        if (reason_ == MoveReason::Travel && reason != MoveReason::Travel)
        {
            Abort(reason);
        }
        else
        {
            return false;
        }
    }

    bot_ = bot;
    ai_ = ai;
    reason_ = reason;
    interrupted_ = false;

    // Playerbots WorldPosition accessors are not const-correct.
    // Make a local copy before calling getMapId().
    WorldPosition destCopy = dest;

    destMapId_ = destCopy.GetMapId();
    destX_ = destCopy.GetPositionX();
    destY_ = destCopy.GetPositionY();
    destZ_ = destCopy.GetPositionZ();

    if (!BuildPath(dest))
    {
        bot_ = nullptr;
        ai_ = nullptr;
        return false;
    }

    active_ = true;
    lastMoveElapsedMs_ = kMinMovePointIntervalMs; // allow immediate first step
    return true;
}

void BotMovement::Update(uint32 diff)
{
    if (!active_ || !bot_)
        return;

    lastMoveElapsedMs_ += diff;

    if (issuedPoint_)
    {
        float dx = bot_->GetPositionX() - issuedX_;
        float dy = bot_->GetPositionY() - issuedY_;
        float dz = bot_->GetPositionZ() - issuedZ_;
        bool reachedPoint = std::sqrt(dx * dx + dy * dy + dz * dz) <= 0.8f;
        auto* motion = bot_->GetMotionMaster();
        bool sameGenerator = motion->top() == issuedGenerator_;
        if (AmigoPointInterrupted(true, sameGenerator, bot_->movespline->GetId() == splineId_,
            bot_->movespline->Finalized(), reachedPoint,
            motion->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE))
        {
            interrupted_ = true;
            // Relinquish ownership before cancellation; preserve the replacement.
            issuedPoint_ = false;
            Abort(reason_);
            return;
        }
        if (sameGenerator)
            splineId_ = bot_->movespline->GetId();
        if (!sameGenerator && reachedPoint)
            issuedPoint_ = false;
    }

    if (ShouldAbort())
    {
        Abort(reason_);
        return;
    }

    if (ReachedDestination())
    {
        active_ = false;
        path_.clear();
        return;
    }

    // Don't overwrite an in-flight point movement.
    if (bot_->isMoving())
        return;

    if (lastMoveElapsedMs_ < kMinMovePointIntervalMs)
        return;

    Advance();
    lastMoveElapsedMs_ = 0;
}

void BotMovement::Abort(MoveReason /*reason*/)
{
    if (!bot_)
    {
        active_ = false;
        path_.clear();
        return;
    }

    if (AmigoOwnsPoint(issuedPoint_, splineId_, bot_->movespline->GetId(),
        bot_->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE))
    {
        bot_->GetMotionMaster()->Clear(false);
        bot_->GetMotionMaster()->MoveIdle();
    }
    issuedPoint_ = false;
    active_ = false;
    path_.clear();
}

bool BotMovement::ConsumeInterruption()
{
    bool interrupted = interrupted_;
    interrupted_ = false;
    return interrupted;
}

bool BotMovement::BuildPath(WorldPosition const& dest)
{
    if (!bot_)
        return false;

    // Playerbots WorldPosition accessors are not const-correct.
    // Make a local copy before calling any accessors.
    WorldPosition destCopy = dest;

    // Enforce same-map pathing only; cross-map movement is not supported here.
    if (bot_->GetMapId() != destCopy.GetMapId())
        return false;

    PathGenerator pathGen(bot_);
    // Playerbots explicitly disables straight-line shortcuts
    pathGen.SetUseStraightPath(false);

    if (!pathGen.CalculatePath(destCopy.GetPositionX(),
                               destCopy.GetPositionY(),
                               destCopy.GetPositionZ()))
        return false;

    if (pathGen.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT))
        return false;
    path_ = pathGen.GetPath();
    if (path_.empty())
        return false;
    G3D::Vector3 end(destX_, destY_, destZ_);
    return (path_.back() - end).length() <= 1.5f;
}

float BotMovement::RemainingRoute() const
{
    if (!bot_)
        return 0.0f;

    // The native spline advances even when a route bends away from the goal.
    // Report remaining route time in seconds, not straight-line distance.
    if (issuedPoint_ && bot_->GetMotionMaster()->top() == issuedGenerator_)
        return std::max(0, bot_->movespline->Duration() - bot_->movespline->timePassed()) / 1000.0f;
    return 0.0f;
}

void BotMovement::Advance()
{
    if (path_.empty())
        return;
    // The reverted Playerbots API does not expose MoveToPosition(). Use the
    // core movement API after validating the route above.
    if (!bot_ || bot_->GetMapId() != destMapId_)
        return;

    bot_->GetMotionMaster()->MovePoint(0, destX_, destY_, destZ_);

    splineId_ = bot_->movespline->GetId();
    issuedGenerator_ = bot_->GetMotionMaster()->top();
    issuedX_ = destX_;
    issuedY_ = destY_;
    issuedZ_ = destZ_;
    issuedPoint_ = true;
}

bool BotMovement::ShouldAbort() const
{
    if (!bot_ || !bot_->IsAlive() || !bot_->IsInWorld())
        return true;

    // Travel is interrupted by combat.
    if (bot_->IsInCombat() && reason_ == MoveReason::Travel)
        return true;

    return false;
}

bool BotMovement::ReachedDestination() const
{
    if (!bot_)
        return true;

    float d2 = Dist2D(bot_->GetPositionX(), bot_->GetPositionY(), destX_, destY_);
    float dz = std::fabs(bot_->GetPositionZ() - destZ_);
    return d2 <= kReachedEpsilon && dz <= 1.5f;
}

void BotMovementRegistry::Register(uint64 guid, BotMovement* movement)
{
    auto& reg = GetRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.byGuid[guid] = movement;
}

void BotMovementRegistry::Unregister(uint64 guid)
{
    auto& reg = GetRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.byGuid.erase(guid);
}

BotMovement* BotMovementRegistry::Get(uint64 guid)
{
    auto& reg = GetRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    auto it = reg.byGuid.find(guid);
    return (it == reg.byGuid.end()) ? nullptr : it->second;
}
