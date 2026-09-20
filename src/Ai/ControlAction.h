#pragma once

#include "Define.h"
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
        MoveHopNpc,
        EnterGrind,
        StopGrind,
        Stay,
        Unstay,
        TalkToQuestGiver,
        EnterAttackPull,
        // Gather one exact nearby game object selected by entry ID.
        GatherTarget,
        // Native Playerbots service and recovery capabilities.
        VendorSell,
        VendorBuyUseful,
        Repair,
        Trainer,
        Hearthstone,
        Taxi,
        Loot,
        Food,
        Drink,
        Maintenance,
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
    uint32 npcEntryId = 0;
    uint32 gameObjectEntryId = 0;
    std::string professionSkill;
    std::string professionIntent;
};

// One contract for actions exposed to the control model. The control loop,
// mock selector, and documentation use this catalog instead of maintaining
// separate tool lists.
struct ControlActionDefinition
{
    char const* name;
    char const* signature;
    ControlAction::Capability capability;
    bool requiresDirection;
    bool requiresDistance;
    bool requiresQuestId;
    bool requiresEntryId;
    bool requiresSkill;
    bool requiresIntent;
    bool requiresMessage;
    bool requiresNavEpoch;
    bool requiresCandidateId;
    bool lifecycleOwned;
    bool requiresNearbyTarget;
    char const* description;
    char const* completion;
    char const* failure;
    bool supported = true;
};

std::vector<ControlActionDefinition> const& GetControlActionCatalog();

struct ControlDecisionOption
{
    std::string action;
    uint32 questId = 0;
    uint32 entryId = 0;
    std::string targetName;
    std::string reason;
    float distance = 0.0f;
    bool local = true;
    bool lifecycleTravel = false;
    std::string destinationType;
    uint32 destinationEntry = 0;
    std::string navigationDirection;
    std::string candidateId;
    std::string objectiveType;
    int32 objectiveTargetId = 0;
    std::string sourceTarget;
};

struct ControlActionState
{
    // Action plus a human-readable explanation from the planner.
    ControlAction action;
    std::string reasoning;
    // Mission generation captured when the async control request started.
    // The main-thread controller rejects the action if this is stale.
    uint64 missionRevision = 0;
    // Lifecycle execution generation captured with the same request. A needs or
    // maintenance lane transition invalidates old model work without changing
    // the durable mission.
    uint64 lifecycleGeneration = 0;
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
