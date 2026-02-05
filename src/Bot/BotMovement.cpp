#include "Bot/BotMovement.h"

#include "Log.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "Player.h"
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

    // Movement stepping tunables:
    // - kMinAdvanceDist ensures we don't pick micro-waypoints when the path is dense.
    // - kMaxTurnAngleDeg prevents skipping around corners (which can cut into obstacles).
    constexpr float kMinAdvanceDist = 6.0f;       // yards
    constexpr float kMaxAdvanceDistFloor = 10.0f; // yards
    constexpr float kMaxAdvanceDistCeil = 24.0f;  // yards
    constexpr float kMaxTurnAngleDeg = 30.0f;     // degrees
    constexpr float kSkipClosePointEps = 0.8f;    // yards

    float Dist2D(float ax, float ay, float bx, float by)
    {
        float dx = ax - bx;
        float dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    float ClampDot(float v)
    {
        if (v < -1.0f)
            return -1.0f;
        if (v > 1.0f)
            return 1.0f;
        return v;
    }


float Clamp(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

    float AngleBetween(const G3D::Vector3& a, const G3D::Vector3& b)
    {
        float la = a.length();
        float lb = b.length();
        if (la <= 1e-4f || lb <= 1e-4f)
            return 0.0f;

        float dot = ClampDot(a.dot(b) / (la * lb));
        return std::acos(dot); // radians
    }

    float ComputeMaxAdvanceDist(Player* bot)
    {
        // Aim for ~2.25s worth of travel per step, clamped.
        // `GetSpeed(MOVE_RUN)` is in yards/sec on TrinityCore.
        float speed = bot ? bot->GetSpeed(MOVE_RUN) : 7.0f;
        float dist = speed * 2.25f;
        return Clamp(dist, kMaxAdvanceDistFloor, kMaxAdvanceDistCeil);
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

bool BotMovement::StartPathMove(Player* bot, WorldPosition const& dest, MoveReason reason)
{
    if (!bot)
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
    reason_ = reason;

    // Playerbots WorldPosition accessors are not const-correct.
    // Make a local copy before calling getMapId().
    WorldPosition destCopy = dest;

    destMapId_ = destCopy.getMapId();
    destX_ = destCopy.getX();
    destY_ = destCopy.getY();
    destZ_ = destCopy.getZ();

    if (!BuildPath(dest))
    {
        bot_ = nullptr;
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

    Advance(ComputeMaxAdvanceDist(bot_));
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

    // We do not call Movement generators here; we only stop our own stepping.
    // MotionMaster may continue existing movement (combat, follow, etc.).
    active_ = false;
    path_.clear();
}

bool BotMovement::BuildPath(WorldPosition const& dest)
{
    if (!bot_)
        return false;

    // Playerbots WorldPosition accessors are not const-correct.
    // Make a local copy before calling any accessors.
    WorldPosition destCopy = dest;

    // Enforce same-map pathing only; cross-map movement is not supported here.
    if (bot_->GetMapId() != destCopy.getMapId())
        return false;

    PathGenerator pathGen(bot_);
    // Playerbots explicitly disables straight-line shortcuts
    pathGen.SetUseStraightPath(false);

    if (!pathGen.CalculatePath(destCopy.getX(),
                               destCopy.getY(),
                               destCopy.getZ()))
        return false;

    path_ = pathGen.GetPath();
    return !path_.empty();
}

void BotMovement::Advance(float maxDist)
{
    if (!bot_)
        return;

    if (path_.empty())
    {
        active_ = false;
        return;
    }

    // Choose a farther waypoint along the path, without skipping around corners.
    // NOTE: We still call MovePoint (straight-line) to the chosen waypoint,
    // so we avoid selecting a point past a significant turn.
    const G3D::Vector3 cur(bot_->GetPositionX(), bot_->GetPositionY(), bot_->GetPositionZ());

    float traveled = 0.0f;
    size_t targetIdx = 0;

    const float maxTurnRad = kMaxTurnAngleDeg * 3.14159265f / 180.0f;

    for (size_t i = 0; i < path_.size(); ++i)
    {
        const G3D::Vector3 prev = (i == 0) ? cur : path_[i - 1];
        const G3D::Vector3 here = path_[i];

        float seg = (here - prev).length();
        if (seg < kSkipClosePointEps)
        {
            // Dense path point; treat as consumed for targeting.
            targetIdx = i;
            continue;
        }

        // If taking this segment would exceed maxDist, stop once we have a reasonable step.
        if ((traveled + seg) > maxDist && traveled >= kMinAdvanceDist)
            break;

        traveled += seg;
        targetIdx = i;

        // Stop at corners once we have moved a bit.
        if (i + 1 < path_.size() && traveled >= kMinAdvanceDist)
        {
            const G3D::Vector3 next = path_[i + 1];
            const G3D::Vector3 v1 = here - prev;
            const G3D::Vector3 v2 = next - here;
            if (AngleBetween(v1, v2) > maxTurnRad)
                break;
        }

        if (traveled >= maxDist)
            break;
    }

    const auto target = path_[targetIdx];
    // Consume waypoints up to and including the target (we will walk straight to it).
    path_.erase(path_.begin(), path_.begin() + targetIdx + 1);

    bot_->GetMotionMaster()->MovePoint(0, target.x, target.y, target.z);
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

    // If we've consumed the path, consider it reached once close in 2D.
    if (path_.empty())
    {
        float d2 = Dist2D(bot_->GetPositionX(), bot_->GetPositionY(), destX_, destY_);
        return d2 <= kReachedEpsilon;
    }

    return false;
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
