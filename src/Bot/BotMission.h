#pragma once

#include "Define.h"
#include <mutex>
#include <string>
#include <unordered_map>

// Durable user-facing intent. Execution details stay in deterministic bot systems.
enum class BotMissionKind : uint8
{
    Quest,
    Gather,
    Grind,
    PvpBg,
    Party,
    Raid,
    Goal
};

struct BotMission
{
    BotMissionKind kind = BotMissionKind::Quest;
    // Quest/Goal: planner guidance. Gather/Grind: exact requested target name.
    std::string target;
    // Party/Raid role. Empty means automatic/default role selection.
    std::string role;

    std::string KindName() const;
    std::string PlannerGoal() const;
    bool MatchesTargetName(std::string const& displayName) const;
};

struct BotMissionState
{
    BotMission mission;
    uint64 revision = 1;
};

class BotMissionRegistry
{
public:
    static BotMissionRegistry& Instance();

    // Returns the current mission. New bots start in quest mode at revision 1.
    BotMissionState Get(uint64 botGuid);

    // Replaces semantic intent and advances the monotonic revision exactly once.
    uint64 Replace(uint64 botGuid, BotMission const& mission);

    // Used by async LLM results and queued commands to reject stale work.
    bool RevisionMatches(uint64 botGuid, uint64 revision);

private:
    std::mutex mutex_;
    std::unordered_map<uint64, BotMissionState> missions_;
};
