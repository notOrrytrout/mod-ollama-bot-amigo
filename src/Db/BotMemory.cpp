#include "Database/DatabaseEnv.h"
#include "Database/QueryResult.h"
#include "Database/Field.h"
#include "BotMemory.h"
#include "Log.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace
{
    constexpr size_t kGoalRingCap = 25;
    constexpr size_t kStuckMaxEntries = 128;

    // DB write budgets (per bot) - tuned for scale.
    constexpr uint32_t kStuckWriteMinMs = 5000;    // 5s
    constexpr uint32_t kStuckWriteMaxMs = 10000;   // 10s
    constexpr uint32_t kPlannerWriteMinMs = 30000; // 30s
    constexpr uint32_t kPlannerWriteMaxMs = 60000; // 60s
    constexpr uint32_t kVendorWriteMinMs = 60000;  // 60s
    constexpr uint32_t kVendorWriteMaxMs = 120000; // 120s

    // DB token bucket - allow short bursts but cap sustained IO.
    constexpr float kDbTokenMax = 2.0f;
    constexpr float kDbTokenRefillPerMs = 1.0f / 5000.0f; // 1 token per 5s

    uint32_t StableJitter(uint64_t guid, uint32_t minMs, uint32_t maxMs)
    {
        if (maxMs <= minMs)
            return minMs;
        uint32_t span = maxMs - minMs;
        uint64_t x = guid;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return minMs + static_cast<uint32_t>(x % (span + 1));
    }
}

void BotMemory::EnsureSchema(bool enablePlanner, bool enableStuck, bool enableVendor)
{
    auto ensureTable = [](std::string const& tableName, std::string const& createSql)
    {
        // Avoid relying on fmt-style Database helpers: build raw SQL strings for compatibility.
        std::string query =
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '" +
            tableName + "' LIMIT 1";
        QueryResult result = CharacterDatabase.Query(query.c_str());
        if (result)
            return;
        CharacterDatabase.Execute(createSql);
        LOG_INFO("server.loading", "[OllamaBotAmigo] Ensured table exists: {}", tableName);
    };

    if (enablePlanner)
    {
        ensureTable("bot_planner_memory",
            "CREATE TABLE bot_planner_memory ("
            "guid BIGINT PRIMARY KEY, "
            "last_goal TEXT, "
            "completed_goals TEXT, "
            "abandoned_goals TEXT, "
            "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
            ")");
    }

    if (enableStuck)
    {
        ensureTable("amigo_stuck_memory",
            "CREATE TABLE amigo_stuck_memory ("
            "bot_guid BIGINT, "
            "action_key VARCHAR(128), "
            "attempts INT, "
            "last_attempt DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "PRIMARY KEY (bot_guid, action_key)"
            ")");
        CharacterDatabase.Execute(
            "DELETE FROM amigo_stuck_memory WHERE last_attempt < NOW() - INTERVAL 7 DAY");
    }

    if (enableVendor)
    {
        ensureTable("amigo_vendor_memory",
            "CREATE TABLE amigo_vendor_memory ("
            "bot_guid BIGINT, "
            "npc_entry INT, "
            "npc_name VARCHAR(64), "
            "role VARCHAR(32), "
            "zone INT, "
            "x FLOAT, "
            "y FLOAT, "
            "z FLOAT, "
            "last_used DATETIME DEFAULT CURRENT_TIMESTAMP, "
            "PRIMARY KEY (bot_guid, npc_entry)"
            ")");
    }
}

void BotMemory::Initialize(uint64_t botGuid, uint32_t nowMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    botGuid_ = botGuid;
    initialized_ = true;
    loaded_ = false;
    plannerDirty_ = false;
    vendorsDirty_ = false;
    lastPlannerWriteMs_ = lastStuckWriteMs_ = lastVendorWriteMs_ = nowMs;
    nextPlannerWriteEarliestMs_ = nowMs;
    nextStuckWriteEarliestMs_ = nowMs;
    nextVendorWriteEarliestMs_ = nowMs;
    dbTokens_ = kDbTokenMax;
    lastTokenRefillMs_ = nowMs;
    jitterMs_ = StableJitter(botGuid, 0, 1500);
}

void BotMemory::EnsureLoaded()
{
    if (!initialized_ || loaded_)
        return;

    LoadPlannerRow();
    LoadStuckRows();
    LoadVendorRows();
    loaded_ = true;
}

