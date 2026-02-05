#include "Bot/BotRecentHistory.h"

#include <algorithm>

namespace
{
    constexpr size_t kMaxMovementAttempts = 16;
    constexpr size_t kMaxMovementResults = 16;
    constexpr size_t kMaxLongTermGoalChanges = 8;
    constexpr size_t kMaxShortTermGoalChanges = 10;
    constexpr size_t kMaxShortTermGoalCompletions = 12;
}

std::mutex BotRecentHistoryRegistry::mutex_;
std::unordered_map<uint64_t, BotRecentHistory*> BotRecentHistoryRegistry::byGuid_;

void BotRecentHistory::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    movementAttempts_.clear();
    movementResults_.clear();
    longTermGoalChanges_.clear();
    shortTermGoalsChanges_.clear();
    shortTermGoalCompletions_.clear();
}

void BotRecentHistory::ClearGoalHistory()
{
    std::lock_guard<std::mutex> lock(mutex_);
    longTermGoalChanges_.clear();
    shortTermGoalsChanges_.clear();
    shortTermGoalCompletions_.clear();
}

void BotRecentHistory::RecordMovementAttempt(MovementAttempt attempt)
{
    attempt.reason = TrimForLog(std::move(attempt.reason), 240);
    attempt.candidateId = TrimForLog(std::move(attempt.candidateId), 64);

    std::lock_guard<std::mutex> lock(mutex_);
    movementAttempts_.push_back(std::move(attempt));
    while (movementAttempts_.size() > kMaxMovementAttempts)
        movementAttempts_.pop_front();
}

void BotRecentHistory::RecordMovementResult(uint32_t nowMs, std::string travelKey, std::string result)
{
    MovementResult e;
    e.atMs = nowMs;
    e.travelKey = TrimForLog(std::move(travelKey), 128);
    e.result = TrimForLog(std::move(result), 32);

    std::lock_guard<std::mutex> lock(mutex_);
    movementResults_.push_back(std::move(e));
    while (movementResults_.size() > kMaxMovementResults)
        movementResults_.pop_front();
}

void BotRecentHistory::RecordLongTermGoalChange(uint32_t nowMs, std::string from, std::string to)
{
    LongTermGoalChange e;
    e.atMs = nowMs;
    e.from = TrimForLog(std::move(from), 240);
    e.to = TrimForLog(std::move(to), 240);

    std::lock_guard<std::mutex> lock(mutex_);
    longTermGoalChanges_.push_back(std::move(e));
    while (longTermGoalChanges_.size() > kMaxLongTermGoalChanges)
        longTermGoalChanges_.pop_front();
}

void BotRecentHistory::RecordShortTermGoalsChange(uint32_t nowMs, std::string longTermGoal, std::vector<std::string> goals)
{
    ShortTermGoalsChange e;
    e.atMs = nowMs;
    e.longTermGoal = TrimForLog(std::move(longTermGoal), 240);
    e.goals.reserve(goals.size());
    for (std::string& g : goals)
        e.goals.push_back(TrimForLog(std::move(g), 240));

    std::lock_guard<std::mutex> lock(mutex_);
    shortTermGoalsChanges_.push_back(std::move(e));
    while (shortTermGoalsChanges_.size() > kMaxShortTermGoalChanges)
        shortTermGoalsChanges_.pop_front();
}

void BotRecentHistory::RecordShortTermGoalCompleted(uint32_t nowMs, std::string goal, std::string travelKey)
{
    ShortTermGoalCompletion e;
    e.atMs = nowMs;
    e.goal = TrimForLog(std::move(goal), 240);
    e.travelKey = TrimForLog(std::move(travelKey), 128);

    std::lock_guard<std::mutex> lock(mutex_);
    shortTermGoalCompletions_.push_back(std::move(e));
    while (shortTermGoalCompletions_.size() > kMaxShortTermGoalCompletions)
        shortTermGoalCompletions_.pop_front();
}

std::vector<BotRecentHistory::MovementAttempt> BotRecentHistory::GetMovementAttempts(size_t max) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return TailCopy(movementAttempts_, max);
}

std::vector<BotRecentHistory::MovementResult> BotRecentHistory::GetMovementResults(size_t max) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return TailCopy(movementResults_, max);
}

std::vector<BotRecentHistory::LongTermGoalChange> BotRecentHistory::GetLongTermGoalChanges(size_t max) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return TailCopy(longTermGoalChanges_, max);
}

std::vector<BotRecentHistory::ShortTermGoalsChange> BotRecentHistory::GetShortTermGoalsChanges(size_t max) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return TailCopy(shortTermGoalsChanges_, max);
}

std::vector<BotRecentHistory::ShortTermGoalCompletion> BotRecentHistory::GetShortTermGoalCompletions(size_t max) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return TailCopy(shortTermGoalCompletions_, max);
}

const char* BotRecentHistory::MovementToolName(MovementTool tool)
{
    switch (tool)
    {
    case MovementTool::MoveHop:
        return "move_hop";
    case MovementTool::MoveHopNpc:
        return "move_hop_npc";
    default:
        return "unknown";
    }
}

template <class T>
std::vector<T> BotRecentHistory::TailCopy(std::deque<T> const& src, size_t max)
{
    if (max == 0 || src.empty())
        return {};

    const size_t count = std::min(max, src.size());
    std::vector<T> out;
    out.reserve(count);
    const size_t start = src.size() - count;
    for (size_t i = start; i < src.size(); ++i)
        out.push_back(src[i]);
    return out;
}

std::string BotRecentHistory::TrimForLog(std::string value, size_t maxLen)
{
    if (value.size() <= maxLen)
        return value;
    value.resize(maxLen);
    return value;
}

void BotRecentHistoryRegistry::Register(uint64_t guid, BotRecentHistory* history)
{
    std::lock_guard<std::mutex> lock(mutex_);
    byGuid_[guid] = history;
}

void BotRecentHistoryRegistry::Unregister(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    byGuid_.erase(guid);
}

BotRecentHistory* BotRecentHistoryRegistry::Get(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byGuid_.find(guid);
    return it == byGuid_.end() ? nullptr : it->second;
}
