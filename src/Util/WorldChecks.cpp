#include "Util/WorldChecks.h"

#include <algorithm>
#include <cmath>

namespace WorldChecks
{
    bool IsWithinLOS(Player* bot, WorldObject* obj)
    {
        if (!bot || !obj)
            return false;

        if (bot->GetMapId() != obj->GetMapId())
            return false;

        // Prefer the positional LOS check for broad compatibility.
        return bot->IsWithinLOS(obj->GetPositionX(),
                                obj->GetPositionY(),
                                obj->GetPositionZ());
    }

    bool IsWithinLOS(Player* bot, WorldPosition const& pos)
    {
        if (!bot)
            return false;

        // Playerbots WorldPosition is not const-correct
        WorldPosition posCopy = pos;

        if (bot->GetMapId() != posCopy.GetMapId())
            return false;

        return bot->IsWithinLOS(posCopy.GetPositionX(),
                                posCopy.GetPositionY(),
                                posCopy.GetPositionZ());
    }

    float GroundDistance(Player* bot, WorldPosition const& pos)
    {
        if (!bot)
            return 0.0f;

        WorldPosition posCopy = pos;

        if (bot->GetMapId() != posCopy.GetMapId())
            return 0.0f;

        float dx = bot->GetPositionX() - posCopy.GetPositionX();
        float dy = bot->GetPositionY() - posCopy.GetPositionY();
        return std::sqrt(dx * dx + dy * dy);
    }

    bool CanReach(Player* bot, WorldPosition const& pos, float tolerance)
    {
        if (!bot)
            return false;

        WorldPosition posCopy = pos;

        if (bot->GetMapId() != posCopy.GetMapId())
            return false;

        PathGenerator pathGen(bot);
        // Playerbots explicitly disables straight-line shortcuts.
        pathGen.SetUseStraightPath(false);

        if (!pathGen.CalculatePath(posCopy.GetPositionX(),
                                   posCopy.GetPositionY(),
                                   posCopy.GetPositionZ()))
            return false;

        Movement::PointsArray const& pts = pathGen.GetPath();
        if (pathGen.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT))
            return false;
        if (pts.empty())
            return false;

        auto const& last = pts.back();
        float dx = last.x - posCopy.GetPositionX();
        float dy = last.y - posCopy.GetPositionY();
        float dist2d = std::sqrt(dx * dx + dy * dy);
        float distz = std::fabs(last.z - posCopy.GetPositionZ());

        // If the path ends close enough to the destination, treat it as reachable.
        // A point on another floor can be close in 2D. The path endpoint must
        // also agree with the requested height before it is usable.
        return dist2d <= std::max(0.5f, tolerance) && distz <= std::max(1.0f, tolerance);
    }
}
