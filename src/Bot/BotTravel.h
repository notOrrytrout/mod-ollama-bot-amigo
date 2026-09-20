#pragma once

#include "Util/WorldPositionCompat.h"
#include "Bot/BotMovement.h"
#include "Bot/BotTaskProgress.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

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

enum class AmigoTravelPurpose
{
    Search,
    LiveTarget,
    Service,
    Loot,
    Manual,
};

enum class AmigoCompletionCondition
{
    Position,
    LiveTarget,
};

// NOTE: Playerbots defines a class named TravelTarget in TravelMgr.
// Keep our semantic target type in a distinct name to avoid ODR/type clashes.
struct AmigoTravelTarget
{
    AmigoTravelTarget() = default;
    AmigoTravelTarget(std::string targetKey, WorldPosition const& targetDest, float targetRadius, uint32_t targetTimeout)
        : key(std::move(targetKey)), dest(targetDest), radius(targetRadius), timeoutMs(targetTimeout) {}

    // Opaque key for memory/diagnostics (not shown to the LLM).
    std::string key;
    WorldPosition dest;
    float radius = 2.5f;           // meters
    uint32_t timeoutMs = 120000;   // 2 minutes default safety timeout
    uint64_t missionRevision = 0;
    uint32_t turnInQuestId = 0;
    // Stable retry key for temporary avoidance after a failed navigation hop.
    std::string retryKey;
    AmigoTravelPurpose purpose = AmigoTravelPurpose::Search;
    AmigoCompletionCondition completion = AmigoCompletionCondition::Position;
    uint64_t targetGuid = 0;
};

class BotTravel
{
public:
    // Shared movement entry point. This owns the movement/travel transaction.
    bool Start(Player* bot, BotMovement* movement, AmigoTravelTarget const& target,
               MoveReason reason, uint32_t nowMs);
    void Begin(AmigoTravelTarget const& target, uint32_t nowMs);
    void Abort(uint32_t nowMs, BotMovement* movement = nullptr);
    void Clear();

    // Update completion/failure state. Called from the main tick.
    void Update(Player* bot, uint32_t nowMs, BotMovement* movement = nullptr);

    bool Active() const { return active_; }
    std::optional<AmigoTravelTarget> Current() const { return target_; }
    TravelResult LastResult() const { return lastResult_; }
    uint32_t LastChangeMs() const { return lastChangeMs_; }
    uint8_t RecoveryAttempts() const { return progress_.Attempts(); }

private:
    bool Reached(Player* bot) const;

private:
    bool active_ = false;
    std::optional<AmigoTravelTarget> target_;
    TravelResult lastResult_ = TravelResult::None;
    uint32_t startMs_ = 0;
    uint32_t lastChangeMs_ = 0;
    BotTaskProgress progress_;
    BotMovement* movement_ = nullptr;
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
