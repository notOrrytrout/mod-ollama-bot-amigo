#pragma once

#include "Bot/ProfessionTypes.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class Player;
class PlayerbotAI;

// Execution-only profession runner.
//
// - No movement here (no MotionMaster).
// - Uses Playerbots action implementations for profession mechanics (cast/use).
// - Tick-driven, abortable, and safe to scale.
class BotProfession
{
public:
    bool StartFishing(Player* bot, PlayerbotAI* ai, uint32_t nowMs);

    void Update(Player* bot, PlayerbotAI* ai, uint32_t nowMs);
    void Abort(Player* bot, PlayerbotAI* ai, uint32_t nowMs);

    bool Active() const { return active_; }
    ProfessionActivity Activity() const { return activity_; }
    ProfessionResult LastResult() const { return lastResult_; }
    uint32_t LastChangeMs() const { return lastChangeMs_; }

private:
    bool ShouldAbort(Player* bot) const;
    void ClearBobberStrategy(PlayerbotAI* ai);

private:
    bool active_ = false;
    ProfessionActivity activity_ = ProfessionActivity::None;
    ProfessionResult lastResult_ = ProfessionResult::None;
    uint32_t startMs_ = 0;
    uint32_t lastStepMs_ = 0;
    uint32_t lastChangeMs_ = 0;
};

class BotProfessionRegistry
{
public:
    static void Register(uint64_t guid, BotProfession* prof);
    static void Unregister(uint64_t guid);
    static BotProfession* Get(uint64_t guid);

private:
    static std::mutex mutex_;
    static std::unordered_map<uint64_t, BotProfession*> byGuid_;
};
