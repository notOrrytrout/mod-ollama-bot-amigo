#include "Util/WorldChecks.h"

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

        if (bot->GetMapId() != posCopy.getMapId())
            return false;

        return bot->IsWithinLOS(posCopy.getX(),
                                posCopy.getY(),
                                posCopy.getZ());
    }

    float GroundDistance(Player* bot, WorldPosition const& pos)
    {
        if (!bot)
            return 0.0f;

        WorldPosition posCopy = pos;

        if (bot->GetMapId() != posCopy.getMapId())
            return 0.0f;

        float dx = bot->GetPositionX() - posCopy.getX();
        float dy = bot->GetPositionY() - posCopy.getY();
        return std::sqrt(dx * dx + dy * dy);
    }

    bool CanReach(Player* bot, WorldPosition const& pos, float tolerance)
    {
        if (!bot)
            return false;

        WorldPosition posCopy = pos;

        if (bot->GetMapId() != posCopy.getMapId())
            return false;

        PathGenerator pathGen(bot);
        // Playerbots explicitly disables straight-line shortcuts.
        pathGen.SetUseStraightPath(false);

        if (!pathGen.CalculatePath(posCopy.getX(),
                                   posCopy.getY(),
                                   posCopy.getZ()))
            return false;

        Movement::PointsArray const& pts = pathGen.GetPath();
        if (pts.empty())
            return false;

        auto const& last = pts.back();
        float dx = last.x - posCopy.getX();
        float dy = last.y - posCopy.getY();
        float dist2d = std::sqrt(dx * dx + dy * dy);

        // If the path ends close enough to the destination, treat it as reachable.
        return dist2d <= std::max(0.5f, tolerance);
    }
}
