#pragma once

#include "Define.h"
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

struct Position3
{
    // Simple coordinate holder for movement targets and bot positions.
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ControlAction
{
    // High-level capabilities surfaced to the control loop.
    enum class Capability
    {
        Idle,
        MoveHop,
        EnterGrind,
        StopGrind,
        Stay,
        Unstay,
        TalkToQuestGiver,
        EnterAttackPull,
        // Profession: fish from current spot (no movement).
        Fish,
        // Profession: generic request (e.g. "mining" / "fish" / "craft").
        UseProfession,
        // Turn left by 90 degrees.
        TurnLeft90,
        // Turn right by 90 degrees.
        TurnRight90,
        // Turn around 180 degrees.
        TurnAround
    };

    Capability capability = Capability::Idle;
    // Move hop selection: the controller chooses among engine-computed
    // navigation candidates by opaque ID (no XYZ). The epoch prevents stale
    // selections.
    uint32 navEpoch = 0;
    std::string navCandidateId;
    uint32 questId = 0;
    std::string professionSkill;
    std::string professionIntent;
};

struct ControlActionState
{
    // Action plus a human-readable explanation from the planner.
    ControlAction action;
    std::string reasoning;
};

class ControlActionRegistry
{
public:
    // Singleton queue for control actions keyed by bot GUID.
    static ControlActionRegistry& Instance();
    void Enqueue(uint64 botGuid, ControlActionState const& action);
    bool TryDequeue(uint64 botGuid, ControlActionState& outAction);

private:
    std::mutex mutex_;
    std::unordered_map<uint64, std::deque<ControlActionState>> actions_;
};
