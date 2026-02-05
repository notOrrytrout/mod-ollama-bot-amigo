#pragma once

#include "Define.h"
#include "PathGenerator.h" // Movement::PointsArray

#include <cstdint>
#include <unordered_map>
#include <vector>

class Player;
class WorldPosition;

enum class MoveReason
{
    Travel,
    Combat,
    Flee,
    Script,
};

// Stateful, tick-driven path movement wrapper.
//
// HARD RULES:
// - Only this unit may call MotionMaster/MovePoint for the bot.
// - Uses TrinityCore PathGenerator; no manual Z interpolation.
// - Long/multi-floor movement must be path-based.
class BotMovement
{
public:
    bool StartPathMove(Player* bot, WorldPosition const& dest, MoveReason reason);

    // Called every server tick.
    void Update(uint32 diff);

    // Stops any active movement, regardless of the abort reason.
    void Abort(MoveReason reason);

    bool IsMoving() const { return active_; }

private:
    bool BuildPath(WorldPosition const& dest);
    void Advance(float maxDist);
    bool ShouldAbort() const;
    bool ReachedDestination() const;

private:
    Player* bot_ = nullptr;
    Movement::PointsArray path_;
    MoveReason reason_ = MoveReason::Travel;

    bool active_ = false;
    uint32 lastMoveElapsedMs_ = 0;

    // Destination cache (avoid storing WorldPosition by value in header).
    uint32 destMapId_ = 0;
    float destX_ = 0.0f;
    float destY_ = 0.0f;
    float destZ_ = 0.0f;
};

// Small registry that allows other server scripts (controller/loop) to locate
// the BotMovement instance associated with a bot GUID without granting them any
// movement execution privileges.
class BotMovementRegistry
{
public:
    static void Register(uint64 guid, BotMovement* movement);
    static void Unregister(uint64 guid);
    static BotMovement* Get(uint64 guid);
};
