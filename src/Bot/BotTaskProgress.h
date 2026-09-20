#pragma once

#include <cstdint>
#include <unordered_map>

inline bool AmigoPointInterrupted(bool issued, bool sameGenerator, bool sameSpline,
                                  bool finished, bool atPoint, bool idle)
{
    return issued && !sameGenerator && !(sameSpline && finished && atPoint && idle);
}

inline bool AmigoOwnsPoint(bool issued, uint32_t issuedSpline, uint32_t currentSpline, bool pointMotion)
{
    return issued && issuedSpline == currentSpline && pointMotion;
}

inline bool AmigoSearchTakeover(bool active, bool search, uint32_t quest, bool alive,
                                bool combat, bool revisionMatches, bool questComplete)
{
    return active && search && quest != 0 && alive && !combat && revisionMatches && questComplete;
}

// A route-local high water mark. Replanning changes the baseline but never
// replenishes the request's recovery budget.
class BotTaskProgress
{
public:
    void Reset(float remaining, uint32_t now) { best_ = remaining; changed_ = now; attempts_ = 0; }
    void Observe(float remaining, uint32_t now)
    {
        if (best_ == 0 && remaining > 0) { best_ = remaining; changed_ = now; }
        if (remaining + 0.5f < best_) { best_ = remaining; changed_ = now; }
    }
    bool Stalled(uint32_t now) const { return now - changed_ >= 8000; }
    bool Exhausted() const { return attempts_ >= 2; }
    void Retry(float remaining, uint32_t now) { ++attempts_; best_ = remaining; changed_ = now; }
    uint8_t Attempts() const { return attempts_; }
private:
    float best_ = 0;
    uint32_t changed_ = 0;
    uint8_t attempts_ = 0;
};

enum class BotLootPhase { Select, Approach, Open, Collect };

class BotLootPending
{
public:
    void Observe(uint64_t target, BotLootPhase phase, float /*remaining*/, uint32_t now)
    {
        if (target != target_ || phase != phase_)
        {
            target_ = target; phase_ = phase; attempts_ = 0; pending_ = false; changed_ = now;
            routeKnown_ = false;
        }
    }
    // Progress is the native spline cursor, including segments away from the
    // target. A new spline alone does not count as progress.
    void ObserveRoute(uint32_t spline, int32_t cursor, uint32_t now)
    {
        if (phase_ != BotLootPhase::Approach)
            return;
        if (routeKnown_ && spline == route_ && cursor > cursor_)
            changed_ = now;
        if (!routeKnown_ || spline != route_ || cursor > cursor_)
            cursor_ = cursor;
        route_ = spline;
        routeKnown_ = true;
    }
    bool Ready(uint32_t now) const { return !pending_ || now - issued_ >= 3000; }
    bool Exhausted(uint32_t now) const { return (attempts_ >= 3 && Ready(now)) || now - changed_ >= 30000; }
    void Issued(uint32_t now, bool succeeded = false)
    {
        pending_ = true; issued_ = now;
        if (succeeded)
            attempts_ = 0;
        else
            ++attempts_;
    }
    void BeginCollection(uint64_t target, uint32_t now)
    {
        Observe(target, BotLootPhase::Collect, 0, now);
        collection_ = true;
        responseSeen_ = false;
        collectionStarted_ = now;
    }
    bool Collecting(uint64_t openGuid, bool casting, uint32_t now)
    {
        if (openGuid)
        {
            if (!collection_ || target_ != openGuid)
                BeginCollection(openGuid, now);
            responseSeen_ = true;
            return true;
        }
        if (!collection_)
            return false;
        // Only a cast following our successful open can retain this request.
        if (casting && now - collectionStarted_ < 30000)
            return true;
        if (!responseSeen_ && now - collectionStarted_ < 5000)
            return true;
        collection_ = false;
        return false;
    }
private:
    uint64_t target_ = 0;
    BotLootPhase phase_ = BotLootPhase::Select;
    uint32_t changed_ = 0;
    uint32_t issued_ = 0;
    uint32_t route_ = 0;
    int32_t cursor_ = 0;
    bool routeKnown_ = false;
    bool collection_ = false;
    bool responseSeen_ = false;
    uint32_t collectionStarted_ = 0;
    uint8_t attempts_ = 0;
    bool pending_ = false;
};

// World-thread state, shared by lifecycle assessment and loot execution.
inline BotLootPending& AmigoLootPending(uint64_t botGuid)
{
    static std::unordered_map<uint64_t, BotLootPending> states;
    return states[botGuid];
}
