#include "Bot/BotMovement.h"
#include "Bot/BotTaskProgress.h"

#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "LastMovementValue.h"
#include "ObjectAccessor.h"
#include "Timer.h"
#include "Util/PlayerbotsCompat.h"
#include "Util/WorldPositionCompat.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
    // Rate-limit MovePoint calls to avoid spamming and re-entrancy.
    // With the additional `bot->isMoving()` gate, this mostly controls how quickly we can
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

    botGuid_ = bot->GetGUID();
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
        botGuid_.Clear();
        return false;
    }

    active_ = true;
    lastMoveElapsedMs_ = kMinMovePointIntervalMs; // allow immediate first step
    return true;
}

void BotMovement::Update(uint32 diff)
{
    if (!active_)
        return;
    Player* bot = ResolveBot();
    if (!bot)
    {
        Abort(reason_);
        return;
    }

    lastMoveElapsedMs_ += diff;

    if (issuedPoint_)
    {
        float dx = bot->GetPositionX() - issuedX_;
        float dy = bot->GetPositionY() - issuedY_;
        float dz = bot->GetPositionZ() - issuedZ_;
        bool reachedPoint = std::sqrt(dx * dx + dy * dy + dz * dz) <= 0.8f;
        auto* motion = bot->GetMotionMaster();
        bool sameGenerator = motion->top() == issuedGenerator_;
        if (AmigoPointInterrupted(true, sameGenerator, bot->movespline->GetId() == splineId_,
            bot->movespline->Finalized(), reachedPoint,
            motion->GetCurrentMovementGeneratorType() == IDLE_MOTION_TYPE))
        {
            interrupted_ = true;
            // Relinquish ownership before cancellation; preserve the replacement.
            issuedPoint_ = false;
            Abort(reason_);
            return;
        }
        if (sameGenerator)
            splineId_ = bot->movespline->GetId();
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
        issuedPoint_ = false;
        path_.clear();
        return;
    }

    // Don't overwrite an in-flight point movement.
    if (bot->isMoving())
        return;

    if (lastMoveElapsedMs_ < kMinMovePointIntervalMs)
        return;

    Advance();
    lastMoveElapsedMs_ = 0;
}

void BotMovement::Abort(MoveReason /*reason*/)
{
    Player* bot = ResolveBot();

    if (bot && AmigoOwnsPoint(issuedPoint_, splineId_, bot->movespline->GetId(),
        bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE))
    {
        bot->GetMotionMaster()->Clear(false);
        bot->GetMotionMaster()->MoveIdle();
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
    Player* bot = ResolveBot();
    if (!bot)
        return false;

    // Playerbots WorldPosition accessors are not const-correct.
    // Make a local copy before calling any accessors.
    WorldPosition destCopy = dest;

    // Enforce same-map pathing only; cross-map movement is not supported here.
    if (bot->GetMapId() != destCopy.GetMapId())
        return false;

    PathGenerator pathGen(bot);
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
    Player* bot = ResolveBot();
    if (!bot)
        return 0.0f;

    // The native spline advances even when a route bends away from the goal.
    // Report remaining route time in seconds, not straight-line distance.
    if (issuedPoint_ && bot->GetMotionMaster()->top() == issuedGenerator_)
        return std::max(0, bot->movespline->Duration() - bot->movespline->timePassed()) / 1000.0f;
    return 0.0f;
}

void BotMovement::Advance()
{
    if (path_.empty())
        return;
    // The reverted Playerbots API does not expose MoveToPosition(). Use the
    // core movement API after validating the route above.
    Player* bot = ResolveBot();
    if (!bot || bot->GetMapId() != destMapId_)
        return;

    bot->GetMotionMaster()->MovePoint(0, destX_, destY_, destZ_);

    splineId_ = bot->movespline->GetId();
    issuedGenerator_ = bot->GetMotionMaster()->top();
    issuedX_ = destX_;
    issuedY_ = destY_;
    issuedZ_ = destZ_;
    issuedPoint_ = true;
}

bool BotMovement::ShouldAbort() const
{
    Player* bot = ResolveBot();
    if (!bot || !bot->IsAlive() || !bot->IsInWorld())
        return true;

    // Travel is interrupted by combat.
    if (bot->IsInCombat() && reason_ == MoveReason::Travel)
        return true;

    return false;
}

bool BotMovement::ReachedDestination() const
{
    Player* bot = ResolveBot();
    if (!bot)
        return true;

    float d2 = Dist2D(bot->GetPositionX(), bot->GetPositionY(), destX_, destY_);
    float dz = std::fabs(bot->GetPositionZ() - destZ_);
    return d2 <= kReachedEpsilon && dz <= 1.5f;
}

Player* BotMovement::ResolveBot() const
{
    return botGuid_.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(botGuid_);
}

PlayerbotAI* BotMovement::GetPlayerbotAI() const
{
    Player* bot = ResolveBot();
    return bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
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
