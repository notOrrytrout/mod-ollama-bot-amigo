#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// Short-lived in-memory avoidance for navigation candidates that did not reach
// their destination. A failed hop must not permanently teach the bot to avoid a
// direction.
class BotNavigationPenalty
{
public:
    void Penalize(std::string const& candidateId, uint32_t nowMs, uint32_t durationMs)
    {
        if (candidateId.empty() || durationMs == 0)
            return;

        uint32_t expiry = nowMs + durationMs;
        auto it = expiryByCandidate_.find(candidateId);
        if (it == expiryByCandidate_.end())
        {
            expiryByCandidate_.emplace(candidateId, expiry);
            return;
        }

        // Repeated failures extend the active penalty.
        if (it->second < expiry)
            it->second = expiry;
    }

    uint32_t Remaining(std::string const& candidateId, uint32_t nowMs) const
    {
        auto it = expiryByCandidate_.find(candidateId);
        if (it == expiryByCandidate_.end() || nowMs >= it->second)
            return 0;
        return it->second - nowMs;
    }

    bool IsPenalized(std::string const& candidateId, uint32_t nowMs) const
    {
        return Remaining(candidateId, nowMs) != 0;
    }

    void Clear(std::string const& candidateId)
    {
        expiryByCandidate_.erase(candidateId);
    }

    void Prune(uint32_t nowMs)
    {
        for (auto it = expiryByCandidate_.begin(); it != expiryByCandidate_.end();)
        {
            if (nowMs >= it->second)
                it = expiryByCandidate_.erase(it);
            else
                ++it;
        }
    }

private:
    std::unordered_map<std::string, uint32_t> expiryByCandidate_;
};
