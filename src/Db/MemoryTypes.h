#pragma once

#include <cstdint>
#include <string>

// Failure taxonomy used by memory and controller glue.
enum class FailureType : uint8_t
{
    Temporary = 0,
    Retryable = 1,
    Permanent = 2,
};

struct FailureStats
{
    uint32_t attempts = 0;
    uint32_t lastAttemptMs = 0;
    FailureType lastType = FailureType::Temporary;
    uint32_t cooldownUntilMs = 0;

    uint32_t CooldownRemainingMs(uint32_t nowMs) const
    {
        return nowMs >= cooldownUntilMs ? 0U : (cooldownUntilMs - nowMs);
    }
};

// Vendor memory record (in-memory tier).
struct VendorRecord
{
    uint64_t npcEntry = 0;
    std::string npcName;
    std::string role;
    uint32_t zone = 0;
    uint32_t mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32_t lastUsedMs = 0;
};
