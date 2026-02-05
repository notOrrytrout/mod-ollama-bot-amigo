#pragma once

#include "Util/WorldPositionCompat.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// Travel semantics layer (Playerbots-inspired): a destination has a radius,
// completion rules, and failure classification.
//
// HARD CONSTRAINTS:
// - No MotionMaster access.
// - No pathfinding.
// - No coordinate interpolation.

enum class TravelResult
{
    None,
    Reached,
    TimedOut,
    Aborted,
};

// NOTE: Playerbots defines a class named TravelTarget in TravelMgr.
// Keep our semantic target type in a distinct name to avoid ODR/type clashes.
struct AmigoTravelTarget
{
    // Opaque key for memory/diagnostics (not shown to the LLM).
    std::string key;
    WorldPosition dest;
    float radius = 2.5f;           // meters
    uint32_t timeoutMs = 120000;   // 2 minutes default safety timeout
};

class BotTravel
{
public:
    void Begin(AmigoTravelTarget const& target, uint32_t nowMs);
    void Abort(uint32_t nowMs);
    void Clear();

    // Update completion/failure state. Called from the main tick.
    void Update(Player* bot, uint32_t nowMs);

    bool Active() const { return active_; }
    std::optional<AmigoTravelTarget> Current() const { return target_; }
    TravelResult LastResult() const { return lastResult_; }
    uint32_t LastChangeMs() const { return lastChangeMs_; }

private:
    bool Reached(Player* bot) const;

private:
    bool active_ = false;
    std::optional<AmigoTravelTarget> target_;
    TravelResult lastResult_ = TravelResult::None;
    uint32_t startMs_ = 0;
    uint32_t lastChangeMs_ = 0;
};

// Registry so controller and loop can share per-bot travel state.
class BotTravelRegistry
{
public:
    static void Register(uint64_t guid, BotTravel* travel);
    static void Unregister(uint64_t guid);
    static BotTravel* Get(uint64_t guid);

private:
    static std::mutex mutex_;
    static std::unordered_map<uint64_t, BotTravel*> travelByGuid_;
};
