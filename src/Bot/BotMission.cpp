#include "Bot/BotMission.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <vector>

namespace
{
    std::string NormalizeName(std::string const& value)
    {
        std::istringstream input(value);
        std::ostringstream output;
        std::string word;
        bool first = true;
        while (input >> word)
        {
            if (!first)
            {
                output << ' ';
            }
            first = false;
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            output << word;
        }
        return output.str();
    }
}

std::string BotMission::KindName() const
{
    switch (kind)
    {
        case BotMissionKind::Quest: return "quest";
        case BotMissionKind::Gather: return "gather";
        case BotMissionKind::Grind: return "grind";
        case BotMissionKind::PvpBg: return "pvp_bg";
        case BotMissionKind::Party: return "party";
        case BotMissionKind::Raid: return "raid";
        case BotMissionKind::Goal: return "goal";
    }
    return "quest";
}

std::string BotMission::PlannerGoal() const
{
    switch (kind)
    {
        case BotMissionKind::Quest:
            return target.empty() ? "Follow the normal questing lifecycle" : target;
        case BotMissionKind::Gather:
            return target.empty() ? "Gather the assigned resource" : "Gather " + target;
        case BotMissionKind::Grind:
            return target.empty() ? "Grind the assigned creature" : "Grind " + target;
        case BotMissionKind::PvpBg:
            return "Queue for and participate in battleground PvP";
        case BotMissionKind::Party:
            return role.empty() ? "Cooperate with the party" : "Cooperate with the party as " + role;
        case BotMissionKind::Raid:
            return role.empty() ? "Cooperate with the raid" : "Cooperate with the raid as " + role;
        case BotMissionKind::Goal:
            return target;
    }
    return target;
}

bool BotMission::MatchesTargetName(std::string const& displayName) const
{
    if (kind != BotMissionKind::Gather && kind != BotMissionKind::Grind)
    {
        return true;
    }
    if (target.empty() || displayName.empty())
    {
        return false;
    }
    return NormalizeName(target) == NormalizeName(displayName);
}

BotMissionRegistry& BotMissionRegistry::Instance()
{
    static BotMissionRegistry instance;
    return instance;
}

BotMissionState BotMissionRegistry::Get(uint64 botGuid)
{
    if (!botGuid)
    {
        return BotMissionState{};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return missions_[botGuid];
}

uint64 BotMissionRegistry::Replace(uint64 botGuid, BotMission const& mission)
{
    if (!botGuid)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    BotMissionState& state = missions_[botGuid];
    if (state.revision < std::numeric_limits<uint64>::max())
    {
        ++state.revision;
    }
    state.mission = mission;
    return state.revision;
}

bool BotMissionRegistry::RevisionMatches(uint64 botGuid, uint64 revision)
{
    if (!botGuid || revision == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    return missions_[botGuid].revision == revision;
}
