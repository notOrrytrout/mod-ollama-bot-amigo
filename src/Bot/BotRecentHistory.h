#pragma once

#include "Define.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Lightweight per-bot ring buffers for "recent" context that helps the LLM
// learn from immediate outcomes (accepted/rejected move hops, travel results,
// goal changes/completions).
//
// Notes:
// - This is intentionally in-memory only (not DB-backed) and small.
// - No coordinates are stored here; only semantic IDs and outcomes.
class BotRecentHistory
{
public:
    enum class MovementTool : uint8_t
    {
        MoveHop,
        MoveHopNpc
    };

    struct MovementAttempt
    {
        uint32_t atMs = 0;
        MovementTool tool = MovementTool::MoveHop;
        bool accepted = false;
        std::string reason; // stable token(s), optionally with short detail

        // move_hop context (no coordinates)
        uint32_t navEpoch = 0;
        std::string candidateId;

        // Candidate feasibility signals (from nav snapshot / engine checks).
        bool candReachable = false;
        bool candHasLOS = false;
        bool candCanMove = false;
        bool engineReachable = false;
        bool engineHasLOS = false;
    };

    struct MovementResult
    {
        uint32_t atMs = 0;
        std::string travelKey; // e.g. move_hop:candidate:<epoch>:<candidate_id>
        std::string result;    // reached/timed_out/aborted
    };

    struct LongTermGoalChange
    {
        uint32_t atMs = 0;
        std::string from;
        std::string to;
    };

    struct ShortTermGoalsChange
    {
        uint32_t atMs = 0;
        std::string longTermGoal;
        std::vector<std::string> goals;
    };

    struct ShortTermGoalCompletion
    {
        uint32_t atMs = 0;
        std::string goal;
        std::string travelKey;
    };

public:
    void Clear();
    void ClearGoalHistory();

    void RecordMovementAttempt(MovementAttempt attempt);
    void RecordMovementResult(uint32_t nowMs, std::string travelKey, std::string result);

    void RecordLongTermGoalChange(uint32_t nowMs, std::string from, std::string to);
    void RecordShortTermGoalsChange(uint32_t nowMs, std::string longTermGoal, std::vector<std::string> goals);
    void RecordShortTermGoalCompleted(uint32_t nowMs, std::string goal, std::string travelKey);

    std::vector<MovementAttempt> GetMovementAttempts(size_t max) const;
    std::vector<MovementResult> GetMovementResults(size_t max) const;
    std::vector<LongTermGoalChange> GetLongTermGoalChanges(size_t max) const;
    std::vector<ShortTermGoalsChange> GetShortTermGoalsChanges(size_t max) const;
    std::vector<ShortTermGoalCompletion> GetShortTermGoalCompletions(size_t max) const;

    static const char* MovementToolName(MovementTool tool);

private:
    template <class T>
    static std::vector<T> TailCopy(std::deque<T> const& src, size_t max);

    static std::string TrimForLog(std::string value, size_t maxLen);

private:
    mutable std::mutex mutex_;
    std::deque<MovementAttempt> movementAttempts_;
    std::deque<MovementResult> movementResults_;
    std::deque<LongTermGoalChange> longTermGoalChanges_;
    std::deque<ShortTermGoalsChange> shortTermGoalsChanges_;
    std::deque<ShortTermGoalCompletion> shortTermGoalCompletions_;
};

class BotRecentHistoryRegistry
{
public:
    static void Register(uint64_t guid, BotRecentHistory* history);
    static void Unregister(uint64_t guid);
    static BotRecentHistory* Get(uint64_t guid);

private:
    static std::mutex mutex_;
    static std::unordered_map<uint64_t, BotRecentHistory*> byGuid_;
};