void BotMemory::Update(uint32_t nowMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureLoaded();

    RefillDbTokens(nowMs);

    // Write-behind flushes.
    if (plannerDirty_ && nowMs >= nextPlannerWriteEarliestMs_ && ConsumeDbToken(nowMs))
    {
        FlushPlanner();
        plannerDirty_ = false;
        lastPlannerWriteMs_ = nowMs;
        nextPlannerWriteEarliestMs_ = nowMs + StableJitter(botGuid_, kPlannerWriteMinMs, kPlannerWriteMaxMs) + jitterMs_;
    }

    if (nowMs >= nextStuckWriteEarliestMs_)
    {
        bool hasDirty = false;
        for (auto const& kv : stuck_)
        {
            if (kv.second.dirty)
            {
                hasDirty = true;
                break;
            }
        }
        if (hasDirty && ConsumeDbToken(nowMs))
        {
            FlushStuck();
            lastStuckWriteMs_ = nowMs;
            nextStuckWriteEarliestMs_ = nowMs + StableJitter(botGuid_, kStuckWriteMinMs, kStuckWriteMaxMs) + jitterMs_;
        }
    }

    if (vendorsDirty_ && nowMs >= nextVendorWriteEarliestMs_ && ConsumeDbToken(nowMs))
    {
        FlushVendors();
        vendorsDirty_ = false;
        lastVendorWriteMs_ = nowMs;
        nextVendorWriteEarliestMs_ = nowMs + StableJitter(botGuid_, kVendorWriteMinMs, kVendorWriteMaxMs) + jitterMs_;
    }
}

std::string BotMemory::GetLastGoal() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastGoal_;
}

void BotMemory::SetLastGoal(std::string goal)
{
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureLoaded();
    lastGoal_ = std::move(goal);
    plannerDirty_ = true;
}

std::vector<std::string> BotMemory::GetCompletedGoals() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::string>(completedGoals_.begin(), completedGoals_.end());
}

std::vector<std::string> BotMemory::GetAbandonedGoals() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::string>(abandonedGoals_.begin(), abandonedGoals_.end());
}

void BotMemory::AppendCompletedGoal(std::string goal)
{
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureLoaded();
    AppendRing(completedGoals_, std::move(goal), kGoalRingCap);
    plannerDirty_ = true;
}

void BotMemory::AppendAbandonedGoal(std::string goal)
{
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureLoaded();
    AppendRing(abandonedGoals_, std::move(goal), kGoalRingCap);
    plannerDirty_ = true;
}

void BotMemory::RecordFailure(std::string const& actionKey, FailureType type, uint32_t nowMs)
{
    if (actionKey.empty())
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    EnsureLoaded();

    auto& entry = stuck_[actionKey];
    entry.stats.attempts = std::min(entry.stats.attempts + 1, 10u);
    entry.stats.lastAttemptMs = nowMs;
    entry.stats.lastType = type;
    entry.stats.cooldownUntilMs = ComputeCooldownUntil(type, entry.stats.attempts, nowMs);
    entry.dirty = true;

    // In-memory eviction (simple cap). If too large, drop oldest by lastAttempt.
    if (stuck_.size() > kStuckMaxEntries)
    {
        auto worstIt = stuck_.begin();
        for (auto it = stuck_.begin(); it != stuck_.end(); ++it)
        {
            if (it->second.stats.lastAttemptMs < worstIt->second.stats.lastAttemptMs)
                worstIt = it;
        }
        stuck_.erase(worstIt);
    }

    // Schedule a flush soon, but rate-limited.
    if (nextStuckWriteEarliestMs_ < nowMs)
        nextStuckWriteEarliestMs_ = nowMs + StableJitter(botGuid_, kStuckWriteMinMs, kStuckWriteMaxMs) + jitterMs_;
}

FailureStats BotMemory::GetFailureStats(std::string const& actionKey, uint32_t nowMs) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stuck_.find(actionKey);
    if (it == stuck_.end())
        return FailureStats{};
    FailureStats out = it->second.stats;
    (void)nowMs;
    return out;
}

void BotMemory::ClearFailures(std::string const& actionKey)
{
    if (actionKey.empty())
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    EnsureLoaded();

    auto it = stuck_.find(actionKey);
    if (it == stuck_.end())
        return;

    stuck_.erase(it);

    // Persist deletion on next stuck flush by marking a synthetic dirty entry.
    // We flush stuck by rewriting attempts rows; deletions are done immediately.
    std::string escaped = actionKey;
    CharacterDatabase.EscapeString(escaped);
    std::ostringstream ss;
    ss << "DELETE FROM amigo_stuck_memory WHERE bot_guid = " << botGuid_ << " AND action_key = '" << escaped << "'";
    CharacterDatabase.Execute(ss.str().c_str());
}

