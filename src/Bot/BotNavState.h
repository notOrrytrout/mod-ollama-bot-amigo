#pragma once

#include "Define.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class WorldPosition;

// Internal-only navigation candidate resolved by the engine.
//
// HARD BOUNDARY:
// - Coordinates stored here must never be serialized to the LLM.
struct NavCandidateInternal
{
    std::string candidateId;   // Opaque id (e.g., "nav_0")
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    // Engine-derived feasibility signals.
    bool reachable = false;
    bool hasLOS = false;
    bool canMove = false;
};

struct BotNavState
{
    // Monotonic epoch for this candidate set.
    uint32 navEpoch = 0;
    std::vector<NavCandidateInternal> candidates;
};

// Registry so the loop can publish internal candidate destinations and
// the controller/executors can resolve candidate_id to WorldPosition.
class BotNavStateRegistry
{
public:
    static void SetState(uint64 guid, BotNavState const& state);

    // Resolve candidate_id to an engine WorldPosition. Returns false if the
    // guid is unknown, epoch mismatches, or the candidateId does not exist.
    static bool TryResolve(
        uint64 guid,
        uint32 navEpoch,
        std::string const& candidateId,
        WorldPosition& outDest,
        bool& outReachable,
        bool& outHasLOS,
        bool& outCanMove);

    static void Clear(uint64 guid);

private:
    static std::mutex& Mutex();
    static std::unordered_map<uint64, std::deque<BotNavState>>& Storage();
};
