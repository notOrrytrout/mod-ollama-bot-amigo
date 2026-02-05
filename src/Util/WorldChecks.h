#pragma once
#include "Util/PlayerbotsCompat.h"

class WorldObject;

namespace WorldChecks
{
    // Line-of-sight from bot to a world object. Returns false on invalid inputs.
    bool IsWithinLOS(Player* bot, WorldObject* obj);

    // Line-of-sight from bot to an arbitrary world position (same map only).
    bool IsWithinLOS(Player* bot, WorldPosition const& pos);

    // Cheap, horizontal (2D) distance in meters. Returns 0 on invalid inputs.
    float GroundDistance(Player* bot, WorldPosition const& pos);

    // Reachability check using TrinityCore pathfinding (PathGenerator). This is a feasibility test,
    // not a movement execution. Returns false on invalid inputs or if the destination cannot be reached.
    bool CanReach(Player* bot, WorldPosition const& pos, float tolerance = 3.0f);
}