void BotMemory::UpsertVendor(uint32_t npcEntry,
                            std::string npcName,
                            std::string role,
                            uint32_t zone,
                            WorldPosition const& pos,
                            uint32_t nowMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureLoaded();

    // Playerbots' WorldPosition accessors are not consistently const-qualified.
    // Keep the BotMemory interface const-correct by working on a local copy.
    WorldPosition posCopy = pos;

    auto& entry = vendors_[npcEntry];
    entry.record.npcEntry = npcEntry;
    entry.record.npcName = std::move(npcName);
    entry.record.role = std::move(role);
    entry.record.zone = zone;
    entry.record.mapId = posCopy.getMapId();
    entry.record.x = posCopy.getX();
    entry.record.y = posCopy.getY();
    entry.record.z = posCopy.getZ();
    entry.record.lastUsedMs = nowMs;
    entry.dirty = true;
    vendorsDirty_ = true;

    if (nextVendorWriteEarliestMs_ < nowMs)
        nextVendorWriteEarliestMs_ = nowMs + StableJitter(botGuid_, kVendorWriteMinMs, kVendorWriteMaxMs) + jitterMs_;
}

std::vector<VendorRecord> BotMemory::GetVendorsByRole(std::string const& role, uint32_t zone) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VendorRecord> out;
    for (auto const& kv : vendors_)
    {
        VendorRecord const& rec = kv.second.record;
        if (!role.empty() && rec.role != role)
            continue;
        if (zone != 0 && rec.zone != zone)
            continue;
        out.push_back(rec);
    }
    return out;
}

uint32_t BotMemory::NextDbFlushInMs(uint32_t nowMs) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t next = std::min({nextPlannerWriteEarliestMs_, nextStuckWriteEarliestMs_, nextVendorWriteEarliestMs_});
    return next <= nowMs ? 0U : (next - nowMs);
}

uint32_t BotMemory::PendingWrites() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t pending = 0;
    if (plannerDirty_)
        pending++;
    if (vendorsDirty_)
        pending++;
    for (auto const& kv : stuck_)
        if (kv.second.dirty)
            pending++;
    return pending;
}

void BotMemory::LoadPlannerRow()
{
    std::ostringstream ss;
    ss << "SELECT last_goal, completed_goals, abandoned_goals FROM bot_planner_memory WHERE guid = " << botGuid_;
    QueryResult result = CharacterDatabase.Query(ss.str().c_str());
    if (!result)
        return;

    Field* fields = result->Fetch();
    lastGoal_ = fields[0].Get<std::string>();
    completedGoals_ = DeserializeRing(fields[1].Get<std::string>());
    abandonedGoals_ = DeserializeRing(fields[2].Get<std::string>());
}

void BotMemory::LoadStuckRows()
{
    std::ostringstream ss;
    ss << "SELECT action_key, attempts, UNIX_TIMESTAMP(last_attempt) FROM amigo_stuck_memory WHERE bot_guid = " << botGuid_;
    QueryResult result = CharacterDatabase.Query(ss.str().c_str());
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        std::string key = fields[0].Get<std::string>();
        uint32_t attempts = fields[1].Get<uint32_t>();
        uint32_t lastUnix = fields[2].Get<uint32_t>();
        auto& entry = stuck_[key];
        entry.stats.attempts = attempts;
        entry.stats.lastAttemptMs = lastUnix * 1000u; // coarse mapping
        entry.stats.lastType = FailureType::Retryable;
        entry.stats.cooldownUntilMs = 0;
        entry.dirty = false;
    } while (result->NextRow());
}

void BotMemory::LoadVendorRows()
{
    std::ostringstream ss;
    ss << "SELECT npc_entry, npc_name, role, zone, x, y, z, UNIX_TIMESTAMP(last_used) FROM amigo_vendor_memory WHERE bot_guid = " << botGuid_;
    QueryResult result = CharacterDatabase.Query(ss.str().c_str());
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32_t npcEntry = fields[0].Get<uint32_t>();
        VendorEntry& entry = vendors_[npcEntry];
        entry.record.npcEntry = npcEntry;
        entry.record.npcName = fields[1].Get<std::string>();
        entry.record.role = fields[2].Get<std::string>();
        entry.record.zone = fields[3].Get<uint32_t>();
        entry.record.x = fields[4].Get<float>();
        entry.record.y = fields[5].Get<float>();
        entry.record.z = fields[6].Get<float>();
        entry.record.lastUsedMs = fields[7].Get<uint32_t>() * 1000u;
        entry.dirty = false;
    } while (result->NextRow());
}

void BotMemory::FlushPlanner()
{
    std::string last = lastGoal_;
    std::string completed = SerializeRing(completedGoals_);
    std::string abandoned = SerializeRing(abandonedGoals_);
    CharacterDatabase.EscapeString(last);
    CharacterDatabase.EscapeString(completed);
    CharacterDatabase.EscapeString(abandoned);

    std::ostringstream ss;
    ss << "INSERT INTO bot_planner_memory (guid, last_goal, completed_goals, abandoned_goals) "
       << "VALUES (" << botGuid_ << ", '" << last << "', '" << completed << "', '" << abandoned << "') "
       << "ON DUPLICATE KEY UPDATE last_goal='" << last << "', completed_goals='" << completed
       << "', abandoned_goals='" << abandoned << "'";
    CharacterDatabase.Execute(ss.str().c_str());
}

