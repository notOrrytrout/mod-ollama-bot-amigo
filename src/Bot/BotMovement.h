#pragma once

#include "Define.h"
#include "ObjectGuid.h"
#include "PathGenerator.h" // Movement::PointsArray

#include <cstdint>
#include <unordered_map>
#include <vector>

class Player;
class PlayerbotAI;
class MovementGenerator;
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
// - Playerbots owns movement execution and cancellation.
// - This unit validates the destination and tracks the travel transaction.
// - Uses TrinityCore PathGenerator; no manual Z interpolation.
class BotMovement
{
public:
    bool StartPathMove(Player* bot, PlayerbotAI* ai, WorldPosition const& dest, MoveReason reason);

    // Called every server tick.
    void Update(uint32 diff);

    // Stops any active movement, regardless of the abort reason.
    void Abort(MoveReason reason);

    float RemainingRoute() const;
    bool IsMoving() const { return active_; }
    bool ConsumeInterruption();
    PlayerbotAI* GetPlayerbotAI() const;

private:
    bool BuildPath(WorldPosition const& dest);
    void Advance();
    bool ShouldAbort() const;
    bool ReachedDestination() const;

private:
    ObjectGuid botGuid_;
    Player* ResolveBot() const;
    Movement::PointsArray path_;
    MoveReason reason_ = MoveReason::Travel;

    bool issuedPoint_ = false;
    MovementGenerator* issuedGenerator_ = nullptr;
    bool interrupted_ = false;
    uint32 splineId_ = 0;
    float issuedX_ = 0.0f;
    float issuedY_ = 0.0f;
    float issuedZ_ = 0.0f;
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
