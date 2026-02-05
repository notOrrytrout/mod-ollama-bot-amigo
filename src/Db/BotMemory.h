#pragma once

#include "DatabaseEnv.h"
#include "MemoryTypes.h"
#include "Util/WorldPositionCompat.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Thin per-bot memory layer backed by CharacterDatabase.
//
// Goals:
// - No raw SQL outside this unit.
// - Two-tier cache: in-memory fast path + persistent backing.
// - Write-behind with per-bot rate limiting.
// - Read-only to the LLM: callers should request summaries only.

class BotMemory
{
public:
    static void EnsureSchema(bool enablePlanner, bool enableStuck, bool enableVendor);

    void Initialize(uint64_t botGuid, uint32_t nowMs);
    void Update(uint32_t nowMs);

    // Planner memory
    std::string GetLastGoal() const;
    void SetLastGoal(std::string goal);
    std::vector<std::string> GetCompletedGoals() const;
    std::vector<std::string> GetAbandonedGoals() const;
    void AppendCompletedGoal(std::string goal);
    void AppendAbandonedGoal(std::string goal);

    // Stuck memory
    void RecordFailure(std::string const& actionKey, FailureType type, uint32_t nowMs);
    FailureStats GetFailureStats(std::string const& actionKey, uint32_t nowMs) const;
    void ClearFailures(std::string const& actionKey);

    // Vendor memory
    void UpsertVendor(uint32_t npcEntry,
                      std::string npcName,
                      std::string role,
                      uint32_t zone,
                      WorldPosition const& pos,
                      uint32_t nowMs);

    std::vector<VendorRecord> GetVendorsByRole(std::string const& role, uint32_t zone) const;

    // Debug/status
    uint32_t NextDbFlushInMs(uint32_t nowMs) const;
    uint32_t PendingWrites() const;

private:
    void EnsureLoaded();
    void LoadPlannerRow();
    void LoadStuckRows();
    void LoadVendorRows();

    void FlushPlanner();
    void FlushStuck();
    void FlushVendors();

    bool ConsumeDbToken(uint32_t nowMs);
    void RefillDbTokens(uint32_t nowMs);

    static void AppendRing(std::deque<std::string>& ring, std::string value, size_t cap);
    static std::string SerializeRing(std::deque<std::string> const& ring);
    static std::deque<std::string> DeserializeRing(std::string const& text);

    static uint32_t ComputeCooldownUntil(FailureType type, uint32_t attempts, uint32_t nowMs);

private:
    uint64_t botGuid_ = 0;
    bool initialized_ = false;
    bool loaded_ = false;

    // Two-tier cache: Tier A in-memory.
    std::string lastGoal_;
    std::deque<std::string> completedGoals_;
    std::deque<std::string> abandonedGoals_;

    struct StuckEntry
    {
        FailureStats stats;
        bool dirty = false;
    };

    // Keyed by action_key.
    std::unordered_map<std::string, StuckEntry> stuck_;

    struct VendorEntry
    {
        VendorRecord record;
        bool dirty = false;
    };

    // Keyed by npc_entry.
    std::unordered_map<uint32_t, VendorEntry> vendors_;

    // Tier B DB control / write-behind.
    bool plannerDirty_ = false;
    bool vendorsDirty_ = false;
    uint32_t lastPlannerWriteMs_ = 0;
    uint32_t lastStuckWriteMs_ = 0;
    uint32_t lastVendorWriteMs_ = 0;

    uint32_t nextPlannerWriteEarliestMs_ = 0;
    uint32_t nextStuckWriteEarliestMs_ = 0;
    uint32_t nextVendorWriteEarliestMs_ = 0;

    // Token bucket for DB writes.
    float dbTokens_ = 2.0f;
    uint32_t lastTokenRefillMs_ = 0;

    // Jitter (per bot) to spread flushes.
    uint32_t jitterMs_ = 0;

    mutable std::mutex mutex_;
};

class BotMemoryRegistry
{
public:
    static void Register(uint64_t guid, BotMemory* memory);
    static void Unregister(uint64_t guid);
    static BotMemory* Get(uint64_t guid);

private:
    static std::mutex mutex_;
    static std::unordered_map<uint64_t, BotMemory*> memoryByGuid_;
};