void BotMemory::FlushStuck()
{
    for (auto& kv : stuck_)
    {
        if (!kv.second.dirty)
            continue;
        std::string key = kv.first;
        CharacterDatabase.EscapeString(key);
        std::ostringstream ss;
        ss << "INSERT INTO amigo_stuck_memory (bot_guid, action_key, attempts, last_attempt) "
           << "VALUES (" << botGuid_ << ", '" << key << "', " << kv.second.stats.attempts << ", NOW()) "
           << "ON DUPLICATE KEY UPDATE attempts = " << kv.second.stats.attempts << ", last_attempt = NOW()";
        CharacterDatabase.Execute(ss.str().c_str());
        kv.second.dirty = false;
    }
}

void BotMemory::FlushVendors()
{
    for (auto& kv : vendors_)
    {
        if (!kv.second.dirty)
            continue;

        VendorRecord const& rec = kv.second.record;
        std::string npcName = rec.npcName;
        std::string role = rec.role;
        CharacterDatabase.EscapeString(npcName);
        CharacterDatabase.EscapeString(role);
        std::ostringstream ss;
        ss << "REPLACE INTO amigo_vendor_memory (bot_guid, npc_entry, npc_name, role, zone, x, y, z, last_used) "
           << "VALUES (" << botGuid_ << ", " << static_cast<uint32_t>(rec.npcEntry) << ", '" << npcName << "', '"
           << role << "', " << rec.zone << ", " << rec.x << ", " << rec.y << ", " << rec.z << ", NOW())";
        CharacterDatabase.Execute(ss.str().c_str());

        kv.second.dirty = false;
    }
}

bool BotMemory::ConsumeDbToken(uint32_t nowMs)
{
    RefillDbTokens(nowMs);
    if (dbTokens_ < 1.0f)
        return false;
    dbTokens_ -= 1.0f;
    return true;
}

void BotMemory::RefillDbTokens(uint32_t nowMs)
{
    if (nowMs <= lastTokenRefillMs_)
        return;

    uint32_t delta = nowMs - lastTokenRefillMs_;
    dbTokens_ = std::min(kDbTokenMax, dbTokens_ + delta * kDbTokenRefillPerMs);
    lastTokenRefillMs_ = nowMs;
}

void BotMemory::AppendRing(std::deque<std::string>& ring, std::string value, size_t cap)
{
    if (value.empty())
        return;
    ring.push_back(std::move(value));
    while (ring.size() > cap)
        ring.pop_front();
}

std::string BotMemory::SerializeRing(std::deque<std::string> const& ring)
{
    std::ostringstream out;
    bool first = true;
    for (auto const& item : ring)
    {
        if (!first)
            out << "\n";
        first = false;
        out << item;
    }
    return out.str();
}

std::deque<std::string> BotMemory::DeserializeRing(std::string const& text)
{
    std::deque<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty())
            out.push_back(line);
        if (out.size() >= kGoalRingCap)
            break;
    }
    return out;
}

uint32_t BotMemory::ComputeCooldownUntil(FailureType type, uint32_t attempts, uint32_t nowMs)
{
    attempts = std::max(1u, std::min(attempts, 10u));

    uint32_t base = 10000; // 10s
    uint32_t cap = 120000; // 2m
    switch (type)
    {
        case FailureType::Temporary:
            base = 10000;
            cap = 120000;
            break;
        case FailureType::Retryable:
            base = 20000;
            cap = 300000;
            break;
        case FailureType::Permanent:
            base = 1800000;
            cap = 21600000;
            break;
    }

    uint32_t cooldown = (type == FailureType::Permanent) ? base : std::min(base * attempts, cap);
    return nowMs + cooldown;
}

// Registry
std::mutex BotMemoryRegistry::mutex_;
std::unordered_map<uint64_t, BotMemory*> BotMemoryRegistry::memoryByGuid_;

void BotMemoryRegistry::Register(uint64_t guid, BotMemory* memory)
{
    std::lock_guard<std::mutex> lock(mutex_);
    memoryByGuid_[guid] = memory;
}

void BotMemoryRegistry::Unregister(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    memoryByGuid_.erase(guid);
}

BotMemory* BotMemoryRegistry::Get(uint64_t guid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = memoryByGuid_.find(guid);
    return it == memoryByGuid_.end() ? nullptr : it->second;
}
