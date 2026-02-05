#include "Script/OllamaBotControlLoop.h"
#include "Ai/ControlAction.h"
#include "Script/OllamaBotConfig.h"
#include "Bot/BotControlApi.h"
#include "Ai/LlmContext.h"
#include "Ai/LlmRoles.h"
#include "DBCStores.h"
#include "Util/PlayerbotsCompat.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Item.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "GameObject.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Timer.h"
#include "Errors.h"
#include "Ai/OllamaRuntime.h"
#include "Bot/BotMovement.h"
#include "Util/WorldChecks.h"
#include "Db/BotMemory.h"
#include "Bot/BotTravel.h"
#include "Bot/BotProfession.h"
#include "Bot/BotNavState.h"
#include "Script/OllamaBotPlannerRefresh.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <curl/curl.h>
#include <ctime>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Timing and tuning constants for planner/control loops.
    // Defaults are tuned to scale across many bots without spamming the control plane.
    constexpr uint32 kStrategicIntervalMs = 20000;           // 20s
    constexpr uint32 kControlIntervalMs = 2500;              // 2.5s (fallback; normally overridden via config)
    constexpr uint32 kStrategicGoalChangeCooldownMs = 60000; // 60s
    constexpr float kIdlePositionEpsilon = 0.1f;
    constexpr uint32 kIdlePenaltyStartCycles = 8;
    constexpr uint32 kOllamaFailureHoldMs = 60000;   // 60s
    constexpr uint32 kPlannerFailureDelayMs = 90000; // 90s
    constexpr uint32 kGlobalFailureWindowMs = 60000;
    constexpr uint32 kGlobalFailureThreshold = 40;
    constexpr uint32 kGlobalControlPauseMs = 3000;
    constexpr uint32 kGlobalResumeSpreadMs = 5000;

    constexpr uint32 kOllamaBaseCooldownMs = 5000; // 5 seconds
    constexpr uint32 kOllamaMaxCooldownMs = 60000; // 60 seconds
    // When entering grind mode, give the bot time to start fighting before requesting
    // another control action from the LLM (prevents rapid grind spam).
    constexpr uint32 kPostEnterGrindControlDelayMs = 10000; // 10 seconds
    constexpr float kQuestGiverApproachOffsetMeters = 1.8f;
    struct DistanceBand
    {
        // Label and concrete distance for move hop tool arguments.
        const char *label;
        float distance;
    };

    constexpr std::array<DistanceBand, 5> kMoveHopDistanceBands = {{{"very close", 12.0f},
                                                                    {"close", 18.0f},
                                                                    {"medium", 36.0f},
                                                                    {"medium far", 46.0f},
                                                                    {"far", 58.0f}}};

    std::string DistanceBandLabelForDistance(float distance)
    {
        // Convert a numeric distance into the closest allowed distance-band label.
        // NOTE: This is for LLM-facing summaries only; the engine remains authoritative.
        for (auto const &band : kMoveHopDistanceBands)
        {
            if (distance <= band.distance)
            {
                return band.label;
            }
        }
        return kMoveHopDistanceBands.back().label;
    }

    struct BotSnapshot
    {
        // Condensed view of bot/world state sent to the LLM.
        Position3 pos;
        float orientation = 0.0f;
        uint32 mapId = 0;
        uint32 navEpoch = 0;
        uint32 zoneId = 0;
        uint32 areaId = 0;
        bool inCombat = false;
        bool grindMode = false;
        bool isMoving = false;
        struct GearSlot
        {
            std::string slot;
            std::string item;
            uint32 itemLevel = 0;
        };
        float avgItemLevel = 0.0f;
        float expectedAvgItemLevel = 0.0f;
        std::string gearBand = "unknown"; // low/medium/high/unknown
        std::vector<GearSlot> lowGearSlots;
        // Travel target state (semantic completion layer).
        bool travelActive = false;
        TravelResult travelLastResult = TravelResult::None;
        uint32 travelLastChangeMs = 0;
        float travelRadius = 0.0f;
        std::string travelLabel;

        // Profession execution state (execution-only, non-combat).
        bool professionActive = false;
        ProfessionActivity professionActivity = ProfessionActivity::None;
        ProfessionResult professionLastResult = ProfessionResult::None;
        uint32 professionLastChangeMs = 0;
        // Debug/backpressure signals (safe to expose; no engine control).
        uint32 controlCooldownRemainingMs = 0;
        uint32 controlOllamaBackoffMs = 0;
        uint32 memoryPendingWrites = 0;
        uint32 memoryNextFlushMs = 0;
        uint32 idleCycles = 0;
        float hpPct = 0.0f;
        float manaPct = 0.0f;
        uint32 level = 0;
        bool hasWeapon = false;
        std::vector<std::string> weaponTypes;
        std::vector<std::string> professions;
        struct NavCandidate
        {
            std::string label;
            Position3 pos;
            bool canMove = false;
            // Engine-derived feasibility signals.
            bool hasLOS = false;
            bool reachable = false;
            // Derived orientation helpers for the LLM.
            float distance2d = 0.0f;
            float bearingDeg = 0.0f;
            std::string direction;
        };
        std::vector<NavCandidate> navCandidates;
        std::vector<uint32> activeQuestIds;
        struct QuestObjectiveProgress
        {
            std::string type;
            int32 targetId = 0;
            std::string targetName;
            uint32 current = 0;
            uint32 required = 0;
        };
        struct QuestProgress
        {
            uint32 questId = 0;
            std::string title;
            QuestStatus status = QUEST_STATUS_NONE;
            bool explored = false;
            std::vector<QuestObjectiveProgress> objectives;
        };
        std::vector<QuestProgress> activeQuests;
        struct QuestPoi
        {
            uint32 questId = 0;
            int32 objectiveIndex = 0;
            uint32 mapId = 0;
            uint32 areaId = 0;
            Position3 pos;
            bool hasZ = false;
            bool isTurnIn = false;
        };
        std::vector<QuestPoi> questPois;
        struct NearbyEntity
        {
            std::string name;
            std::string type;
            uint32 entryId = 0;
            Position3 pos;
            float distance = 0.0f;
            bool isQuestGiver = false;
            std::string questMarker;
        };
        std::vector<NearbyEntity> nearbyEntities;
        struct QuestGiverInRange
        {
            std::string name;
            std::string type;
            uint32 entryId = 0;
            float distance = 0.0f;
            Position3 pos;
            std::vector<uint32> availableQuestIds;
            std::vector<uint32> turnInQuestIds;
            std::string questMarker;
            // Derived relevance tags for planners.
            std::vector<uint32> availableNewQuestIds;
            std::vector<uint32> turnInActiveQuestIds;
        };
        std::vector<QuestGiverInRange> questGiversInRange;
    };

    struct WorldSnapshot
    {
        // Friendly names for the current location.
        std::string zone;
        std::string area;
    };

    struct Task
    {
        // Human-readable next action for planner summaries.
        std::string description;
    };

    class Goal
    {
    public:
        // Base interface for LLM-selected goals.
        virtual ~Goal() = default;
        virtual bool IsComplete(BotSnapshot const &snapshot) const = 0;
        virtual bool IsInvalid(BotSnapshot const &snapshot) const
        {
            (void)snapshot;
            return false;
        }
        virtual bool RequiresCombat() const { return false; }
        virtual Task NextTask(BotSnapshot const &snapshot, WorldSnapshot const &world) = 0;
        virtual nlohmann::json ToJson() const = 0;
    };

    class WorldQuestGoal : public Goal
    {
    public:
        // Represents a single quest with incomplete objectives.
        explicit WorldQuestGoal(uint32 questId)
            : questId_(questId) {}

        bool IsComplete(BotSnapshot const &snapshot) const override
        {
            if (!questId_)
            {
                return false;
            }

            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.questId == questId_)
                {
                    return quest.status == QUEST_STATUS_COMPLETE;
                }
            }

            return false;
        }

        bool IsInvalid(BotSnapshot const &snapshot) const override
        {
            if (questId_ == 0)
            {
                return true;
            }

            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.questId == questId_)
                {
                    return false;
                }
            }

            return true;
        }

        Task NextTask(BotSnapshot const & /*snapshot*/, WorldSnapshot const & /*world*/) override
        {
            return Task{"world_quest"};
        }

        bool RequiresCombat() const override
        {
            return true;
        }

        nlohmann::json ToJson() const override
        {
            return nlohmann::json{
                {"type", "world_quest"},
                {"quest_id", questId_}};
        }

    private:
        uint32 questId_ = 0;
    };

    class GrindGoal : public Goal
    {
    public:
        // "Grind" is always valid and never complete by itself.
        bool IsComplete(BotSnapshot const & /*snapshot*/) const override
        {
            return false;
        }

        bool IsInvalid(BotSnapshot const & /*snapshot*/) const override
        {
            return false;
        }

        Task NextTask(BotSnapshot const & /*snapshot*/, WorldSnapshot const & /*world*/) override
        {
            return Task{"grind"};
        }

        bool RequiresCombat() const override
        {
            return true;
        }

        nlohmann::json ToJson() const override
        {
            return nlohmann::json{{"type", "grind"}};
        }
    };

    class TravelGoal : public Goal
    {
    public:
        // Travel goal references an index into navCandidates.
        explicit TravelGoal(int navTargetIndex)
            : navTargetIndex_(navTargetIndex) {}

        bool IsComplete(BotSnapshot const & /*snapshot*/) const override
        {
            return false;
        }

        bool IsInvalid(BotSnapshot const &snapshot) const override
        {
            if (navTargetIndex_ < 0)
            {
                return true;
            }
            return static_cast<size_t>(navTargetIndex_) >= snapshot.navCandidates.size();
        }

        Task NextTask(BotSnapshot const & /*snapshot*/, WorldSnapshot const & /*world*/) override
        {
            return Task{"travel"};
        }

        nlohmann::json ToJson() const override
        {
            return nlohmann::json{
                {"type", "travel"},
                {"nav_target_index", navTargetIndex_}};
        }

    private:
        int navTargetIndex_ = -1;
    };

    class TurnInGoal : public Goal
    {
    public:
        // Turn in a quest that is already complete.
        explicit TurnInGoal(uint32 questId)
            : questId_(questId) {}

        bool IsComplete(BotSnapshot const &snapshot) const override
        {
            if (!questId_)
            {
                return false;
            }

            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.questId == questId_)
                {
                    return false;
                }
            }

            return true;
        }

        bool IsInvalid(BotSnapshot const &snapshot) const override
        {
            if (questId_ == 0)
            {
                return true;
            }

            for (auto const &quest : snapshot.activeQuests)
            {
                if (quest.questId == questId_)
                {
                    return quest.status != QUEST_STATUS_COMPLETE;
                }
            }

            return true;
        }

        Task NextTask(BotSnapshot const & /*snapshot*/, WorldSnapshot const & /*world*/) override
        {
            return Task{"turn_in"};
        }

        nlohmann::json ToJson() const override
        {
            return nlohmann::json{
                {"type", "turn_in"},
                {"quest_id", questId_}};
        }

    private:
        uint32 questId_ = 0;
    };

    struct PlannerPlan
    {
        // Long-term goal and short-term goals derived from planner output.
        std::string longTermGoal;
        std::vector<std::string> shortTermGoals;
    };

    std::string TrimCopy(std::string const &input)
    {
        // Trim without modifying the original string.
        size_t start = input.find_first_not_of(" \t\r\n");
        size_t end = input.find_last_not_of(" \t\r\n");
        if (start == std::string::npos || end == std::string::npos)
        {
            return {};
        }
        return input.substr(start, end - start + 1);
    }

    class ThinkScheduler
    {
    public:
        // Simple periodic scheduler for planner/control ticks.
        bool ShouldRunStrategic(uint32 nowMs)
        {
            if (nowMs - lastStrategicMs_ >= kStrategicIntervalMs)
            {
                lastStrategicMs_ = nowMs;
                return true;
            }
            return false;
        }

        bool ShouldRunControl(uint32 nowMs, uint64 guid)
        {
            uint32 intervalMs = g_OllamaBotControlDelayControlMs > 0
                                    ? g_OllamaBotControlDelayControlMs
                                    : kControlIntervalMs;
            // Spread calls across bots to reduce thundering herd.
            uint32 jitterMs = static_cast<uint32>(guid % 500u);
            if (nowMs - lastControlMs_ >= intervalMs + jitterMs)
            {
                lastControlMs_ = nowMs;
                return true;
            }
            return false;
        }

    private:
        uint32 lastStrategicMs_ = 0;
        uint32 lastControlMs_ = 0;
    };

    struct PendingStrategicUpdate
    {
        // Planner output waiting to be applied on the main thread.
        PlannerPlan plan;
        bool hasUpdate = false;
        bool refreshedShortTermGoals = false;
    };

    std::mutex pendingMutex;
    std::unordered_map<uint64, PendingStrategicUpdate> pendingStrategicUpdates;
    std::mutex globalControlMutex;
    uint32 globalFailureWindowStartMs = 0;
    uint32 globalFailureCount = 0;
    std::atomic<uint32> globalControlPauseUntilMs{0};
    std::atomic<uint32> globalControlResumeBaseMs{0};
    std::mutex plannerSummaryLogMutex;

    std::string NormalizeCommandToken(std::string value)
    {
        // Normalize tokens for case/whitespace comparison.
        auto trim = [](std::string &text)
        {
            size_t start = text.find_first_not_of(" \t\r\n");
            size_t end = text.find_last_not_of(" \t\r\n");
            if (start == std::string::npos || end == std::string::npos)
            {
                text.clear();
                return;
            }
            text = text.substr(start, end - start + 1);
        };

        trim(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool UseCompactPromptFormat()
    {
        return NormalizeCommandToken(g_OllamaBotControlPromptFormat) == "compact";
    }

    uint64 GetNowMs()
    {
        // Monotonic clock for LLM context timestamps.
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint32 ReadEnvDelayMs(const char *name, uint32 fallback)
    {
        const char *value = std::getenv(name);
        if (!value || !*value)
            return fallback;

        char *endptr = nullptr;
        unsigned long parsed = std::strtoul(value, &endptr, 10);
        if (endptr == value)
            return fallback;

        if (parsed > std::numeric_limits<uint32>::max())
            return fallback;

        return static_cast<uint32>(parsed);
    }

    uint32 GetPlannerShortTermDelayMs()
    {
        uint32 configured = g_OllamaBotControlDelayStgMs;
        // Optional env override (no rebuild of config needed):
        //   AMIGO_PLANNER_SHORT_DELAY_MS=30000
        return ReadEnvDelayMs("AMIGO_PLANNER_SHORT_DELAY_MS", configured);
    }

    uint32 GetPlannerLongTermDelayMs()
    {
        uint32 configured = g_OllamaBotControlDelayLtgMs;
        // Optional env override:
        //   AMIGO_PLANNER_LONG_DELAY_MS=900000
        return ReadEnvDelayMs("AMIGO_PLANNER_LONG_DELAY_MS", configured);
    }

    std::string BuildPlanSummary(std::string const &longTermGoal,
                                 std::vector<std::string> const &shortTermGoals,
                                 size_t shortTermIndex)
    {
        // Summarize the current plan for logs/debug output.
        std::ostringstream oss;
        if (!longTermGoal.empty())
        {
            oss << "long_term_goal: " << longTermGoal;
        }
        else
        {
            oss << "long_term_goal: none";
        }
        if (!shortTermGoals.empty())
        {
            size_t index = std::min(shortTermIndex, shortTermGoals.size() - 1);
            oss << " | short_term_goal (" << (index + 1) << "/" << shortTermGoals.size() << "): "
                << shortTermGoals[index];
        }
        return oss.str();
    }

    std::string ExtractPlannerSentence(std::string const &reply)
    {
        // Pull the first non-empty line from a planner response.
        std::istringstream iss(reply);
        std::string line;
        while (std::getline(iss, line))
        {
            line = TrimCopy(line);
            if (!line.empty())
            {
                if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
                {
                    line = line.substr(1, line.size() - 2);
                }
                return TrimCopy(line);
            }
        }
        return {};
    }

    bool LooksLikeJsonOrToolBlock(std::string const &text)
    {
        std::string s = TrimCopy(text);
        if (s.empty())
        {
            return false;
        }
        // Tool blocks.
        if (s.find("<tool_call>") != std::string::npos || s.find("</tool_call>") != std::string::npos)
        {
            return true;
        }
        // JSON-like structures or controller-style payloads.
        if (s.find('{') != std::string::npos || s.find('[') != std::string::npos)
        {
            return true;
        }
        // Common JSON fields seen in tool calls.
        if (s.find("\"name\"") != std::string::npos || s.find("\"arguments\"") != std::string::npos)
        {
            return true;
        }
        return false;
    }

    bool LooksLikeListItem(std::string const &text)
    {
        std::string s = TrimCopy(text);
        if (s.empty())
        {
            return false;
        }
        if (s[0] == '-' || s[0] == '*')
        {
            return true;
        }
        // "1. foo" or "1) foo"
        size_t dotPos = s.find('.');
        if (dotPos != std::string::npos && dotPos > 0 && dotPos <= 3)
        {
            bool numeric = true;
            for (size_t i = 0; i < dotPos; ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(s[i])))
                {
                    numeric = false;
                    break;
                }
            }
            if (numeric)
            {
                return true;
            }
        }
        size_t parenPos = s.find(')');
        if (parenPos != std::string::npos && parenPos > 0 && parenPos <= 3)
        {
            bool numeric = true;
            for (size_t i = 0; i < parenPos; ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(s[i])))
                {
                    numeric = false;
                    break;
                }
            }
            if (numeric)
            {
                return true;
            }
        }
        return false;
    }

    size_t CountSentenceTerminators(std::string const &text)
    {
        size_t count = 0;
        bool inTerminatorRun = false;
        for (char c : text)
        {
            if (c == '.' || c == '!' || c == '?')
            {
                if (!inTerminatorRun)
                {
                    count += 1;
                    inTerminatorRun = true;
                }
            }
            else
            {
                inTerminatorRun = false;
            }
        }
        return count;
    }

    std::string StripListPrefix(std::string const &text)
    {
        std::string s = TrimCopy(text);
        if (s.empty())
        {
            return s;
        }
        if (s[0] == '-' || s[0] == '*')
        {
            size_t i = 1;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            {
                ++i;
            }
            return TrimCopy(s.substr(i));
        }
        // "1. foo" or "1) foo" or "(1) foo"
        if (s.size() >= 3 && s[0] == '(')
        {
            size_t close = s.find(')');
            if (close != std::string::npos && close > 1 && close <= 4)
            {
                bool numeric = true;
                for (size_t i = 1; i < close; ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(s[i])))
                    {
                        numeric = false;
                        break;
                    }
                }
                if (numeric)
                {
                    size_t i = close + 1;
                    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                    {
                        ++i;
                    }
                    return TrimCopy(s.substr(i));
                }
            }
        }
        size_t dotPos = s.find('.');
        if (dotPos != std::string::npos && dotPos > 0 && dotPos <= 3)
        {
            bool numeric = true;
            for (size_t i = 0; i < dotPos; ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(s[i])))
                {
                    numeric = false;
                    break;
                }
            }
            if (numeric)
            {
                size_t i = dotPos + 1;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                {
                    ++i;
                }
                return TrimCopy(s.substr(i));
            }
        }
        size_t parenPos = s.find(')');
        if (parenPos != std::string::npos && parenPos > 0 && parenPos <= 3)
        {
            bool numeric = true;
            for (size_t i = 0; i < parenPos; ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(s[i])))
                {
                    numeric = false;
                    break;
                }
            }
            if (numeric)
            {
                size_t i = parenPos + 1;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                {
                    ++i;
                }
                return TrimCopy(s.substr(i));
            }
        }
        return s;
    }

    bool ValidatePlannerSentence(std::string const& text, std::string& outReason)
    {
        std::string s = StripListPrefix(text);
        if (s.empty())
        {
            outReason = "empty";
            return false;
        }
        if (s.size() > 220)
        {
            outReason = "too_long";
            return false;
        }
        if (LooksLikeJsonOrToolBlock(s))
        {
            outReason = "json_or_tool";
            return false;
        }
        if (CountSentenceTerminators(s) > 1)
        {
            outReason = "multi_sentence";
            return false;
        }
        if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos)
        {
            outReason = "contains_newlines";
            return false;
        }
        outReason.clear();
        return true;
    }

    bool ValidateShortTermGoal(std::string const& text, std::string& outReason)
    {
        std::string s = StripListPrefix(text);
        if (s.empty())
        {
            outReason = "empty";
            return false;
        }
        if (s.size() > 260)
        {
            outReason = "too_long";
            return false;
        }
        if (LooksLikeJsonOrToolBlock(s))
        {
            outReason = "json_or_tool";
            return false;
        }
        if (CountSentenceTerminators(s) > 1)
        {
            outReason = "multi_sentence";
            return false;
        }
        if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos)
        {
            outReason = "contains_newlines";
            return false;
        }
        outReason.clear();
        return true;
    }

    std::string ParseShortTermGoal(std::string const& reply)
    {
        // Short-term planner is expected to return exactly one non-empty line.
        return ExtractPlannerSentence(reply);
    }

    std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool ContainsInsensitive(std::string const &haystack, std::string const &needle)
    {
        if (needle.empty())
        {
            return false;
        }
        std::string hay = ToLowerCopy(haystack);
        std::string ned = ToLowerCopy(needle);
        return hay.find(ned) != std::string::npos;
    }

    std::string QuestStatusToString(QuestStatus status);

    BotSnapshot::QuestProgress const *FindFocusQuest(BotSnapshot const &bot, std::string const &longTermGoal)
    {
        BotSnapshot::QuestProgress const *best = nullptr;
        size_t bestLen = 0;
        for (auto const &quest : bot.activeQuests)
        {
            if (quest.title.empty())
            {
                continue;
            }
            if (ContainsInsensitive(longTermGoal, quest.title))
            {
                if (quest.title.size() > bestLen)
                {
                    best = &quest;
                    bestLen = quest.title.size();
                }
            }
        }
        return best;
    }

    std::string BuildFocusQuestBlock(BotSnapshot::QuestProgress const &quest)
    {
        std::ostringstream oss;
        oss << "title: " << quest.title << "\n";
        oss << "status: " << QuestStatusToString(quest.status) << "\n";
        if (!quest.objectives.empty())
        {
            oss << "objectives:\n";
            for (auto const &objective : quest.objectives)
            {
                oss << "- " << objective.type;
                if (!objective.targetName.empty())
                {
                    oss << " " << objective.targetName;
                }
                else if (objective.targetId != 0)
                {
                    oss << " " << objective.targetId;
                }
                oss << " " << objective.current << "/" << objective.required << "\n";
            }
        }
        return oss.str();
    }

    bool MentionsOtherQuest(std::string const &text,
                            std::vector<BotSnapshot::QuestProgress> const &quests,
                            std::string const &focusTitle)
    {
        for (auto const &quest : quests)
        {
            if (quest.title.empty())
            {
                continue;
            }
            if (!focusTitle.empty() && quest.title == focusTitle)
            {
                continue;
            }
            if (ContainsInsensitive(text, quest.title))
            {
                return true;
            }
        }
        return false;
    }

    std::string CurrentShortTermGoal(std::vector<std::string> const &goals, size_t index)
    {
        if (goals.empty())
        {
            return {};
        }
        size_t clamped = std::min(index, goals.size() - 1);
        return goals[clamped];
    }

    std::string SummarizeControlAction(ControlAction const &action)
    {
        // Summary text stored in the LLM context for debugging.
        std::ostringstream oss;
        switch (action.capability)
        {
        case ControlAction::Capability::MoveHop:
            oss << "move_hop"
                << " nav_epoch=" << action.navEpoch;
            if (!action.navCandidateId.empty())
            {
                oss << " candidate_id=" << action.navCandidateId;
            }
            break;
        case ControlAction::Capability::EnterGrind:
            oss << "enter_grind";
            break;
        case ControlAction::Capability::StopGrind:
            oss << "stop_grind";
            break;
        case ControlAction::Capability::Stay:
            oss << "stay";
            break;
        case ControlAction::Capability::Unstay:
            oss << "unstay";
            break;
        case ControlAction::Capability::TalkToQuestGiver:
            oss << "talk_to_quest_giver";
            if (action.questId > 0)
            {
                oss << " quest_id=" << action.questId;
            }
            break;
        case ControlAction::Capability::Fish:
            oss << "fish";
            break;
        case ControlAction::Capability::UseProfession:
            oss << "profession";
            if (!action.professionSkill.empty())
            {
                oss << " skill=" << action.professionSkill;
            }
            if (!action.professionIntent.empty())
            {
                oss << " intent=" << action.professionIntent;
            }
            break;
        case ControlAction::Capability::Idle:
        default:
            oss << "idle";
            break;
        }
        return oss.str();
    }

    std::string NormalizeAreaToken(std::string value)
    {
        // Normalize zone/area names for compact JSON fields.
        std::string output;
        output.reserve(value.size());
        bool lastWasUnderscore = false;
        for (unsigned char c : value)
        {
            if (std::isalnum(c))
            {
                output.push_back(static_cast<char>(std::tolower(c)));
                lastWasUnderscore = false;
            }
            else if (!lastWasUnderscore)
            {
                output.push_back('_');
                lastWasUnderscore = true;
            }
        }

        while (!output.empty() && output.front() == '_')
        {
            output.erase(output.begin());
        }
        while (!output.empty() && output.back() == '_')
        {
            output.pop_back();
        }
        if (output.empty())
        {
            output = "unknown";
        }
        return output;
    }

    std::string QuestStatusToString(QuestStatus status)
    {
        // Convert enum to a stable string for LLM consumption.
        switch (status)
        {
        case QUEST_STATUS_INCOMPLETE:
            return "incomplete";
        case QUEST_STATUS_COMPLETE:
            return "complete";
        case QUEST_STATUS_FAILED:
            return "failed";
        case QUEST_STATUS_REWARDED:
            return "rewarded";
        case QUEST_STATUS_NONE:
        default:
            return "none";
        }
    }

    bool IsFollowingCorrectly(Player *bot, PlayerbotAI *ai)
    {
        // Check follow distance against Playerbot config.
        if (!bot || !ai)
        {
            return false;
        }

        Player *master = ai->GetMaster();
        if (!master || !master->IsInWorld())
        {
            return false;
        }

        if (master == bot)
        {
            return true;
        }

        return bot->IsWithinDistInMap(master, sPlayerbotAIConfig.followDistance);
    }

    struct ToolCall
    {
        // Parsed tool call output from the control LLM.
        std::string name;
        nlohmann::json arguments;
    };

    struct ControlToolDefinition
    {
        // Control tool metadata for validation and mapping.
        const char *name;
        const char *signature;
        ControlAction::Capability capability;
        bool requiresDirection;
        bool requiresDistance;
        bool requiresQuestId;
        bool requiresSkill;
        bool requiresIntent;
        bool requiresMessage;
        bool requiresNavEpoch;
        bool requiresCandidateId;
    };

    const std::array<ControlToolDefinition, 12> kControlTools = {
        ControlToolDefinition{
            "request_idle",
            "request_idle()",
            ControlAction::Capability::Idle,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_move_hop",
            "request_move_hop(nav_epoch, candidate_id)",
            ControlAction::Capability::MoveHop,
            false,
            false,
            false,
            false,
            false,
            false,
            true,
            true},
        ControlToolDefinition{
            "request_enter_grind",
            "request_enter_grind()",
            ControlAction::Capability::EnterGrind,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_stop_grind",
            "request_stop_grind()",
            ControlAction::Capability::StopGrind,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_stay",
            "request_stay()",
            ControlAction::Capability::Stay,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_unstay",
            "request_unstay()",
            ControlAction::Capability::Unstay,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_talk_to_quest_giver",
            "request_talk_to_quest_giver(quest_id)",
            ControlAction::Capability::TalkToQuestGiver,
            false,
            false,
            true,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_fish",
            "request_fish()",
            ControlAction::Capability::Fish,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_profession",
            "request_profession(skill, intent)",
            ControlAction::Capability::UseProfession,
            false,
            false,
            false,
            true,
            true,
            false,
            false,
            false},
        // Turning tools added for precise orientation changes.
        ControlToolDefinition{
            "request_turn_left_90",
            "request_turn_left_90()",
            ControlAction::Capability::TurnLeft90,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_turn_right_90",
            "request_turn_right_90()",
            ControlAction::Capability::TurnRight90,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false},
        ControlToolDefinition{
            "request_turn_around",
            "request_turn_around()",
            ControlAction::Capability::TurnAround,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            false}};

    bool TryExtractToolCall(std::string const &reply, ToolCall &outCall, std::string &outJson)
    {
        // Parse the first <tool_call> block found in the response.
        static std::string const kStartTag = "<tool_call>";
        static std::string const kEndTag = "</tool_call>";

        size_t start = reply.find(kStartTag);
        if (start == std::string::npos)
        {
            return false;
        }
        size_t end = reply.find(kEndTag, start + kStartTag.size());
        if (end == std::string::npos)
        {
            return false;
        }

        size_t contentStart = start + kStartTag.size();
        std::string inner = TrimCopy(reply.substr(contentStart, end - contentStart));
        if (inner.empty())
        {
            return false;
        }

        try
        {
            nlohmann::json parsed = nlohmann::json::parse(inner);
            if (!parsed.contains("name") || !parsed["name"].is_string())
            {
                return false;
            }
            outCall.name = NormalizeCommandToken(parsed["name"].get<std::string>());
            if (parsed.contains("arguments"))
            {
                outCall.arguments = parsed["arguments"];
            }
            else
            {
                outCall.arguments = nlohmann::json::object();
            }
            outJson = parsed.dump(2);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool TryExtractSingleToolCall(std::string const &reply, ToolCall &outCall, std::string &outJson)
    {
        // Ensure the output is exactly one tool call block.
        static std::string const kStartTag = "<tool_call>";
        static std::string const kEndTag = "</tool_call>";

        std::string trimmed = TrimCopy(reply);
        if (trimmed.rfind(kStartTag, 0) != 0)
        {
            return false;
        }
        if (trimmed.size() < kEndTag.size() || trimmed.find(kEndTag) != trimmed.size() - kEndTag.size())
        {
            return false;
        }

        return TryExtractToolCall(trimmed, outCall, outJson);
    }

    bool FindControlToolDefinition(std::string const &name, ControlToolDefinition &outDefinition)
    {
        // Look up tool metadata by name.
        for (const auto &tool : kControlTools)
        {
            if (name == tool.name)
            {
                outDefinition = tool;
                return true;
            }
        }
        return false;
    }

    bool ParseProfessionArguments(nlohmann::json const &arguments, std::string &outSkill, std::string &outIntent)
    {
        outSkill.clear();
        outIntent.clear();

        if (!arguments.is_object())
        {
            return false;
        }
        if (!arguments.contains("skill") || !arguments["skill"].is_string())
        {
            return false;
        }
        if (!arguments.contains("intent") || !arguments["intent"].is_string())
        {
            return false;
        }

        outSkill = NormalizeCommandToken(arguments["skill"].get<std::string>());
        outIntent = NormalizeCommandToken(arguments["intent"].get<std::string>());
        return !outSkill.empty() && !outIntent.empty();
    }

    static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
    {
        // cURL write callback for accumulating response payloads.
        std::string *responseBuffer = static_cast<std::string *>(userp);
        size_t totalSize = size * nmemb;
        responseBuffer->append(static_cast<char *>(contents), totalSize);
        return totalSize;
    }

    const char *DescribeControlTool(const char *name)
    {
        // Short descriptions used in the control prompt.
        if (std::strcmp(name, "request_idle") == 0)
        {
            return "wait for the next update";
        }
        if (std::strcmp(name, "request_move_hop") == 0)
        {
            return "move using a server-provided navigation candidate (by ID)";
        }
        if (std::strcmp(name, "request_enter_grind") == 0)
        {
            return "fight nearby mobs (grind / quest objectives)";
        }
        if (std::strcmp(name, "request_stop_grind") == 0)
        {
            return "stop grinding and resume normal movement/follow";
        }
        if (std::strcmp(name, "request_stay") == 0)
        {
            return "stay in place until told otherwise";
        }
        if (std::strcmp(name, "request_unstay") == 0)
        {
            return "resume normal movement (clear stay)";
        }
        if (std::strcmp(name, "request_talk_to_quest_giver") == 0)
        {
            return "talk to a quest giver in range (accept or turn in a quest)";
        }
        if (std::strcmp(name, "request_fish") == 0)
        {
            return "perform fishing from the current spot (no movement)";
        }
        if (std::strcmp(name, "request_profession") == 0)
        {
            return "perform a profession-related action (requires bot to have that skill)";
        }
        if (std::strcmp(name, "request_turn_left_90") == 0)
        {
            return "turn left 90 degrees";
        }
        if (std::strcmp(name, "request_turn_right_90") == 0)
        {
            return "turn right 90 degrees";
        }
        if (std::strcmp(name, "request_turn_around") == 0)
        {
            return "turn around 180 degrees";
        }
        return "";
    }

    std::string BuildControlToolList(std::string const &prefix)
    {
        // Build a bullet list of tools for the LLM prompt.
        std::ostringstream oss;
        for (size_t i = 0; i < kControlTools.size(); ++i)
        {
            const auto &tool = kControlTools[i];
            oss << prefix << tool.signature;
            const char *description = DescribeControlTool(tool.name);
            if (description && description[0] != '\0')
            {
                oss << " — " << description;
            }
            if (i + 1 < kControlTools.size())
            {
                oss << "\n";
            }
        }
        return oss.str();
    }

    std::string QueryOllamaLLMOnce(std::string const &prompt, std::string const &model)
    {
        // Blocking LLM request used by planner/control threads.
        constexpr long kOllamaConnectTimeoutMs = 5000;
        constexpr long kOllamaRequestTimeoutMs = 120000;

        if (model.empty())
        {
            LOG_ERROR("server.loading", "[OllamaBotAmigo] Missing Ollama model for request.");
            return "";
        }
        std::string resolvedModel = model;
        CURL *curl = curl_easy_init();
        if (!curl)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Failed to initialize cURL.");
            return "";
        }

        nlohmann::json requestData = {
            {"model", resolvedModel},
            {"prompt", prompt}};
        std::string requestDataStr = requestData.dump();

        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        std::string responseBuffer;
        curl_easy_setopt(curl, CURLOPT_URL, g_OllamaBotControlUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestDataStr.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(requestDataStr.length()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kOllamaConnectTimeoutMs);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kOllamaRequestTimeoutMs);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Failed to reach Ollama AI. cURL error: {}", curl_easy_strerror(res));
            return "";
        }

        std::stringstream ss(responseBuffer);
        std::string line;
        std::string extracted;
        while (std::getline(ss, line))
        {
            try
            {
                nlohmann::json jsonResponse = nlohmann::json::parse(line);
                if (jsonResponse.contains("response"))
                {
                    extracted += jsonResponse["response"].get<std::string>();
                }
            }
            catch (...)
            {
            }
        }
        return extracted;
    }

    std::string BuildControlToolInstructions(std::string const &stateToken)
    {
        // Shared instruction block appended to control prompts.
        std::ostringstream oss;
        oss << "Available control tools (choose exactly one):\n";
        oss << BuildControlToolList("- ");
        oss << R"(

Rules:
- Output exactly one <tool_call> block and nothing else.
- request_move_hop: choose a candidate from )";
        oss << stateToken;
        oss << R"(.nav.candidates by its candidate_id, and echo )";
        oss << stateToken;
        oss << R"(.nav.nav_epoch.
  Only choose candidates where can_move is true (and preferably reachable is true).
- request_talk_to_quest_giver: quest_id must be in )";
        oss << stateToken;
        oss << R"(.quest_givers_in_range entries (available_quest_ids or turn_in_quest_ids).
- If )";
        oss << stateToken;
        oss << R"(.quest_givers_in_range is not empty, prioritize request_talk_to_quest_giver.
- request_stop_grind: call this when )";
        oss << stateToken;
        oss << R"(.bot.grind_mode is true and you need to travel/quest/talk; it disables grinding.
- request_profession: skill must be a profession/secondary skill name (e.g. \"fishing\", \"mining\", \"skinning\").
  intent describes what you want to do with the skill (e.g. \"fish\", \"gather\", \"craft\").
- If no valid control action exists, call request_idle.

Tool call format:
<tool_call>
{"name":"request_idle","arguments":{}}
</tool_call>

request_move_hop format:
<tool_call>
{"name":"request_move_hop","arguments":{"nav_epoch":42,"candidate_id":"nav_0"}}
</tool_call>

request_profession format:
<tool_call>
{"name":"request_profession","arguments":{"skill":"fishing","intent":"fish"}}
</tool_call>
)";
        return oss.str();
    }

    const char *CapabilityName(ControlAction::Capability capability)
    {
        // Human-readable labels for logging and summaries.
        switch (capability)
        {
        case ControlAction::Capability::Idle:
            return "idle";
        case ControlAction::Capability::MoveHop:
            return "move_hop";
        case ControlAction::Capability::EnterGrind:
            return "enter_grind";
        case ControlAction::Capability::StopGrind:
            return "stop_grind";
        case ControlAction::Capability::EnterAttackPull:
            return "enter_attack_pull";
        case ControlAction::Capability::Stay:
            return "stay";
        case ControlAction::Capability::Unstay:
            return "unstay";
        case ControlAction::Capability::TalkToQuestGiver:
            return "talk_to_quest_giver";
        case ControlAction::Capability::Fish:
            return "fish";
        case ControlAction::Capability::UseProfession:
            return "profession";
        case ControlAction::Capability::TurnLeft90:
            return "turn_left_90";
        case ControlAction::Capability::TurnRight90:
            return "turn_right_90";
        case ControlAction::Capability::TurnAround:
            return "turn_around";
        default:
            return "unknown";
        }
    }

    float Distance2d(Position3 const &a, Position3 const &b);

    float BearingDegrees(Position3 const &from, Position3 const &to);

    std::string DirectionLabelFromBearing(float bearingDeg);

    std::vector<BotSnapshot::NavCandidate> BuildNavCandidates(Player *bot)
    {
        // Build forward/back/left/right candidates around the bot.
        std::vector<BotSnapshot::NavCandidate> candidates;
        if (!bot)
        {
            return candidates;
        }

        float baseDistance = std::max(g_OllamaBotControlNavBaseDistance, sPlayerbotAIConfig.followDistance);
        if (!(baseDistance > 0.0f))
        {
            baseDistance = 6.0f;
        }
        float distanceMultiplier = g_OllamaBotControlNavDistanceMultiplier;
        if (!(distanceMultiplier > 1.0f))
        {
            distanceMultiplier = 2.0f;
        }
        float maxDistance = g_OllamaBotControlNavMaxDistance;
        if (!(maxDistance > 0.0f))
        {
            maxDistance = 60.0f;
        }
        uint32 bands = g_OllamaBotControlNavDistanceBands;
        if (bands < 1)
        {
            bands = 1;
        }
        if (bands > 6)
        {
            bands = 6;
        }
        float const orientation = bot->GetOrientation();
        float const cosO = std::cos(orientation);
        float const sinO = std::sin(orientation);

        Position3 origin{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        Map *map = bot->GetMap();
        uint32 mapId = bot->GetMapId();

        auto addCandidate = [&](std::string label, float dx, float dy)
        {
            BotSnapshot::NavCandidate candidate;
            candidate.label = std::move(label);
            float x = origin.x + dx;
            float y = origin.y + dy;
            float z = origin.z;

            // Resolve a ground/water Z at the candidate X/Y to avoid "mid-air" points.
            if (map)
            {
                float height = map->GetHeight(x, y, MAX_HEIGHT);
                float water = map->GetWaterLevel(x, y);
                float candidateZ = std::max(height, water);
                if (candidateZ != INVALID_HEIGHT)
                    z = candidateZ;
            }

            candidate.pos = Position3{x, y, z};

            // Derived, engine-backed feasibility signals.
            WorldPosition wp(mapId, x, y, z);
            candidate.hasLOS = WorldChecks::IsWithinLOS(bot, wp);
            candidate.reachable = WorldChecks::CanReach(bot, wp);

            // Presentation helpers for the LLM.
            candidate.distance2d = Distance2d(origin, candidate.pos);
            candidate.bearingDeg = BearingDegrees(origin, candidate.pos);
            candidate.direction = DirectionLabelFromBearing(candidate.bearingDeg);
            candidates.push_back(std::move(candidate));
        };

        std::vector<float> distances;
        distances.reserve(bands);
        float current = baseDistance;
        for (uint32 i = 0; i < bands; ++i)
        {
            distances.push_back(std::min(current, maxDistance));
            current *= distanceMultiplier;
        }

        float const diagScale = 0.70710677f;

        for (float dist : distances)
        {
            float fwdX = dist * cosO;
            float fwdY = dist * sinO;
            float rightX = dist * sinO;
            float rightY = -dist * cosO;

            addCandidate("forward", fwdX, fwdY);
            addCandidate("backward", -fwdX, -fwdY);
            addCandidate("left", -rightX, -rightY);
            addCandidate("right", rightX, rightY);

            float diagX = fwdX * diagScale;
            float diagY = fwdY * diagScale;
            float diagRX = rightX * diagScale;
            float diagRY = rightY * diagScale;

            addCandidate("forward_left", diagX - diagRX, diagY - diagRY);
            addCandidate("forward_right", diagX + diagRX, diagY + diagRY);
            addCandidate("backward_left", -diagX - diagRX, -diagY - diagRY);
            addCandidate("backward_right", -diagX + diagRX, -diagY + diagRY);
        }

        return candidates;
    }

    void AppendQuestGiverNavCandidates(Player *bot,
                                       std::vector<BotSnapshot::NearbyEntity> const &nearbyEntities,
                                       std::vector<BotSnapshot::NavCandidate> &candidates,
                                       size_t maxTargets = 4)
    {
        if (!bot || nearbyEntities.empty())
        {
            return;
        }

        std::vector<BotSnapshot::NearbyEntity> questGivers;
        questGivers.reserve(nearbyEntities.size());
        for (auto const &entity : nearbyEntities)
        {
            if (entity.type == "npc" && entity.isQuestGiver && !entity.questMarker.empty())
            {
                questGivers.push_back(entity);
            }
        }
        if (questGivers.empty())
        {
            return;
        }

        std::sort(questGivers.begin(), questGivers.end(),
                  [](BotSnapshot::NearbyEntity const &a, BotSnapshot::NearbyEntity const &b)
                  {
                      return a.distance < b.distance;
                  });
        if (questGivers.size() > maxTargets)
        {
            questGivers.resize(maxTargets);
        }

        Position3 origin{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        Map *map = bot->GetMap();
        uint32 mapId = bot->GetMapId();
        float maxDistance = g_OllamaBotControlNavMaxDistance > 0.0f ? g_OllamaBotControlNavMaxDistance : 60.0f;

        for (auto const &entity : questGivers)
        {
            float dx = entity.pos.x - origin.x;
            float dy = entity.pos.y - origin.y;
            float dist2d = std::sqrt(dx * dx + dy * dy);
            if (dist2d <= 0.1f)
            {
                continue;
            }

            float step = std::min(dist2d, maxDistance);
            if (dist2d > kQuestGiverApproachOffsetMeters)
            {
                step = std::min(dist2d - kQuestGiverApproachOffsetMeters, maxDistance);
            }
            else
            {
                // Already close enough; avoid suggesting moves that clip into the quest giver.
                continue;
            }
            float dirX = dx / dist2d;
            float dirY = dy / dist2d;
            float x = origin.x + dirX * step;
            float y = origin.y + dirY * step;
            float z = origin.z;

            if (map)
            {
                float height = map->GetHeight(x, y, MAX_HEIGHT);
                float water = map->GetWaterLevel(x, y);
                float candidateZ = std::max(height, water);
                if (candidateZ != INVALID_HEIGHT)
                    z = candidateZ;
            }

            BotSnapshot::NavCandidate candidate;
            std::string label = "quest_giver";
            label += entity.questMarker;
            if (!entity.name.empty())
            {
                label += "_";
                label += NormalizeAreaToken(entity.name);
            }
            candidate.label = std::move(label);
            candidate.pos = Position3{x, y, z};

            WorldPosition wp(mapId, x, y, z);
            candidate.hasLOS = WorldChecks::IsWithinLOS(bot, wp);
            candidate.reachable = WorldChecks::CanReach(bot, wp);
            candidate.distance2d = Distance2d(origin, candidate.pos);
            candidate.bearingDeg = BearingDegrees(origin, candidate.pos);
            candidate.direction = DirectionLabelFromBearing(candidate.bearingDeg);
            candidates.push_back(std::move(candidate));
        }
    }

    float Distance(Position3 const &a, Position3 const &b)
    {
        // 3D Euclidean distance helper.
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float Distance2d(Position3 const &a, Position3 const &b)
    {
        // 2D distance helper for map-based calculations.
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    float BearingDegrees(Position3 const &from, Position3 const &to)
    {
        // Compass bearing in degrees, 0 = east, 90 = north.
        float dx = to.x - from.x;
        float dy = to.y - from.y;
        constexpr float kPi = 3.14159265f;
        float angle = std::atan2(dy, dx) * 180.0f / kPi;
        if (angle < 0.0f)
        {
            angle += 360.0f;
        }
        return angle;
    }

    std::string DirectionLabelFromBearing(float bearingDeg)
    {
        // Map a bearing angle to a coarse cardinal label.
        static constexpr std::array<const char *, 8> kDirections = {
            "east",
            "northeast",
            "north",
            "northwest",
            "west",
            "southwest",
            "south",
            "southeast"};

        float normalized = std::fmod(bearingDeg, 360.0f);
        if (normalized < 0.0f)
        {
            normalized += 360.0f;
        }
        int index = static_cast<int>(std::round(normalized / 45.0f)) % 8;
        return kDirections[static_cast<size_t>(index)];
    }

    std::vector<BotSnapshot::QuestGiverInRange> BuildQuestGiversInRange(Player *bot, PlayerbotAI *ai)
    {
        // Find quest givers that can offer or turn in quests.
        std::vector<BotSnapshot::QuestGiverInRange> results;
        if (!bot || !ai)
        {
            return results;
        }

        AiObjectContext *context = ai->GetAiObjectContext();
        if (!context)
        {
            return results;
        }

        auto addQuestGiver = [&](WorldObject *questGiver,
                                 std::string const &typeLabel,
                                 QuestRelationBounds offeredBounds,
                                 QuestRelationBounds involvedBounds)
        {
            if (!questGiver || !bot->CanInteractWithQuestGiver(questGiver))
            {
                return;
            }

            std::vector<uint32> availableIds;
            for (auto it = offeredBounds.first; it != offeredBounds.second; ++it)
            {
                uint32 questId = it->second;
                if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE)
                {
                    continue;
                }
                Quest const *quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest)
                {
                    continue;
                }
                if (!bot->CanTakeQuest(quest, false))
                {
                    continue;
                }
                availableIds.push_back(questId);
            }

            std::vector<uint32> turnInIds;
            for (auto it = involvedBounds.first; it != involvedBounds.second; ++it)
            {
                uint32 questId = it->second;
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                {
                    turnInIds.push_back(questId);
                }
            }

            if (availableIds.empty() && turnInIds.empty())
            {
                return;
            }

            BotSnapshot::QuestGiverInRange candidate;
            candidate.type = typeLabel;
            candidate.distance = bot->GetDistance(questGiver);
            candidate.pos = Position3{questGiver->GetPositionX(), questGiver->GetPositionY(), questGiver->GetPositionZ()};
            candidate.availableQuestIds = std::move(availableIds);
            candidate.turnInQuestIds = std::move(turnInIds);
            // Compute relevance relative to current quest log (helps prevent planner picking unrelated NPCs).
            for (uint32 questId : candidate.availableQuestIds)
            {
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                {
                    candidate.availableNewQuestIds.push_back(questId);
                }
            }
            for (uint32 questId : candidate.turnInQuestIds)
            {
                if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                {
                    candidate.turnInActiveQuestIds.push_back(questId);
                }
            }
            if (!candidate.turnInQuestIds.empty())
            {
                candidate.questMarker = "?";
            }
            else if (!candidate.availableQuestIds.empty())
            {
                candidate.questMarker = "!";
            }

            if (Creature *creature = questGiver->ToCreature())
            {
                candidate.entryId = creature->GetEntry();
                candidate.name = creature->GetName();
            }
            else if (GameObject *gameObject = questGiver->ToGameObject())
            {
                candidate.entryId = gameObject->GetEntry();
                candidate.name = gameObject->GetName();
            }

            results.push_back(std::move(candidate));
        };

        GuidVector npcs = context->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const &guid : npcs)
        {
            Creature *creature = ai->GetCreature(guid);
            if (!creature || !creature->IsQuestGiver())
            {
                continue;
            }

            QuestRelationBounds offered = sObjectMgr->GetCreatureQuestRelationBounds(creature->GetEntry());
            QuestRelationBounds involved = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(creature->GetEntry());
            addQuestGiver(creature, "npc", offered, involved);
        }

        GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
        for (ObjectGuid const &guid : gos)
        {
            GameObject *gameObject = ai->GetGameObject(guid);
            if (!gameObject || gameObject->GetGoType() != GAMEOBJECT_TYPE_QUESTGIVER)
            {
                continue;
            }

            QuestRelationBounds offered = sObjectMgr->GetGOQuestRelationBounds(gameObject->GetEntry());
            QuestRelationBounds involved = sObjectMgr->GetGOQuestInvolvedRelationBounds(gameObject->GetEntry());
            addQuestGiver(gameObject, "game_object", offered, involved);
        }

        return results;
    }

    std::vector<BotSnapshot::NearbyEntity> BuildNearbyEntities(Player *bot, PlayerbotAI *ai)
    {
        // Collect nearby NPCs and game objects for context.
        std::vector<BotSnapshot::NearbyEntity> results;
        if (!bot || !ai)
        {
            return results;
        }

        AiObjectContext *context = ai->GetAiObjectContext();
        if (!context)
        {
            return results;
        }

        GuidVector npcs = context->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const &guid : npcs)
        {
            Creature *creature = ai->GetCreature(guid);
            if (!creature)
            {
                continue;
            }

            BotSnapshot::NearbyEntity entity;
            entity.type = "npc";
            entity.entryId = creature->GetEntry();
            entity.name = creature->GetName();
            entity.pos = Position3{creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ()};
            entity.distance = bot->GetDistance(creature);
            entity.isQuestGiver = creature->IsQuestGiver();
            if (entity.isQuestGiver)
            {
                QuestRelationBounds startBounds = sObjectMgr->GetCreatureQuestRelationBounds(creature->GetEntry());
                QuestRelationBounds endBounds = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(creature->GetEntry());
                bool hasTurnIn = false;
                bool hasAvailable = false;
                for (auto it = endBounds.first; it != endBounds.second; ++it)
                {
                    uint32 questId = it->second;
                    if (bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                    {
                        hasTurnIn = true;
                        break;
                    }
                }
                if (!hasTurnIn)
                {
                    for (auto it = startBounds.first; it != startBounds.second; ++it)
                    {
                        uint32 questId = it->second;
                        if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                        {
                            hasAvailable = true;
                            break;
                        }
                    }
                }
                if (hasTurnIn)
                {
                    entity.questMarker = "?";
                }
                else if (hasAvailable)
                {
                    entity.questMarker = "!";
                }
            }
            results.push_back(std::move(entity));
        }

        GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
        for (ObjectGuid const &guid : gos)
        {
            GameObject *gameObject = ai->GetGameObject(guid);
            if (!gameObject)
            {
                continue;
            }

            std::string name = gameObject->GetName();
            if (!ContainsInsensitive(name, "fire") && !ContainsInsensitive(name, "brazier") &&
                !ContainsInsensitive(name, "torch") && !ContainsInsensitive(name, "flame"))
            {
                continue;
            }

            BotSnapshot::NearbyEntity entity;
            entity.type = "game_object";
            entity.entryId = gameObject->GetEntry();
            entity.name = gameObject->GetName();
            entity.pos = Position3{gameObject->GetPositionX(), gameObject->GetPositionY(), gameObject->GetPositionZ()};
            entity.distance = bot->GetDistance(gameObject);
            entity.isQuestGiver = false;
            results.push_back(std::move(entity));
        }

        return results;
    }

    std::vector<BotSnapshot::QuestPoi> BuildQuestPois(Player *bot)
    {
        // Build quest POIs for active objectives in the current map.
        std::vector<BotSnapshot::QuestPoi> results;
        if (!bot)
        {
            return results;
        }

        Map *map = bot->GetMap();
        for (auto const &entry : bot->getQuestStatusMap())
        {
            uint32 questId = entry.first;
            QuestStatusData const &statusData = entry.second;
            if (statusData.Status != QUEST_STATUS_INCOMPLETE && statusData.Status != QUEST_STATUS_COMPLETE)
            {
                continue;
            }

            Quest const *quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
            {
                continue;
            }

            QuestPOIVector const *poiVector = sObjectMgr->GetQuestPOIVector(questId);
            if (!poiVector)
            {
                continue;
            }

            std::vector<int32> incompleteObjectiveIdx;
            if (statusData.Status == QUEST_STATUS_INCOMPLETE)
            {
                for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                {
                    if (quest->RequiredNpcOrGoCount[i] > 0 && statusData.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
                    {
                        incompleteObjectiveIdx.push_back(i);
                    }
                }
                for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                {
                    if (quest->RequiredItemCount[i] > 0 && statusData.ItemCount[i] < quest->RequiredItemCount[i])
                    {
                        incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
                    }
                }
            }

            for (QuestPOI const &poi : *poiVector)
            {
                if (poi.MapId != bot->GetMapId())
                {
                    continue;
                }
                if (poi.points.empty())
                {
                    continue;
                }

                bool includePoi = false;
                bool isTurnIn = false;
                if (statusData.Status == QUEST_STATUS_COMPLETE)
                {
                    if (poi.ObjectiveIndex == -1)
                    {
                        includePoi = true;
                        isTurnIn = true;
                    }
                }
                else if (statusData.Status == QUEST_STATUS_INCOMPLETE)
                {
                    for (int32 objectiveIndex : incompleteObjectiveIdx)
                    {
                        if (poi.ObjectiveIndex == objectiveIndex)
                        {
                            includePoi = true;
                            break;
                        }
                    }
                }

                if (!includePoi)
                {
                    continue;
                }

                float sumX = 0.0f;
                float sumY = 0.0f;
                for (QuestPOIPoint const &point : poi.points)
                {
                    sumX += static_cast<float>(point.x);
                    sumY += static_cast<float>(point.y);
                }
                float avgX = sumX / static_cast<float>(poi.points.size());
                float avgY = sumY / static_cast<float>(poi.points.size());

                BotSnapshot::QuestPoi entryPoi;
                entryPoi.questId = questId;
                entryPoi.objectiveIndex = poi.ObjectiveIndex;
                entryPoi.mapId = poi.MapId;
                entryPoi.areaId = poi.AreaId;
                entryPoi.pos = Position3{avgX, avgY, bot->GetPositionZ()};
                entryPoi.isTurnIn = isTurnIn;
                entryPoi.hasZ = false;

                if (map)
                {
                    float height = map->GetHeight(avgX, avgY, MAX_HEIGHT);
                    float water = map->GetWaterLevel(avgX, avgY);
                    float z = std::max(height, water);
                    if (z != INVALID_HEIGHT)
                    {
                        entryPoi.pos.z = z;
                        entryPoi.hasZ = true;
                    }
                }

                results.push_back(std::move(entryPoi));
            }
        }

        return results;
    }

    float ExpectedAvgItemLevelForLevel(uint8 level)
    {
        // Heuristic baseline for "appropriate gear" by level. This is not a true GearScore implementation,
        // but it provides a consistent low/medium/high signal across leveling bands.
        if (level == 0)
        {
            return 0.0f;
        }
        if (level <= 60)
        {
            return static_cast<float>(level) + 5.0f;
        }
        if (level <= 70)
        {
            // 60->70: roughly 65 -> 115
            return 65.0f + (static_cast<float>(level) - 60.0f) * 5.0f;
        }
        // 70->80: roughly 115 -> 187
        return 115.0f + (static_cast<float>(level) - 70.0f) * 7.2f;
    }

    const char *SlotName(uint8 slot)
    {
        switch (slot)
        {
        case EQUIPMENT_SLOT_HEAD:
            return "head";
        case EQUIPMENT_SLOT_NECK:
            return "neck";
        case EQUIPMENT_SLOT_SHOULDERS:
            return "shoulder";
        case EQUIPMENT_SLOT_CHEST:
            return "chest";
        case EQUIPMENT_SLOT_WAIST:
            return "waist";
        case EQUIPMENT_SLOT_LEGS:
            return "legs";
        case EQUIPMENT_SLOT_FEET:
            return "feet";
        case EQUIPMENT_SLOT_WRISTS:
            return "wrist";
        case EQUIPMENT_SLOT_HANDS:
            return "hands";
        case EQUIPMENT_SLOT_FINGER1:
            return "ring1";
        case EQUIPMENT_SLOT_FINGER2:
            return "ring2";
        case EQUIPMENT_SLOT_TRINKET1:
            return "trinket1";
        case EQUIPMENT_SLOT_TRINKET2:
            return "trinket2";
        case EQUIPMENT_SLOT_BACK:
            return "cloak";
        case EQUIPMENT_SLOT_MAINHAND:
            return "mainhand";
        default:
            return "unknown";
        }
    }

    BotSnapshot BuildBotSnapshot(Player *bot, PlayerbotAI *ai)
    {
        // Gather bot state needed for planning and control.
        BotSnapshot snapshot;
        snapshot.pos = Position3{bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};
        snapshot.orientation = bot->GetOrientation();
        snapshot.mapId = bot->GetMapId();
        snapshot.zoneId = bot->GetZoneId();
        snapshot.areaId = bot->GetAreaId();
        snapshot.inCombat = bot->IsInCombat();
        snapshot.isMoving = bot->isMoving();
        snapshot.level = bot->GetLevel();
        snapshot.navCandidates = BuildNavCandidates(bot);
        snapshot.questGiversInRange = BuildQuestGiversInRange(bot, ai);
        snapshot.nearbyEntities = BuildNearbyEntities(bot, ai);
        AppendQuestGiverNavCandidates(bot, snapshot.nearbyEntities, snapshot.navCandidates);
        snapshot.questPois = BuildQuestPois(bot);

        // Gear / equipment signal (planner + control context).
        snapshot.avgItemLevel = bot->GetAverageItemLevel();
        snapshot.expectedAvgItemLevel = ExpectedAvgItemLevelForLevel(static_cast<uint8>(bot->GetLevel()));
        float expected = snapshot.expectedAvgItemLevel;
        if (expected > 0.0f)
        {
            float lowCut = expected * 0.85f;
            float highCut = expected * 1.15f;
            if (snapshot.avgItemLevel < lowCut)
                snapshot.gearBand = "low";
            else if (snapshot.avgItemLevel > highCut)
                snapshot.gearBand = "high";
            else
                snapshot.gearBand = "medium";
        }
        else
        {
            snapshot.gearBand = "unknown";
        }

        // Identify weak slots relative to expected average. Keep the list small.
        constexpr size_t kMaxLowSlots = 4;
        uint8 level = bot->GetLevel();
        float slotLowCut = expected > 0.0f ? std::max(0.0f, expected - 12.0f) : 0.0f;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_TABARD || slot == EQUIPMENT_SLOT_BODY)
                continue;
            if (slot == EQUIPMENT_SLOT_RANGED || slot == EQUIPMENT_SLOT_OFFHAND)
                continue;

            Item *item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            uint32 ilvl = 0;
            std::string name = "none";
            if (item && item->GetTemplate())
            {
                ilvl = item->GetTemplate()->GetItemLevelIncludingQuality(level);
                name = item->GetTemplate()->Name1;
            }
            bool isLow = (name == "none") || (expected > 0.0f && static_cast<float>(ilvl) < slotLowCut);
            if (isLow && snapshot.lowGearSlots.size() < kMaxLowSlots)
            {
                snapshot.lowGearSlots.push_back(BotSnapshot::GearSlot{SlotName(slot), name, ilvl});
            }
        }

        if (bot->GetMaxHealth() > 0)
        {
            snapshot.hpPct = (static_cast<float>(bot->GetHealth()) / bot->GetMaxHealth()) * 100.0f;
        }

        uint32 maxMana = bot->GetMaxPower(POWER_MANA);
        if (maxMana > 0)
        {
            snapshot.manaPct = (static_cast<float>(bot->GetPower(POWER_MANA)) / maxMana) * 100.0f;
        }

        auto weaponSubClassLabel = [](uint8 subClass) -> const char *
        {
            switch (subClass)
            {
            case ITEM_SUBCLASS_WEAPON_AXE:
                return "axe";
            case ITEM_SUBCLASS_WEAPON_AXE2:
                return "axe_2h";
            case ITEM_SUBCLASS_WEAPON_BOW:
                return "bow";
            case ITEM_SUBCLASS_WEAPON_GUN:
                return "gun";
            case ITEM_SUBCLASS_WEAPON_MACE:
                return "mace";
            case ITEM_SUBCLASS_WEAPON_MACE2:
                return "mace_2h";
            case ITEM_SUBCLASS_WEAPON_POLEARM:
                return "polearm";
            case ITEM_SUBCLASS_WEAPON_SWORD:
                return "sword";
            case ITEM_SUBCLASS_WEAPON_SWORD2:
                return "sword_2h";
            case ITEM_SUBCLASS_WEAPON_STAFF:
                return "staff";
            case ITEM_SUBCLASS_WEAPON_FIST:
                return "fist";
            case ITEM_SUBCLASS_WEAPON_DAGGER:
                return "dagger";
            case ITEM_SUBCLASS_WEAPON_THROWN:
                return "thrown";
            case ITEM_SUBCLASS_WEAPON_SPEAR:
                return "spear";
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                return "crossbow";
            case ITEM_SUBCLASS_WEAPON_WAND:
                return "wand";
            case ITEM_SUBCLASS_WEAPON_FISHING_POLE:
                return "fishing_pole";
            case ITEM_SUBCLASS_WEAPON_EXOTIC:
            case ITEM_SUBCLASS_WEAPON_EXOTIC2:
                return "exotic";
            case ITEM_SUBCLASS_WEAPON_MISC:
                return "misc";
            default:
                return nullptr;
            }
        };

        auto addWeaponTypeIfWeapon = [&](uint8 slot)
        {
            Item *item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
            {
                return;
            }

            ItemTemplate const *proto = item->GetTemplate();
            if (!proto || !proto->IsWeapon())
            {
                return;
            }

            const char *label = weaponSubClassLabel(proto->SubClass);
            if (!label || label[0] == '\0')
            {
                return;
            }

            snapshot.weaponTypes.emplace_back(label);
        };

        addWeaponTypeIfWeapon(EQUIPMENT_SLOT_MAINHAND);
        addWeaponTypeIfWeapon(EQUIPMENT_SLOT_OFFHAND);
        addWeaponTypeIfWeapon(EQUIPMENT_SLOT_RANGED);

        if (!snapshot.weaponTypes.empty())
        {
            std::sort(snapshot.weaponTypes.begin(), snapshot.weaponTypes.end());
            snapshot.weaponTypes.erase(std::unique(snapshot.weaponTypes.begin(), snapshot.weaponTypes.end()),
                                       snapshot.weaponTypes.end());
            snapshot.hasWeapon = true;
        }
        else
        {
            snapshot.hasWeapon = false;
        }

        auto addSkill = [&](uint32 skillId, const char *label)
        {
            if (!label || label[0] == '\0')
            {
                return;
            }

            uint32 value = bot->GetSkillValue(skillId);
            if (value == 0)
            {
                return;
            }

            uint32 maxValue = bot->GetMaxSkillValue(skillId);
            if (maxValue == 0)
            {
                maxValue = value;
            }

            std::ostringstream skill;
            skill << label << " " << value << "/" << maxValue;
            snapshot.professions.emplace_back(skill.str());
        };

        addSkill(SKILL_ALCHEMY, "alchemy");
        addSkill(SKILL_BLACKSMITHING, "blacksmithing");
        addSkill(SKILL_ENCHANTING, "enchanting");
        addSkill(SKILL_ENGINEERING, "engineering");
        addSkill(SKILL_HERBALISM, "herbalism");
        addSkill(SKILL_INSCRIPTION, "inscription");
        addSkill(SKILL_JEWELCRAFTING, "jewelcrafting");
        addSkill(SKILL_LEATHERWORKING, "leatherworking");
        addSkill(SKILL_MINING, "mining");
        addSkill(SKILL_SKINNING, "skinning");
        addSkill(SKILL_TAILORING, "tailoring");
        addSkill(SKILL_COOKING, "cooking");
        addSkill(SKILL_FIRST_AID, "first aid");
        addSkill(SKILL_FISHING, "fishing");

        if (!snapshot.professions.empty())
        {
            std::sort(snapshot.professions.begin(), snapshot.professions.end());
        }

        std::string currentActivity;
        std::string activityReason;
        if (TryGetActivityState(bot, currentActivity, activityReason))
        {
            snapshot.grindMode = (NormalizeCommandToken(currentActivity) == "grind");
        }

        const bool canMove = !snapshot.inCombat && !snapshot.grindMode && !snapshot.isMoving;
        for (auto &candidate : snapshot.navCandidates)
        {
            // canMove is the high-level gate (combat/grind/moving) AND physical reachability.
            candidate.canMove = canMove && candidate.reachable;
        }

        for (auto const &entry : bot->getQuestStatusMap())
        {
            QuestStatus status = entry.second.Status;
            if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
            {
                snapshot.activeQuestIds.push_back(entry.first);
                BotSnapshot::QuestProgress progress;
                progress.questId = entry.first;
                progress.status = status;
                progress.explored = entry.second.Explored;

                if (Quest const *quest = sObjectMgr->GetQuestTemplate(entry.first))
                {
                    progress.title = quest->GetTitle();
                    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    {
                        if (quest->RequiredItemCount[i] > 0)
                        {
                            std::string itemName;
                            if (ItemTemplate const *item = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]))
                            {
                                itemName = item->Name1;
                            }
                            progress.objectives.push_back(BotSnapshot::QuestObjectiveProgress{
                                "item",
                                static_cast<int32>(quest->RequiredItemId[i]),
                                itemName,
                                entry.second.ItemCount[i],
                                quest->RequiredItemCount[i]});
                        }
                    }

                    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    {
                        if (quest->RequiredNpcOrGoCount[i] > 0)
                        {
                            std::string targetName;
                            int32 target = quest->RequiredNpcOrGo[i];
                            if (target > 0)
                            {
                                if (CreatureTemplate const *creature = sObjectMgr->GetCreatureTemplate(static_cast<uint32>(target)))
                                {
                                    targetName = creature->Name;
                                }
                            }
                            else if (target < 0)
                            {
                                uint32 entryId = static_cast<uint32>(-target);
                                if (GameObjectTemplate const *go = sObjectMgr->GetGameObjectTemplate(entryId))
                                {
                                    targetName = go->name;
                                }
                            }
                            progress.objectives.push_back(BotSnapshot::QuestObjectiveProgress{
                                "npc_or_go",
                                quest->RequiredNpcOrGo[i],
                                targetName,
                                entry.second.CreatureOrGOCount[i],
                                quest->RequiredNpcOrGoCount[i]});
                        }
                    }

                    if (quest->GetPlayersSlain() > 0)
                    {
                        progress.objectives.push_back(BotSnapshot::QuestObjectiveProgress{
                            "player",
                            0,
                            std::string("players"),
                            entry.second.PlayerCount,
                            quest->GetPlayersSlain()});
                    }
                }

                snapshot.activeQuests.push_back(std::move(progress));
            }
        }

        return snapshot;
    }

    WorldSnapshot BuildWorldSnapshot(Player *bot)
    {
        // Resolve zone and area names for readability.
        WorldSnapshot snapshot;
        uint32 zoneId = bot->GetZoneId();
        uint32 areaId = bot->GetAreaId();

        if (AreaTableEntry const *zoneEntry = sAreaTableStore.LookupEntry(zoneId))
        {
            if (zoneEntry->area_name[LOCALE_enUS] && zoneEntry->area_name[LOCALE_enUS][0] != '\0')
            {
                snapshot.zone = zoneEntry->area_name[LOCALE_enUS];
            }
        }

        if (AreaTableEntry const *areaEntry = sAreaTableStore.LookupEntry(areaId))
        {
            if (areaEntry->area_name[LOCALE_enUS] && areaEntry->area_name[LOCALE_enUS][0] != '\0')
            {
                snapshot.area = areaEntry->area_name[LOCALE_enUS];
            }
        }

        if (snapshot.zone.empty())
        {
            snapshot.zone = "unknown";
        }
        if (snapshot.area.empty())
        {
            snapshot.area = "unknown";
        }

        return snapshot;
    }

    struct LocalAreaProfile
    {
        // Heuristic profile for area risk and pacing.
        std::string areaRole;
        uint32 levelBandMin = 1;
        uint32 levelBandMax = 1;
        std::string mobDensity;
        std::string respawnRate;
        std::string competitionLevel;
        std::string deathRisk;
        std::string corpseRunSeverity;
        std::string pullComplexity;
        std::string navigationComplexity;
        std::string obstacleFrequency;
        std::string roamingRequired;
    };

    LocalAreaProfile DeriveLocalAreaProfile(uint32 level)
    {
        // Rough defaults that scale with player level.
        LocalAreaProfile profile;
        if (level <= 5)
        {
            profile.areaRole = "starter_zone";
            profile.levelBandMin = 1;
            profile.levelBandMax = 5;
            profile.mobDensity = "high";
            profile.respawnRate = "fast";
            profile.competitionLevel = "low";
            profile.deathRisk = "low";
            profile.corpseRunSeverity = "minimal";
            profile.pullComplexity = "simple";
            profile.navigationComplexity = "simple";
            profile.obstacleFrequency = "low";
            profile.roamingRequired = "minimal";
            return profile;
        }

        if (level <= 10)
        {
            profile.areaRole = "low_level_zone";
            profile.levelBandMin = 6;
            profile.levelBandMax = 10;
            profile.mobDensity = "medium";
            profile.respawnRate = "normal";
            profile.competitionLevel = "low";
            profile.deathRisk = "low";
            profile.corpseRunSeverity = "low";
            profile.pullComplexity = "simple";
            profile.navigationComplexity = "simple";
            profile.obstacleFrequency = "low";
            profile.roamingRequired = "low";
            return profile;
        }

        if (level <= 20)
        {
            profile.areaRole = "adventuring_zone";
            profile.levelBandMin = 11;
            profile.levelBandMax = 20;
            profile.mobDensity = "medium";
            profile.respawnRate = "normal";
            profile.competitionLevel = "medium";
            profile.deathRisk = "medium";
            profile.corpseRunSeverity = "low";
            profile.pullComplexity = "moderate";
            profile.navigationComplexity = "moderate";
            profile.obstacleFrequency = "medium";
            profile.roamingRequired = "moderate";
            return profile;
        }

        profile.areaRole = "adventuring_zone";
        profile.levelBandMin = std::max<uint32>(1, level > 5 ? level - 5 : 1);
        profile.levelBandMax = level + 5;
        profile.mobDensity = "medium";
        profile.respawnRate = "normal";
        profile.competitionLevel = "medium";
        profile.deathRisk = "medium";
        profile.corpseRunSeverity = "moderate";
        profile.pullComplexity = "moderate";
        profile.navigationComplexity = "moderate";
        profile.obstacleFrequency = "medium";
        profile.roamingRequired = "moderate";
        return profile;
    }

    nlohmann::json BuildWorldModelJson()
    {
        // Static world model assumptions for LLM context.
        return {
            {"continent", "eastern_kingdoms"},
            {"faction_control", "alliance"},
            {"expansion_tier", "vanilla"},
            {"danger_profile", {{"baseline_threat", "low"}, {"elite_density", "none"}, {"pvp_risk", "none"}}},
            {"mob_ecology", {{"dominant_creature_types", {"humanoid", "beast"}}, {"average_mob_level_delta", 0}, {"mob_social_behavior", "loose_groups"}}},
            {"travel_characteristics", {{"terrain_openness", "open"}, {"line_of_sight", "long"}, {"verticality", "low"}}}};
    }

    nlohmann::json BuildSnapshotJson(BotSnapshot const &bot, WorldSnapshot const &world, Goal const *goal, LlmView view)
    {
        // Serialize the bot/world state for LLM prompts.
        LocalAreaProfile localProfile = DeriveLocalAreaProfile(bot.level);
        std::string normalizedZone = NormalizeAreaToken(world.zone);
        std::string normalizedArea = NormalizeAreaToken(world.area);
        std::string densityBand = localProfile.mobDensity;
        constexpr float kPi = 3.14159265f;
        float facingDeg = bot.orientation * 180.0f / kPi;
        if (facingDeg < 0.0f)
        {
            facingDeg += 360.0f;
        }
        std::string facingDirection = DirectionLabelFromBearing(facingDeg);
        nlohmann::json json;
        nlohmann::json questList = nlohmann::json::array();
        for (auto const &quest : bot.activeQuests)
        {
            bool eligibleForWorldActivity = quest.status == QUEST_STATUS_INCOMPLETE;
            bool needsTurnIn = quest.status == QUEST_STATUS_COMPLETE;
            bool isBlocked = quest.status == QUEST_STATUS_FAILED || quest.status == QUEST_STATUS_REWARDED || quest.status == QUEST_STATUS_NONE;
            uint32 totalRequired = 0;
            bool requiresKills = false;
            bool requiresItems = false;
            std::vector<std::string> objectiveTypes;
            nlohmann::json objectives = nlohmann::json::array();
            nlohmann::json questJson = {
                {"id", quest.questId},
                {"title", quest.title},
                {"status", QuestStatusToString(quest.status)},
                {"explored", quest.explored},
                {"eligible_for_world_activity", eligibleForWorldActivity},
                {"needs_turn_in", needsTurnIn}};
            nlohmann::json poiList = nlohmann::json::array();
            for (auto const &poi : bot.questPois)
            {
                if (poi.questId != quest.questId)
                {
                    continue;
                }

                nlohmann::json poiJson = {
                    {"objective_index", poi.objectiveIndex},
                    {"objective_type", poi.isTurnIn ? "turn_in"
                                                    : (poi.objectiveIndex >= QUEST_OBJECTIVES_COUNT ? "item" : "npc_or_go")},
                    {"map_id", poi.mapId},
                    {"area_id", poi.areaId},
                    {"is_turn_in", poi.isTurnIn}};

                if (poi.mapId == bot.mapId)
                {
                    float distance = Distance2d(bot.pos, poi.pos);
                    float bearing = BearingDegrees(bot.pos, poi.pos);
                    poiJson["distance_band"] = DistanceBandLabelForDistance(distance);
                    poiJson["direction"] = DirectionLabelFromBearing(bearing);
                }

                poiList.push_back(std::move(poiJson));
            }
            if (!poiList.empty())
            {
                questJson["poi"] = poiList;
            }
            if (!quest.objectives.empty())
            {
                for (auto const &objective : quest.objectives)
                {
                    totalRequired += objective.required;
                    if (objective.type == "npc_or_go" || objective.type == "player")
                    {
                        requiresKills = true;
                    }
                    if (objective.type == "item")
                    {
                        requiresItems = true;
                    }
                    if (std::find(objectiveTypes.begin(), objectiveTypes.end(), objective.type) == objectiveTypes.end())
                    {
                        objectiveTypes.push_back(objective.type);
                    }
                    objectives.push_back({{"type", objective.type},
                                          {"target_name", objective.targetName},
                                          {"current", objective.current},
                                          {"required", objective.required}});
                }
                questJson["objectives"] = objectives;
            }
            bool multiObjective = quest.objectives.size() > 1;
            bool parallelizable = multiObjective;
            bool satisfiableInCurrentArea = eligibleForWorldActivity && !isBlocked;
            std::string expectedTime = "short";
            if (totalRequired > 12)
            {
                expectedTime = "long";
            }
            else if (totalRequired > 5)
            {
                expectedTime = "medium";
            }
            std::string expectedCombatStyle = requiresKills ? "short_repeated" : "minimal";
            std::string movementStyle = (requiresKills || requiresItems) ? "local_wandering" : "localized";
            std::string overpullRisk = (densityBand == "high" && requiresKills) ? "medium" : "low";
            std::string expectedFriction = localProfile.deathRisk == "low" ? "low" : "medium";
            questJson["affordances"] = {
                {"lifecycle", {{"eligible_for_world_activity", eligibleForWorldActivity}, {"needs_turn_in", needsTurnIn}, {"is_blocked", isBlocked}}},
                {"objective_analysis", {{"objective_types", objectiveTypes}, {"requires_kills", requiresKills}, {"requires_items", requiresItems}, {"multi_objective", multiObjective}, {"parallelizable", parallelizable}}},
                {"world_footprint", {{"known_activity_regions", {{{"zone", normalizedZone}, {"area", normalizedArea}, {"confidence", 0.7}, {"proximity_band", "near"}, {"mob_density_band", densityBand}, {"mob_type_mix", {"humanoid", "beast"}}, {"expected_combat_style", expectedCombatStyle}, {"expected_movement_style", movementStyle}}}}, {"aggregate_proximity", "near"}, {"aggregate_density", densityBand}, {"satisfiable_in_current_area", satisfiableInCurrentArea}}},
                {"activity_expectations", {{"is_grind_friendly", requiresKills}, {"is_travel_heavy", false}, {"is_wait_gated", false}, {"expected_time_to_complete", expectedTime}, {"expected_friction", expectedFriction}}},
                {"risk_profile", {{"threat_level", localProfile.deathRisk}, {"overpull_risk", overpullRisk}, {"death_penalty_severity", localProfile.corpseRunSeverity}}}};
            questList.push_back(questJson);
        }

        if (view == LlmView::Planner)
        {
            nlohmann::json planner;
            planner["bot"] = {
                {"level", bot.level},
                {"in_combat", bot.inCombat},
                {"is_moving", bot.isMoving},
                {"grind_mode", bot.grindMode},
                {"active_quest_ids", bot.activeQuestIds},
                {"gear", {{"avg_item_level", std::round(bot.avgItemLevel * 10.0f) / 10.0f},
                          {"expected_avg_item_level", std::round(bot.expectedAvgItemLevel * 10.0f) / 10.0f},
                          {"band", bot.gearBand}}}};
            planner["world_model"] = BuildWorldModelJson();
            if (goal)
                planner["current_goal"] = goal->ToJson();
            return planner;
        }

        json["bot"] = {
            {"orientation_rad", std::round(bot.orientation * 1000.0f) / 1000.0f},
            {"facing_deg", std::round(facingDeg * 10.0f) / 10.0f},
            {"facing_direction", facingDirection},
            {"map_id", bot.mapId},
            {"zone_id", bot.zoneId},
            {"area_id", bot.areaId},
            {"in_combat", bot.inCombat},
            {"is_moving", bot.isMoving},
            {"grind_mode", bot.grindMode},
            {"idle_cycles", bot.idleCycles},
            {"hp_pct", std::round(bot.hpPct * 10.0f) / 10.0f},
            {"mana_pct", std::round(bot.manaPct * 10.0f) / 10.0f},
            {"level", bot.level},
            {"gear", {{"avg_item_level", std::round(bot.avgItemLevel * 10.0f) / 10.0f},
                      {"expected_avg_item_level", std::round(bot.expectedAvgItemLevel * 10.0f) / 10.0f},
                      {"band", bot.gearBand}}},
            {"travel", {{"active", bot.travelActive}, {"label", bot.travelLabel}, {"radius", std::round(bot.travelRadius * 10.0f) / 10.0f}, {"last_result", (bot.travelLastResult == TravelResult::Reached) ? "reached" : (bot.travelLastResult == TravelResult::TimedOut) ? "timed_out"
                                                                                                                                                                                                                      : (bot.travelLastResult == TravelResult::Aborted)    ? "aborted"
                                                                                                                                                                                                                                                                           : "none"},
                        {"last_change_ms", bot.travelLastChangeMs}}},
            {"profession", {{"active", bot.professionActive}, {"activity", (bot.professionActivity == ProfessionActivity::Fishing) ? "fishing" : "none"}, {"last_result", (bot.professionLastResult == ProfessionResult::Succeeded) ? "succeeded" : (bot.professionLastResult == ProfessionResult::TimedOut)      ? "timed_out"
                                                                                                                                                                                                                                                : (bot.professionLastResult == ProfessionResult::Aborted)         ? "aborted"
                                                                                                                                                                                                                                                : (bot.professionLastResult == ProfessionResult::FailedPermanent) ? "failed_permanent"
                                                                                                                                                                                                                                                : (bot.professionLastResult == ProfessionResult::FailedTemporary) ? "failed_temporary"
                                                                                                                                                                                                                                                : (bot.professionLastResult == ProfessionResult::Started)         ? "started"
                                                                                                                                                                                                                                                                                                                  : "none"},
                            {"last_change_ms", bot.professionLastChangeMs}}},
            {"debug", {{"control_cooldown_remaining_ms", bot.controlCooldownRemainingMs}, {"ollama_backoff_ms", bot.controlOllamaBackoffMs}, {"memory_pending_writes", bot.memoryPendingWrites}, {"memory_next_flush_ms", bot.memoryNextFlushMs}}},
            {"active_quest_ids", bot.activeQuestIds},
            {"active_quests", questList}};
        json["world_model"] = BuildWorldModelJson();
        json["local_area_model"] = {
            {"zone", normalizedZone},
            {"area", normalizedArea},
            {"area_role", localProfile.areaRole},
            {"recommended_level_band", {localProfile.levelBandMin, localProfile.levelBandMax}},
            {"population_model", {{"mob_density", localProfile.mobDensity}, {"respawn_rate", localProfile.respawnRate}, {"competition_level", localProfile.competitionLevel}}},
            {"activity_affordances", {{"supports_grinding", true}, {"supports_questing", true}, {"supports_exploration", true}, {"supports_safe_idle", localProfile.deathRisk == "low"}}},
            {"risk_model", {{"death_risk", localProfile.deathRisk}, {"corpse_run_severity", localProfile.corpseRunSeverity}, {"pull_complexity", localProfile.pullComplexity}}},
            {"movement_characteristics", {{"navigation_complexity", localProfile.navigationComplexity}, {"obstacle_frequency", localProfile.obstacleFrequency}, {"roaming_required", localProfile.roamingRequired}}}};
        nlohmann::json navCandidates = nlohmann::json::array();
        for (size_t i = 0; i < bot.navCandidates.size(); ++i)
        {
            navCandidates.push_back({{"candidate_id", std::string("nav_") + std::to_string(i)},
                                     {"label", bot.navCandidates[i].label},
                                     {"direction", bot.navCandidates[i].direction},
                                     {"distance_band", DistanceBandLabelForDistance(bot.navCandidates[i].distance2d)},
                                     {"has_los", bot.navCandidates[i].hasLOS},
                                     {"reachable", bot.navCandidates[i].reachable},
                                     {"can_move", bot.navCandidates[i].canMove}});
        }
        nlohmann::json distanceBands = nlohmann::json::array();
        for (auto const &band : kMoveHopDistanceBands)
        {
            distanceBands.push_back({{"label", band.label}});
        }
        json["nav"] = {
            {"nav_epoch", bot.navEpoch},
            {"candidates", navCandidates},
            {"distance_bands", distanceBands}};
        nlohmann::json questGivers = nlohmann::json::array();
        for (auto const &giver : bot.questGiversInRange)
        {
            float bearing = BearingDegrees(bot.pos, giver.pos);
            questGivers.push_back({{"name", giver.name},
                                   {"type", giver.type},
                                   {"entry_id", giver.entryId},
                                   {"distance_band", DistanceBandLabelForDistance(giver.distance)},
                                   {"direction", DirectionLabelFromBearing(bearing)},
                                   {"quest_marker", giver.questMarker},
                                   {"available_quest_ids", giver.availableQuestIds},
                                   {"turn_in_quest_ids", giver.turnInQuestIds},
                                   {"available_new_quest_ids", giver.availableNewQuestIds},
                                   {"turn_in_active_quest_ids", giver.turnInActiveQuestIds}});
        }
        json["quest_givers_in_range"] = questGivers;
        if (!bot.lowGearSlots.empty())
        {
            nlohmann::json lowSlots = nlohmann::json::array();
            for (auto const &slot : bot.lowGearSlots)
            {
                lowSlots.push_back({{"slot", slot.slot}, {"item", slot.item}, {"item_level", slot.itemLevel}});
            }
            json["bot"]["gear"]["low_slots"] = lowSlots;
        }
        nlohmann::json nearbyEntities = nlohmann::json::array();
        std::vector<BotSnapshot::NearbyEntity> orderedEntities = bot.nearbyEntities;
        std::stable_partition(orderedEntities.begin(), orderedEntities.end(),
                              [](BotSnapshot::NearbyEntity const &entity)
                              {
                                  return entity.type != "game_object";
                              });
        for (auto const &entity : orderedEntities)
        {
            float bearing = BearingDegrees(bot.pos, entity.pos);
            float distance = Distance2d(bot.pos, entity.pos);
            nearbyEntities.push_back({{"name", entity.name},
                                      {"type", entity.type},
                                      {"entry_id", entity.entryId},
                                      {"distance_band", DistanceBandLabelForDistance(distance)},
                                      {"direction", DirectionLabelFromBearing(bearing)},
                                      {"is_quest_giver", entity.isQuestGiver},
                                      {"quest_marker", entity.questMarker},
                                      {"visible", true}});
        }
        json["nearby_entities"] = nearbyEntities;
        if (goal)
        {
            json["current_goal"] = goal->ToJson();
        }
        return json;
    }

    std::string BuildPlannerStateSummary(BotSnapshot const &bot, WorldSnapshot const &world)
    {
        // Natural-language summary of state for the planner (no JSON).
        auto questTitleForId = [](uint32 questId) -> std::string
        {
            if (Quest const *quest = sObjectMgr->GetQuestTemplate(questId))
            {
                if (!quest->GetTitle().empty())
                {
                    return quest->GetTitle();
                }
            }
            std::ostringstream fallback;
            fallback << "Quest " << questId;
            return fallback.str();
        };

        std::ostringstream oss;
        oss << "Location: ";
        if (!world.zone.empty())
        {
            oss << world.zone;
        }
        else
        {
            oss << "unknown zone";
        }
        if (!world.area.empty())
        {
            oss << " / " << world.area;
        }
        oss << ". ";
        oss << "Level " << bot.level
            << ", HP " << std::round(bot.hpPct * 10.0f) / 10.0f << "%, Mana "
            << std::round(bot.manaPct * 10.0f) / 10.0f << "%.";
        oss << " Combat: " << (bot.inCombat ? "yes" : "no")
            << ", moving: " << (bot.isMoving ? "yes" : "no")
            << ", grind mode: " << (bot.grindMode ? "yes" : "no")
            << ", idle cycles: " << bot.idleCycles << ".\n";
        oss << "Travel: " << (bot.travelActive ? "active" : "inactive");
        if (bot.travelActive && !bot.travelLabel.empty())
        {
            oss << " (" << bot.travelLabel << ")";
        }
        if (bot.travelRadius > 0.0f)
        {
            oss << ", radius " << std::round(bot.travelRadius * 10.0f) / 10.0f << "m";
        }
        oss << ".\n";
        oss << "Profession: " << (bot.professionActive ? "active" : "inactive");
        if (bot.professionActivity == ProfessionActivity::Fishing)
        {
            oss << " (fishing)";
        }
        oss << ".\n";

        oss << "Weapons: ";
        if (!bot.weaponTypes.empty())
        {
            for (size_t i = 0; i < bot.weaponTypes.size(); ++i)
            {
                if (i > 0)
                {
                    oss << ", ";
                }
                oss << bot.weaponTypes[i];
            }
        }
        else
        {
            oss << "none";
        }
        oss << ".\n";

        oss << "Skills: ";
        if (!bot.professions.empty())
        {
            for (size_t i = 0; i < bot.professions.size(); ++i)
            {
                if (i > 0)
                {
                    oss << ", ";
                }
                oss << bot.professions[i];
            }
        }
        else
        {
            oss << "none";
        }
        oss << ".\n";

        oss << "Gear: avg item level " << std::round(bot.avgItemLevel * 10.0f) / 10.0f;
        if (bot.expectedAvgItemLevel > 0.0f)
        {
            oss << " (expected ~" << std::round(bot.expectedAvgItemLevel * 10.0f) / 10.0f
                << ", " << bot.gearBand << ").\n";
        }
        else
        {
            oss << ".\n";
        }
        if (!bot.lowGearSlots.empty())
        {
            oss << "Low gear slots: ";
            for (size_t i = 0; i < bot.lowGearSlots.size(); ++i)
            {
                if (i > 0)
                {
                    oss << ", ";
                }
                oss << bot.lowGearSlots[i].slot << " (" << bot.lowGearSlots[i].item;
                if (bot.lowGearSlots[i].itemLevel > 0)
                {
                    oss << " ilvl " << bot.lowGearSlots[i].itemLevel;
                }
                oss << ")";
            }
            oss << ".\n";
        }

        if (bot.activeQuests.empty())
        {
            oss << "Active quests: none.\n";
        }
        else
        {
            oss << "Active quests:\n";
            for (auto const &quest : bot.activeQuests)
            {
                bool eligibleForWorldActivity = quest.status == QUEST_STATUS_INCOMPLETE;
                bool needsTurnIn = quest.status == QUEST_STATUS_COMPLETE;
                std::string title = !quest.title.empty() ? quest.title : questTitleForId(quest.questId);
                oss << "- " << title << " (" << QuestStatusToString(quest.status) << ")";
                oss << ", eligible_for_world_activity: " << (eligibleForWorldActivity ? "yes" : "no")
                    << ", needs_turn_in: " << (needsTurnIn ? "yes" : "no") << ".";
                if (!quest.objectives.empty())
                {
                    oss << " Objectives: ";
                    bool first = true;
                    for (auto const &objective : quest.objectives)
                    {
                        if (!first)
                        {
                            oss << "; ";
                        }
                        first = false;
                        oss << objective.type;
                        if (!objective.targetName.empty())
                        {
                            oss << " " << objective.targetName;
                        }
                        else if (objective.targetId != 0)
                        {
                            oss << " " << objective.targetId;
                        }
                        oss << " " << objective.current << "/" << objective.required;
                    }
                    oss << ".";
                }
                oss << "\n";
            }
        }

        if (bot.questGiversInRange.empty())
        {
            oss << "Quest givers in range: none.\n";
        }
        else
        {
            oss << "Quest givers in range:\n";
            // Prefer turn-ins for active quests first (these are usually the most relevant).
            std::vector<BotSnapshot::QuestGiverInRange> ordered = bot.questGiversInRange;
            std::stable_sort(ordered.begin(), ordered.end(),
                             [](BotSnapshot::QuestGiverInRange const &a, BotSnapshot::QuestGiverInRange const &b)
                             {
                                 bool aTurnIn = !a.turnInActiveQuestIds.empty();
                                 bool bTurnIn = !b.turnInActiveQuestIds.empty();
                                 if (aTurnIn != bTurnIn)
                                     return aTurnIn > bTurnIn;
                                 return a.distance < b.distance;
                             });
            for (auto const &giver : ordered)
            {
                oss << "- " << giver.name;
                if (!giver.questMarker.empty())
                {
                    oss << " " << giver.questMarker;
                }
                if (!giver.turnInActiveQuestIds.empty())
                {
                    oss << " [turn_in_active]";
                }
                else if (!giver.availableNewQuestIds.empty())
                {
                    oss << " [available_new]";
                }
                oss << " (" << giver.type << "), distance "
                    << std::round(giver.distance * 10.0f) / 10.0f << "m, available quests: ";
                if (giver.availableQuestIds.empty())
                {
                    oss << "none";
                }
                else
                {
                    for (size_t i = 0; i < giver.availableQuestIds.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << ", ";
                        }
                        oss << questTitleForId(giver.availableQuestIds[i]);
                    }
                }
                oss << ", turn-in quests: ";
                if (giver.turnInQuestIds.empty())
                {
                    oss << "none";
                }
                else
                {
                    for (size_t i = 0; i < giver.turnInQuestIds.size(); ++i)
                    {
                        if (i > 0)
                        {
                            oss << ", ";
                        }
                        oss << questTitleForId(giver.turnInQuestIds[i]);
                    }
                }
                oss << ".\n";
            }
        }

        // Nearby quest givers with visible markers, even if not currently interactable.
        // This helps planners recognize quest NPCs slightly outside interaction range.
        size_t nearbyQuestGiverCount = 0;
        if (!bot.nearbyEntities.empty())
        {
            for (auto const& entity : bot.nearbyEntities)
            {
                if (!entity.isQuestGiver || entity.questMarker.empty())
                {
                    continue;
                }
                // Skip those already captured as "in range".
                bool alreadyInRange = false;
                for (auto const& inRange : bot.questGiversInRange)
                {
                    if (inRange.entryId == entity.entryId && inRange.name == entity.name)
                    {
                        alreadyInRange = true;
                        break;
                    }
                }
                if (alreadyInRange)
                {
                    continue;
                }

                if (nearbyQuestGiverCount == 0)
                {
                    oss << "Quest givers nearby (not in interact range):\n";
                }
                if (nearbyQuestGiverCount >= 5)
                {
                    break;
                }
                oss << "- " << entity.name << " " << entity.questMarker
                    << " (npc), distance " << std::round(entity.distance * 10.0f) / 10.0f << "m.\n";
                nearbyQuestGiverCount++;
            }
        }
        if (nearbyQuestGiverCount == 0)
        {
            oss << "Quest givers nearby (not in interact range): none.\n";
        }

        if (!bot.questPois.empty())
        {
            oss << "Quest POIs:\n";
            size_t count = 0;
            for (auto const &poi : bot.questPois)
            {
                if (count >= 5)
                {
                    break;
                }
                float distance = Distance2d(bot.pos, poi.pos);
                float bearing = BearingDegrees(bot.pos, poi.pos);
                oss << "- " << questTitleForId(poi.questId)
                    << " objective " << poi.objectiveIndex
                    << " (" << (poi.isTurnIn ? "turn_in" : "objective") << ")"
                    << ", distance " << std::round(distance * 10.0f) / 10.0f << "m"
                    << ", direction " << DirectionLabelFromBearing(bearing) << ".\n";
                count += 1;
            }
            if (bot.questPois.size() > count)
            {
                oss << "- ...and " << (bot.questPois.size() - count) << " more POIs.\n";
            }
        }

        if (bot.navCandidates.empty())
        {
            oss << "Navigation options: none.\n";
        }
        else
        {
            oss << "Navigation options:\n";
            for (auto const &candidate : bot.navCandidates)
            {
                oss << "- " << candidate.label
                    << " (" << candidate.direction << "), distance " << std::round(candidate.distance2d * 10.0f) / 10.0f
                    << "m, los: " << (candidate.hasLOS ? "yes" : "no")
                    << ", reachable: " << (candidate.reachable ? "yes" : "no")
                    << ", can_move: " << (candidate.canMove ? "yes" : "no") << ".\n";
            }
        }

        if (!bot.nearbyEntities.empty())
        {
            oss << "Nearby entities:\n";
            size_t count = 0;
            std::vector<BotSnapshot::NearbyEntity> orderedEntities = bot.nearbyEntities;
            std::stable_partition(orderedEntities.begin(), orderedEntities.end(),
                                  [](BotSnapshot::NearbyEntity const &entity)
                                  {
                                      return entity.type != "game_object";
                                  });
            for (auto const &entity : orderedEntities)
            {
                if (count >= 5)
                {
                    break;
                }
                float distance = Distance2d(bot.pos, entity.pos);
                float bearing = BearingDegrees(bot.pos, entity.pos);
                oss << "- " << entity.name;
                if (!entity.questMarker.empty())
                {
                    oss << " " << entity.questMarker;
                }
                oss << " (" << entity.type << "), distance "
                    << std::round(distance * 10.0f) / 10.0f << "m";
                oss << ", direction " << DirectionLabelFromBearing(bearing);
                if (entity.isQuestGiver)
                {
                    oss << ", quest giver";
                }
                oss << ".\n";
                count += 1;
            }
            if (bot.nearbyEntities.size() > count)
            {
                oss << "- ...and " << (bot.nearbyEntities.size() - count) << " more nearby entities.\n";
            }
        }

        return oss.str();
    }

    std::string BuildPlannerContext(BotSnapshot const &bot, WorldSnapshot const &world)
    {
        // Shared context header for planner prompts.
        const OllamaSettings settings = GetOllamaSettings();
        const std::string &systemPrompt = GetPrompt(LLMRole::Planner, settings);

        std::ostringstream oss;
        if (!systemPrompt.empty())
        {
            oss << systemPrompt << "\n\n";
        }
        oss << "STATE_SUMMARY\n";
        oss << BuildPlannerStateSummary(bot, world) << "\n\n";
        return oss.str();
    }

    void AppendPlannerStateSummary(std::string const& botName, std::string const& summary)
    {
        if (!g_EnableOllamaBotPlannerStateSummaryLog || g_OllamaBotPlannerStateSummaryLogPath.empty())
        {
            return;
        }
        std::lock_guard<std::mutex> lock(plannerSummaryLogMutex);
        std::ofstream out(g_OllamaBotPlannerStateSummaryLogPath, std::ios::out | std::ios::app);
        if (!out.is_open())
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Failed to open planner state summary log file.");
            return;
        }
        out << "bot=" << botName << " ts_ms=" << GetNowMs() << "\n";
        out << summary << "\n";
        out << "----\n";
    }

    std::string BuildLongTermGoalPrompt(BotSnapshot const &bot, WorldSnapshot const &world)
    {
        // Prompt the planner to produce a long-term goal sentence.
        std::ostringstream oss;
        oss << BuildPlannerContext(bot, world);
        oss << R"(INSTRUCTIONS
Write a single-sentence long-term goal based on STATE_SUMMARY.
- Output exactly one sentence.
- Use plain natural language.
- Do not mention tools, schemas, or JSON.
- Do not use bullet points or numbering.
)";
        return oss.str();
    }

    std::string BuildLongTermGoalReviewPrompt(BotSnapshot const &bot, WorldSnapshot const &world, std::string const &proposedGoal)
    {
        // Ask the planner to confirm or revise a long-term goal.
        std::ostringstream oss;
        oss << BuildPlannerContext(bot, world);
        oss << "PROPOSED_LONG_TERM_GOAL\n";
        oss << proposedGoal << "\n\n";
        oss << R"(INSTRUCTIONS
If the proposed long-term goal is still relevant, repeat it verbatim.
If it is no longer relevant, replace it with a new single-sentence long-term goal.
- Output exactly one sentence.
- Use plain natural language.
- Do not mention tools, schemas, or JSON.
- Do not use bullet points or numbering.
)";
        return oss.str();
    }

    std::string BuildShortTermGoalsPrompt(BotSnapshot const &bot, WorldSnapshot const &world, std::string const &longTermGoal)
    {
        // Prompt the planner to break a long-term goal into short-term goals.
        std::ostringstream oss;
        oss << BuildPlannerContext(bot, world);
        oss << "LONG_TERM_GOAL\n";
        oss << longTermGoal << "\n\n";
        oss << R"(INSTRUCTIONS
Break the long-term goal into short-term goals, using STATE_SUMMARY for context.
- Provide 3 to 5 short-term goals.
- Each short-term goal must be 2 to 3 sentences describing a concrete near-term objective.
- Separate each short-term goal with a blank line.
- Use plain natural language only.
- Do not mention tools, schemas, or JSON.
- Do not use numbered or bulleted lists.
)";
        return oss.str();
    }

    // --
    // Two-phase planner prompt builders
    //
    // Build a long-term goal prompt using distinct PlannerLongTerm role. This prompt
    // includes optional memory text and relies on the planner system prompt for
    // PlannerLongTerm. It instructs the LLM to output exactly one sentence.
    std::string BuildPlannerLongTermPrompt(BotSnapshot const &bot, WorldSnapshot const &world, std::string const &memory)
    {
        const OllamaSettings settings = GetOllamaSettings();
        const std::string &systemPrompt = GetPrompt(LLMRole::PlannerLongTerm, settings);
        std::ostringstream oss;
        if (!systemPrompt.empty())
        {
            oss << systemPrompt << "\n\n";
        }
        // Include optional prior memory for context if provided.
        if (!memory.empty())
        {
            oss << "MEMORY\n";
            oss << memory << "\n\n";
        }
        oss << "STATE_SUMMARY\n";
        oss << BuildPlannerStateSummary(bot, world) << "\n\n";
        oss << "INSTRUCTIONS\n";
        oss << "Write a single-sentence long-term goal based on STATE_SUMMARY and MEMORY.\n";
        oss << "- Prefer picking up nearby available quests, nearby quest objectives, and nearby quest turn-ins when possible.\n";
        oss << "- If you mention talking to a quest giver, prefer quest givers that turn in ACTIVE quests, and do not suggest unrelated NPCs.\n";
        oss << "- Output exactly one sentence.\n";
        oss << "- Use plain natural language.\n";
        oss << "- Do not mention tools, schemas, or JSON.\n";
        oss << "- Do not use bullet points or numbering.\n";
        return oss.str();
    }

    // Build a short-term goals prompt using distinct PlannerShortTerm role. This
    // prompt includes the current long-term goal and optional memory text, and
    // instructs the LLM to output several short-term goals separated by blank
    // lines. Each goal should be a small paragraph of 2–3 sentences.
    std::string BuildPlannerShortTermPrompt(BotSnapshot const &bot, WorldSnapshot const &world, std::string const &memory, std::string const &longTermGoal, std::string const &focusQuest)
    {
        const OllamaSettings settings = GetOllamaSettings();
        const std::string &systemPrompt = GetPrompt(LLMRole::PlannerShortTerm, settings);
        std::ostringstream oss;
        if (!systemPrompt.empty())
        {
            oss << systemPrompt << "\n\n";
        }
        if (!memory.empty())
        {
            oss << "MEMORY\n";
            oss << memory << "\n\n";
        }
        oss << "LONG_TERM_GOAL\n";
        oss << longTermGoal << "\n\n";
        if (!focusQuest.empty())
        {
            oss << "FOCUS_QUEST\n";
            oss << focusQuest << "\n\n";
        }
        oss << "STATE_SUMMARY\n";
        oss << BuildPlannerStateSummary(bot, world) << "\n\n";
        oss << "INSTRUCTIONS\n";
        oss << "Using LONG_TERM_GOAL (and STATE_SUMMARY/MEMORY for context), write exactly ONE short-term goal.\n";
        oss << "- Prefer picking up nearby available quests, nearby quest objectives, and nearby quest turn-ins when possible.\n";
        oss << "- The short-term goal must be a single plain-text sentence.\n";
        oss << "- Make it specific: name quest(s), NPC(s), mob(s), item(s), and/or objective target(s).\n";
        oss << "- If the next step requires killing mobs, explicitly say to grind the relevant mobs.\n";
        oss << "- Do NOT output JSON.\n";
        oss << "- Do NOT output bullet points or numbering.\n";
        oss << "- Do NOT include steps, explanations, or tool references.\n";
        oss << "- If FOCUS_QUEST is provided, every goal must advance FOCUS_QUEST and must not mention other quests.\n";
        oss << "- Return exactly one line, and nothing else.\n";
        return oss.str();
    }

    std::string BuildControlPrompt(BotSnapshot const &bot, WorldSnapshot const &world,
                                   std::string const &longTermGoal,
                                   std::vector<std::string> const &shortTermGoals,
                                   size_t shortTermIndex)
    {
        // Compose the control prompt with goal and tool rules.
        nlohmann::json stateJson = BuildSnapshotJson(bot, world, nullptr, LlmView::Control);
        const OllamaSettings settings = GetOllamaSettings();
        const std::string &systemPrompt = GetPrompt(LLMRole::Control, settings);
        bool compact = UseCompactPromptFormat();

        std::string currentShortTermGoal = CurrentShortTermGoal(shortTermGoals, shortTermIndex);

        std::ostringstream oss;
        if (!systemPrompt.empty())
        {
            oss << systemPrompt << "\n\n";
        }
        if (compact)
        {
            oss << "LT:\n";
            oss << (longTermGoal.empty() ? "none" : longTermGoal) << "\n\n";

            oss << "ST:\n";
            oss << (currentShortTermGoal.empty() ? "none" : currentShortTermGoal) << "\n\n";

            oss << "S:\n";
            oss << stateJson.dump() << "\n\n";
        oss << R"(INSTRUCTIONS
You are a control-only executor.
- Output exactly one <tool_call> block (or no output).
- No extra text or JSON outside the tool call.
- Use LT, ST, and S to choose a valid tool.
- If S.bot.in_combat is true or S.bot.is_moving is true, call request_idle.
- If S.quest_givers_in_range is not empty, prioritize request_talk_to_quest_giver.
- Otherwise, prefer nearer quest objectives or nearer quest POIs when choosing movement.
- If no control action is needed, call request_idle.
)";
            oss << "- If S.bot.idle_cycles >= " << kIdlePenaltyStartCycles
                << " and you are idle, avoid request_idle; prefer a safe move_hop.\n";
            oss << BuildControlToolInstructions("S");
            return oss.str();
        }

        oss << "LONG_TERM_GOAL:\n";
        oss << (longTermGoal.empty() ? "none" : longTermGoal) << "\n\n";

        oss << "SHORT_TERM_GOAL:\n";
        oss << (currentShortTermGoal.empty() ? "none" : currentShortTermGoal) << "\n\n";

        oss << "STATE_JSON\n";
        oss << stateJson.dump(2) << "\n\n";
        oss << R"(INSTRUCTIONS
You are a control-only executor.
- Output exactly one <tool_call> block (or no output).
- No extra text or JSON outside the tool call.
- Use LONG_TERM_GOAL, SHORT_TERM_GOAL, and STATE_JSON to choose a valid tool.
- If STATE_JSON.bot.in_combat is true, call request_idle.
- If STATE_JSON.bot.is_moving is true, call request_idle unless STATE_JSON.bot.grind_mode is true (in that case you may call request_stop_grind).
- If STATE_JSON.bot.grind_mode is true and you need to travel/quest/talk, call request_stop_grind.
- If STATE_JSON.quest_givers_in_range is not empty, prioritize request_talk_to_quest_giver.
- If you intend to talk to a quest giver and your facing does not match its direction, use a turn tool first, then talk.
- If working on incomplete quest objectives and relevant mobs are nearby, call request_enter_grind.
- Otherwise, prefer nearer quest objectives or nearer quest POIs when choosing movement.
- If no control action is needed, call request_idle.
)";
        oss << "- If STATE_JSON.bot.idle_cycles >= " << kIdlePenaltyStartCycles
            << " and you are idle, avoid request_idle; prefer a safe move_hop.\n";
        oss << BuildControlToolInstructions("STATE_JSON");
        return oss.str();
    }

    bool HasQuestGiverForQuestId(BotSnapshot const &snapshot, uint32 questId)
    {
        // Validate a quest talk command against nearby quest givers.
        for (auto const &giver : snapshot.questGiversInRange)
        {
            if (std::find(giver.availableQuestIds.begin(), giver.availableQuestIds.end(), questId) != giver.availableQuestIds.end())
            {
                return true;
            }
            if (std::find(giver.turnInQuestIds.begin(), giver.turnInQuestIds.end(), questId) != giver.turnInQuestIds.end())
            {
                return true;
            }
        }
        return false;
    }

    std::string NormalizeDirectionToken(std::string direction)
    {
        // Accept synonyms and normalize to a single direction token.
        direction = TrimCopy(direction);
        std::transform(direction.begin(), direction.end(), direction.begin(), ::tolower);
        if (direction == "forward" || direction == "forwards" || direction == "ahead" || direction == "up")
        {
            return "forward";
        }
        if (direction == "backward" || direction == "backwards" || direction == "back" || direction == "down")
        {
            return "backward";
        }
        if (direction == "left" || direction == "leftward")
        {
            return "left";
        }
        if (direction == "right" || direction == "rightward")
        {
            return "right";
        }
        return "";
    }

    bool TryParseNavCandidateIndex(std::string const &candidateId, size_t &outIndex)
    {
        // Current candidate IDs are of the form "nav_<index>".
        // This helper is intentionally strict to avoid accidental acceptance of
        // geometry-bearing IDs.
        constexpr const char *kPrefix = "nav_";
        if (candidateId.rfind(kPrefix, 0) != 0)
        {
            return false;
        }
        std::string suffix = candidateId.substr(std::strlen(kPrefix));
        if (suffix.empty())
        {
            return false;
        }
        for (unsigned char c : suffix)
        {
            if (!std::isdigit(c))
            {
                return false;
            }
        }
        try
        {
            outIndex = static_cast<size_t>(std::stoul(suffix));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseMoveHopNavArguments(nlohmann::json const& args, uint32& outNavEpoch, std::string& outCandidateId)
    {
        if (!args.is_object())
        {
            return false;
        }
        if (!args.contains("nav_epoch"))
        {
            return false;
        }
        if (!args.contains("candidate_id"))
        {
            return false;
        }

        if (args["nav_epoch"].is_number_unsigned())
        {
            outNavEpoch = args["nav_epoch"].get<uint32>();
        }
        else if (args["nav_epoch"].is_number_integer())
        {
            int64 epoch = args["nav_epoch"].get<int64>();
            if (epoch < 0 || epoch > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            outNavEpoch = static_cast<uint32>(epoch);
        }
        else
        {
            return false;
        }

        outCandidateId.clear();
        if (args["candidate_id"].is_string())
        {
            outCandidateId = TrimCopy(args["candidate_id"].get<std::string>());
        }
        else if (args["candidate_id"].is_number_unsigned())
        {
            outCandidateId = "nav_" + std::to_string(args["candidate_id"].get<uint64>());
        }
        else if (args["candidate_id"].is_number_integer())
        {
            int64 idx = args["candidate_id"].get<int64>();
            if (idx < 0)
            {
                return false;
            }
            outCandidateId = "nav_" + std::to_string(static_cast<uint64>(idx));
        }
        else
        {
            return false;
        }

        std::transform(outCandidateId.begin(), outCandidateId.end(), outCandidateId.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!outCandidateId.empty() &&
            std::all_of(outCandidateId.begin(), outCandidateId.end(),
                        [](unsigned char c) { return std::isdigit(c); }))
        {
            outCandidateId = "nav_" + outCandidateId;
        }

        return !outCandidateId.empty();
    }

    bool ParseQuestIdArguments(nlohmann::json const& args, uint32& questId)
    {
        if (!args.is_object())
        {
            return false;
        }
        if (!args.contains("quest_id"))
        {
            return false;
        }

        if (args["quest_id"].is_number_unsigned())
        {
            questId = args["quest_id"].get<uint32>();
        }
        else if (args["quest_id"].is_number_integer())
        {
            int64 parsed = args["quest_id"].get<int64>();
            if (parsed <= 0 || parsed > std::numeric_limits<uint32>::max())
            {
                return false;
            }
            questId = static_cast<uint32>(parsed);
        }
        else
        {
            return false;
        }

        return questId != 0;
    }

    void LogControlToolAccepted(std::string const &name, ControlAction::Capability capability, std::string const &reason)
    {
        // Structured logging for accepted tool calls.
        LOG_INFO("server.loading", "[ControlTool] name={}", name);
        LOG_INFO("server.loading", "[ControlTool] resolved={}", CapabilityName(capability));
        LOG_INFO("server.loading", "[ControlTool] gated=true reason={}", reason);
    }

    void LogControlToolRejected(std::string const &name, std::string const &reason)
    {
        // Structured logging for rejected tool calls.
        LOG_INFO("server.loading", "[ControlTool] rejected name={} reason={}", name, reason);
    }

    struct LlmBotState
    {
        // Per-bot LLM runtime state for throttling and context.
        enum class ControlState : uint8
        {
            Idle,
            Waiting,
            FailureHold,
            Cooldown
        };

        ThinkScheduler scheduler;
        std::string longTermGoal;
        std::vector<std::string> shortTermGoals;
        std::atomic<size_t> shortTermIndex{0};
        std::atomic<bool> strategicBusy{false};
        std::atomic<bool> controlBusy{false};
        std::atomic<bool> promptInFlight{false};
        // Control planner (Ollama) backpressure.
        // These are atomic because the control request runs in a detached thread.
        std::atomic<ControlState> controlState{ControlState::Idle};
        std::atomic<uint32> nextAllowedAttemptMs{0};
        std::atomic<uint32> nextPlannerShortTickMs{0};
        std::atomic<uint32> nextPlannerLongTickMs{0};
        std::atomic<uint32> nextStrategicAllowedMs{0};
        std::atomic<uint32> failureHoldUntilMs{0};
        std::atomic<uint32> ollamaCooldownMs{kOllamaBaseCooldownMs};
        uint32 lastGoalChangeMs = 0;
        bool hasStrategicResult = false;
        std::atomic<bool> loggedStrategicParseError{false};
        std::atomic<bool> loggedControlParseError{false};
        bool hasLastPosition = false;
        Position3 lastPosition;
        uint32 idleCycles = 0;

        // Monotonic nav epoch for navigation candidates.
        uint32 navEpoch = 0;

        // Movement is owned per-bot and must be ticked before any LLM/AI logic.
        BotMovement movement;

        // Travel semantics (completion/failure) for the last requested destination.
        BotTravel travel;

        // Persistent memory (two-tier cache + DB backing), read-only to LLM.
        BotMemory memory;

        // Professions (execution-only) such as fishing.
        BotProfession profession;

        // Guard to record travel outcomes into memory once.
        uint32 lastTravelRecordedMs = 0;
        uint32 lastTravelAdvanceMs = 0;
        std::atomic<uint8> lastControlCapability{static_cast<uint8>(ControlAction::Capability::Idle)};
        std::atomic<bool> forceControl{false};
        std::atomic<bool> forceStrategic{false};

        // Track quest completion transitions to force planner refresh (dedup per quest id).
        std::unordered_set<uint32> notifiedCompletedQuestIds;

        // Guard to record profession outcomes into memory once.
        uint32 lastProfessionRecordedMs = 0;
    };

    std::unordered_map<uint64, std::shared_ptr<LlmBotState>> botStates;
}

static std::mutex sPlannerRefreshMutex;
static std::unordered_map<uint64, uint32> sPendingLongTermPlannerRefreshMs;

static uint32 ConsumeLongTermPlannerRefresh(uint64 guid)
{
    std::lock_guard<std::mutex> lock(sPlannerRefreshMutex);
    auto it = sPendingLongTermPlannerRefreshMs.find(guid);
    if (it == sPendingLongTermPlannerRefreshMs.end())
    {
        return 0;
    }
    uint32 at = it->second;
    sPendingLongTermPlannerRefreshMs.erase(it);
    return at;
}

OllamaBotControlLoop::OllamaBotControlLoop() : WorldScript("OllamaBotControlLoop") {}

void RequestLongTermPlannerRefresh(uint64 guid, uint32 nowMs)
{
    if (guid == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sPlannerRefreshMutex);
    sPendingLongTermPlannerRefreshMs[guid] = nowMs;
}

static void EnqueueStrategicUpdate(uint64 guid, PendingStrategicUpdate update)
{
    // Stage the planner result until the main thread applies it.
    std::lock_guard<std::mutex> lock(pendingMutex);
    pendingStrategicUpdates[guid] = std::move(update);
}

std::string EscapeBracesForFmt(const std::string &input)
{
    // Double braces so fmt-style formatting doesn't consume JSON braces.
    std::string output;
    output.reserve(input.size() * 2);

    for (char c : input)
    {
        if (c == '{' || c == '}')
        {
            output.push_back(c);
            output.push_back(c);
        }
        else
        {
            output.push_back(c);
        }
    }
    return output;
}

void OllamaBotControlLoop::OnUpdate(uint32 diff)
{
    // Main update loop: manage LLM planning and control per bot.
    if (!g_OllamaBotRuntime.enable_control)
    {
        return;
    }

    for (auto const &itr : ObjectAccessor::GetPlayers())
    {
        Player *bot = itr.second;
        if (!bot || !bot->IsInWorld())
        {
            continue;
        }

        PlayerbotAI *ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai || !ai->IsBotAI())
        {
            continue;
        }

        if (!g_OllamaBotControlBotName.empty())
        {
            // Optional bot-name allowlist (comma separated).
            bool allowed = false;
            std::stringstream ss(g_OllamaBotControlBotName);
            std::string name;

            while (std::getline(ss, name, ','))
            {
                if (bot->GetName() == name)
                {
                    allowed = true;
                    break;
                }
            }

            if (!allowed)
                continue;
        }

        uint32 nowMs = getMSTime();
        uint64 guid = bot->GetGUID().GetRawValue();
        auto &statePtr = botStates[guid];
        if (!statePtr)
        {
            statePtr = std::make_shared<LlmBotState>();

            // Expose the per-bot movement instance to other scripts...
            BotMovementRegistry::Register(guid, &statePtr->movement);
            BotTravelRegistry::Register(guid, &statePtr->travel);
            BotMemoryRegistry::Register(guid, &statePtr->memory);
            BotProfessionRegistry::Register(guid, &statePtr->profession);
            statePtr->memory.Initialize(guid, nowMs);
            if (g_OllamaBotRuntime.control_startup_delay_ms > 0)
            {
                uint32 delayUntilMs = nowMs + static_cast<uint32>(g_OllamaBotRuntime.control_startup_delay_ms);
                statePtr->controlState.store(LlmBotState::ControlState::Cooldown, std::memory_order_relaxed);
                statePtr->nextAllowedAttemptMs.store(delayUntilMs, std::memory_order_relaxed);
                statePtr->nextPlannerShortTickMs.store(delayUntilMs, std::memory_order_relaxed);
                statePtr->nextPlannerLongTickMs.store(delayUntilMs, std::memory_order_relaxed);
                statePtr->nextStrategicAllowedMs.store(delayUntilMs, std::memory_order_relaxed);
            }
        }
        LlmBotState &state = *statePtr;

        // Tick movement first; travel completion is checked every tick.
        state.movement.Update(diff);
        state.travel.Update(bot, nowMs);

        if (g_OllamaBotControlClearGoalsOnConfigLoad)
        {
            state.longTermGoal.clear();
            state.shortTermGoals.clear();
            state.shortTermIndex.store(0, std::memory_order_relaxed);
            state.hasStrategicResult = false;
            state.lastGoalChangeMs = 0;
            state.loggedStrategicParseError.store(false, std::memory_order_relaxed);
            state.loggedControlParseError.store(false, std::memory_order_relaxed);
        }

        // Tick professions (non-combat execution). Uses Playerbots actions but no movement.
        state.profession.Update(bot, ai, nowMs);

        if (state.travel.LastResult() == TravelResult::Reached &&
            state.travel.LastChangeMs() > state.lastTravelAdvanceMs)
        {
            state.lastTravelAdvanceMs = state.travel.LastChangeMs();
            if (!state.shortTermGoals.empty())
            {
                size_t currentIndex = state.shortTermIndex.load(std::memory_order_relaxed);
                size_t nextIndex = (currentIndex + 1) % state.shortTermGoals.size();
                state.shortTermIndex.store(nextIndex, std::memory_order_relaxed);
            }
            if (state.lastControlCapability.load(std::memory_order_relaxed) ==
                static_cast<uint8>(ControlAction::Capability::MoveHop))
            {
                state.forceControl.store(true, std::memory_order_relaxed);
            }
        }

        // Update memory (write-behind flushes are rate-limited internally).
        state.memory.Update(nowMs);

        // Tie travel outcomes into memory to reduce thrash and improve stability.
        if (state.travel.LastResult() != TravelResult::None && state.travel.LastChangeMs() > state.lastTravelRecordedMs)
        {
            state.lastTravelRecordedMs = state.travel.LastChangeMs();
            std::string key = "travel:unknown";
            if (auto cur = state.travel.Current())
            {
                if (!cur->key.empty())
                    key = "travel:" + cur->key;
            }

            switch (state.travel.LastResult())
            {
            case TravelResult::Reached:
                state.memory.ClearFailures(key);
                break;
            case TravelResult::TimedOut:
                state.memory.RecordFailure(key, FailureType::Retryable, nowMs);
                break;
            case TravelResult::Aborted:
                state.memory.RecordFailure(key, FailureType::Temporary, nowMs);
                break;
            default:
                break;
            }
        }

        // Tie profession outcomes into memory. This prevents spammy retries and gives the controller
        // realistic cooldown behavior.
        if (state.profession.LastResult() != ProfessionResult::None &&
            state.profession.LastChangeMs() > state.lastProfessionRecordedMs &&
            !state.profession.Active())
        {
            state.lastProfessionRecordedMs = state.profession.LastChangeMs();
            std::string key = "profession:fishing";

            switch (state.profession.LastResult())
            {
            case ProfessionResult::Succeeded:
                state.memory.ClearFailures(key);
                break;
            case ProfessionResult::TimedOut:
                state.memory.RecordFailure(key, FailureType::Retryable, nowMs);
                break;
            case ProfessionResult::Aborted:
                state.memory.RecordFailure(key, FailureType::Temporary, nowMs);
                break;
            case ProfessionResult::FailedPermanent:
                state.memory.RecordFailure(key, FailureType::Permanent, nowMs);
                break;
            case ProfessionResult::FailedTemporary:
                state.memory.RecordFailure(key, FailureType::Temporary, nowMs);
                break;
            default:
                break;
            }
        }
        if (state.movement.IsMoving())
        {
            continue;
        }

        if (state.profession.Active())
        {
            // While a profession session is running, do not invoke the LLM/controller.
            continue;
        }

        if (state.promptInFlight.load(std::memory_order_relaxed))
        {
            continue;
        }
        uint32 globalPauseUntil = globalControlPauseUntilMs.load(std::memory_order_relaxed);
        if (globalPauseUntil > 0 && nowMs < globalPauseUntil)
        {
            continue;
        }
        if (globalPauseUntil > 0 && nowMs >= globalPauseUntil)
        {
            uint32 expected = globalPauseUntil;
            if (globalControlPauseUntilMs.compare_exchange_strong(expected, 0u))
            {
                globalControlResumeBaseMs.store(nowMs, std::memory_order_relaxed);
            }
        }
        uint32 resumeBaseMs = globalControlResumeBaseMs.load(std::memory_order_relaxed);
        LlmBotState::ControlState controlState = state.controlState.load(std::memory_order_relaxed);
        if (controlState == LlmBotState::ControlState::Waiting)
        {
            continue;
        }
        if (controlState == LlmBotState::ControlState::FailureHold)
        {
            uint32 holdUntil = state.failureHoldUntilMs.load(std::memory_order_relaxed);
            if (nowMs < holdUntil)
            {
                continue;
            }
            state.controlState.store(LlmBotState::ControlState::Cooldown, std::memory_order_relaxed);
            controlState = LlmBotState::ControlState::Cooldown;
        }
        if (controlState == LlmBotState::ControlState::Cooldown)
        {
            uint32 nextAttempt = state.nextAllowedAttemptMs.load(std::memory_order_relaxed);
            if (nowMs < nextAttempt)
            {
                continue;
            }
            state.controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
        }

        uint32 nextShortTick = state.nextPlannerShortTickMs.load(std::memory_order_relaxed);
        uint32 nextLongTick = state.nextPlannerLongTickMs.load(std::memory_order_relaxed);
        uint32 nextDueTick = std::min(nextShortTick, nextLongTick);

        if (resumeBaseMs > 0 && nextDueTick < resumeBaseMs)
        {
            uint32 jitterMs = static_cast<uint32>(guid % kGlobalResumeSpreadMs);
            uint32 shifted = resumeBaseMs + jitterMs;
            if (nextShortTick < shifted)
            {
                nextShortTick = shifted;
                state.nextPlannerShortTickMs.store(nextShortTick, std::memory_order_relaxed);
            }
            if (nextLongTick < shifted)
            {
                nextLongTick = shifted;
                state.nextPlannerLongTickMs.store(nextLongTick, std::memory_order_relaxed);
            }
            nextDueTick = std::min(nextShortTick, nextLongTick);
        }

        // If any quest just transitioned to COMPLETE, force a strategic refresh immediately
        // (short-term + long-term) regardless of normal planner tick delays.
        bool newlyCompletedQuest = false;
        std::unordered_set<uint32> completedNow;
        for (auto const& entry : bot->getQuestStatusMap())
        {
            if (entry.second.Status == QUEST_STATUS_COMPLETE)
            {
                completedNow.insert(entry.first);
                if (state.notifiedCompletedQuestIds.insert(entry.first).second)
                {
                    newlyCompletedQuest = true;
                }
            }
        }
        for (auto it = state.notifiedCompletedQuestIds.begin(); it != state.notifiedCompletedQuestIds.end();)
        {
            if (completedNow.find(*it) == completedNow.end())
            {
                it = state.notifiedCompletedQuestIds.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (newlyCompletedQuest)
        {
            state.forceStrategic.store(true, std::memory_order_relaxed);
            state.nextPlannerShortTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextPlannerLongTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextStrategicAllowedMs.store(0u, std::memory_order_relaxed);
            state.hasStrategicResult = false;
            nextShortTick = nowMs;
            nextLongTick = nowMs;
            nextDueTick = nowMs;
        }

        if (nowMs < nextDueTick)
        {
            continue;
        }
        BotSnapshot snapshot = BuildBotSnapshot(bot, ai);
        // Publish internal navigation candidates for controller resolution (not serialized to the LLM).
        {
            BotNavState navState;
            uint32 navEpoch = ++state.navEpoch;
            snapshot.navEpoch = navEpoch;
            navState.navEpoch = navEpoch;
            navState.candidates.reserve(snapshot.navCandidates.size());
            for (size_t i = 0; i < snapshot.navCandidates.size(); ++i)
            {
                auto const &c = snapshot.navCandidates[i];
                NavCandidateInternal internal;
                internal.candidateId = "nav_" + std::to_string(i);
                internal.mapId = snapshot.mapId;
                internal.x = c.pos.x;
                internal.y = c.pos.y;
                internal.z = c.pos.z;
                internal.reachable = c.reachable;
                internal.hasLOS = c.hasLOS;
                internal.canMove = c.canMove;
                navState.candidates.push_back(std::move(internal));
            }
            BotNavStateRegistry::SetState(guid, navState);
        }
        // Attach travel status for the controller LLM.
        snapshot.travelActive = state.travel.Active();
        snapshot.travelLastResult = state.travel.LastResult();
        snapshot.travelLastChangeMs = state.travel.LastChangeMs();
        if (auto cur = state.travel.Current())
        {
            snapshot.travelRadius = cur->radius;
            snapshot.travelLabel = "movement";
        }

        snapshot.professionActive = state.profession.Active();
        snapshot.professionActivity = state.profession.Activity();
        snapshot.professionLastResult = state.profession.LastResult();
        snapshot.professionLastChangeMs = state.profession.LastChangeMs();

        snapshot.memoryPendingWrites = state.memory.PendingWrites();
        snapshot.memoryNextFlushMs = state.memory.NextDbFlushInMs(nowMs);
        uint32 nextAllowed = state.nextAllowedAttemptMs.load(std::memory_order_relaxed);
        snapshot.controlCooldownRemainingMs = (nowMs < nextAllowed) ? (nextAllowed - nowMs) : 0;
        snapshot.controlOllamaBackoffMs = state.ollamaCooldownMs.load(std::memory_order_relaxed);
        WorldSnapshot world = BuildWorldSnapshot(bot);
        bool isIdleCandidate = !snapshot.inCombat && !snapshot.isMoving;
        if (isIdleCandidate)
        {
            if (state.hasLastPosition)
            {
                float distance = Distance(snapshot.pos, state.lastPosition);
                if (distance < kIdlePositionEpsilon)
                {
                    state.idleCycles += 1;
                }
                else
                {
                    state.idleCycles = 0;
                }
            }
            else
            {
                state.idleCycles = 0;
            }
        }
        else
        {
            state.idleCycles = 0;
        }
        state.lastPosition = snapshot.pos;
        state.hasLastPosition = true;
        snapshot.idleCycles = state.idleCycles;

        PendingStrategicUpdate strategicUpdate;
        bool hasStrategicUpdate = false;

        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto stratIt = pendingStrategicUpdates.find(guid);
            if (stratIt != pendingStrategicUpdates.end())
            {
                strategicUpdate = stratIt->second;
                pendingStrategicUpdates.erase(stratIt);
                hasStrategicUpdate = true;
            }
        }

        if (hasStrategicUpdate && strategicUpdate.hasUpdate)
        {
            bool hasLongTermGoal = !state.longTermGoal.empty();
            bool canGenerateGoal = true;

            if (!hasLongTermGoal && state.lastGoalChangeMs > 0 && nowMs - state.lastGoalChangeMs < kStrategicGoalChangeCooldownMs)
            {
                canGenerateGoal = false;
            }

            bool longTermChanged = strategicUpdate.plan.longTermGoal != state.longTermGoal;

            if (longTermChanged)
            {
                if (hasLongTermGoal || canGenerateGoal)
                {
                    state.longTermGoal = strategicUpdate.plan.longTermGoal;
                    state.shortTermIndex.store(0, std::memory_order_relaxed);
                    if (strategicUpdate.refreshedShortTermGoals)
                    {
                        state.shortTermGoals = std::move(strategicUpdate.plan.shortTermGoals);
                    }
                    state.lastGoalChangeMs = nowMs;
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Long-term goal updated for {}: {}", bot->GetName(), state.longTermGoal);
                    state.nextStrategicAllowedMs.store(nowMs + kStrategicGoalChangeCooldownMs, std::memory_order_relaxed);
                }
                else if (g_EnableOllamaBotAmigoDebug && !canGenerateGoal)
                {
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Planner update ignored due to cooldown for {}", bot->GetName());
                }
            }
            else if (strategicUpdate.refreshedShortTermGoals && canGenerateGoal)
            {
                state.longTermGoal = strategicUpdate.plan.longTermGoal;
                state.shortTermGoals = std::move(strategicUpdate.plan.shortTermGoals);
                state.shortTermIndex.store(0, std::memory_order_relaxed);
                state.lastGoalChangeMs = nowMs;
                LOG_INFO("server.loading", "[OllamaBotAmigo] Short-term goals refreshed for {}", bot->GetName());
                state.nextStrategicAllowedMs.store(nowMs + kStrategicGoalChangeCooldownMs, std::memory_order_relaxed);
            }

            if (!state.longTermGoal.empty())
            {
                std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
                BotLLMContext &ctx = GetBotLLMContext()[guid];
                ctx.lastPlan = BuildPlanSummary(state.longTermGoal, state.shortTermGoals,
                                                state.shortTermIndex.load(std::memory_order_relaxed));
            }

            state.hasStrategicResult = true;
        }

        // Out-of-band planner refresh request (e.g., after quest turn-ins).
        // This is guarded at the request site to avoid spamming.
        if (ConsumeLongTermPlannerRefresh(guid) > 0)
        {
            state.forceStrategic.store(true, std::memory_order_relaxed);
            state.nextPlannerLongTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextPlannerShortTickMs.store(nowMs, std::memory_order_relaxed);
            state.nextStrategicAllowedMs.store(0u, std::memory_order_relaxed);
            state.hasStrategicResult = false;
        }

        // Seed next due times if unset (prevents immediate repeated replans after restart).
        if (state.nextPlannerLongTickMs.load(std::memory_order_relaxed) == 0)
        {
            state.nextPlannerLongTickMs.store(nowMs + GetPlannerLongTermDelayMs(), std::memory_order_relaxed);
        }
        if (state.nextPlannerShortTickMs.load(std::memory_order_relaxed) == 0)
        {
            state.nextPlannerShortTickMs.store(nowMs + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
        }

        uint32 nextShortPlanner = state.nextPlannerShortTickMs.load(std::memory_order_relaxed);
        uint32 nextLongPlanner = state.nextPlannerLongTickMs.load(std::memory_order_relaxed);

        bool longTermDue = (!state.hasStrategicResult) || (nowMs >= nextLongPlanner) || state.longTermGoal.empty();
        bool shortTermDue = (!state.hasStrategicResult) || (nowMs >= nextShortPlanner) || state.shortTermGoals.empty();

        // Run planner work only when either layer is due, and keep the lightweight scheduler jitter
        // to avoid thundering herds.
        bool forceStrategic = state.forceStrategic.load(std::memory_order_relaxed);
        bool shouldRunStrategic = g_EnableOllamaBotPlanner &&
                                  (longTermDue || shortTermDue) &&
                                  (forceStrategic || !state.hasStrategicResult || state.scheduler.ShouldRunStrategic(nowMs));
        uint32 nextStrategicAllowedMs = state.nextStrategicAllowedMs.load(std::memory_order_relaxed);
        if (!snapshot.inCombat && shouldRunStrategic && (forceStrategic || nowMs >= nextStrategicAllowedMs) && !state.strategicBusy.exchange(true))
        {
            // Planner runs in a detached thread to avoid blocking the world loop.
            state.forceStrategic.store(false, std::memory_order_relaxed);
            state.promptInFlight.store(true, std::memory_order_relaxed);
            std::string botName = bot->GetName();
            std::string previousLongTermGoal = state.longTermGoal;
            bool hasShortTermGoals = !state.shortTermGoals.empty();
            std::shared_ptr<LlmBotState> stateRef = statePtr;
            bool runLongTerm = longTermDue;
            bool runShortTerm = shortTermDue;

            std::thread([guid, snapshot, world, botName, previousLongTermGoal, hasShortTermGoals, stateRef, runLongTerm, runShortTerm]()
                        {
                            // Planner worker thread.
                            bool loggedSummary = false;
                            auto clearBusy = [&]() {
                                stateRef->strategicBusy.store(false);
                                stateRef->promptInFlight.store(false, std::memory_order_relaxed);
                            };
                            auto rejectAndBackoff = [&](const char* msg) {
                                bool expected = false;
                                if (stateRef->loggedStrategicParseError.compare_exchange_strong(expected, true))
                                {
                                    LOG_ERROR("server.loading", "[OllamaBotAmigo] Planner reply rejected: {}.", msg);
                                }
                                uint32 now = getMSTime();
                                stateRef->nextPlannerShortTickMs.store(now + kPlannerFailureDelayMs, std::memory_order_relaxed);
                                stateRef->nextPlannerLongTickMs.store(now + kPlannerFailureDelayMs, std::memory_order_relaxed);
                                clearBusy();
                            };
                            auto rejectAndBackoffShort = [&](const char* msg)
                            {
                                bool expected = false;
                                if (stateRef->loggedStrategicParseError.compare_exchange_strong(expected, true))
                                    LOG_ERROR("server.loading", "[OllamaBotAmigo] Planner reply rejected: {}.", msg);

                                uint32 now = getMSTime();
                                stateRef->nextPlannerShortTickMs.store(now + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                                clearBusy();
                            };

                            auto rejectAndBackoffLong = [&](const char* msg)
                            {
                                bool expected = false;
                                if (stateRef->loggedStrategicParseError.compare_exchange_strong(expected, true))
                                    LOG_ERROR("server.loading", "[OllamaBotAmigo] Planner reply rejected: {}.", msg);

                                uint32 now = getMSTime();
                                stateRef->nextPlannerLongTickMs.store(now + GetPlannerLongTermDelayMs(), std::memory_order_relaxed);
                                clearBusy();
                            };

                            auto rejectAndBackoffBoth = [&](const char* msg)
                            {
                                bool expected = false;
                                if (stateRef->loggedStrategicParseError.compare_exchange_strong(expected, true))
                                    LOG_ERROR("server.loading", "[OllamaBotAmigo] Planner reply rejected: {}.", msg);

                                uint32 now = getMSTime();
                                stateRef->nextPlannerShortTickMs.store(now + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                                stateRef->nextPlannerLongTickMs.store(now + GetPlannerLongTermDelayMs(), std::memory_order_relaxed);
                                clearBusy();
                            };
                            PendingStrategicUpdate update;

                            // If only short-term goals are due, reuse the existing long-term goal and refresh short-term goals only.
                            std::string longTermGoal;
                            bool needsShortTermGoals = false;

                            if (!runLongTerm && runShortTerm && !previousLongTermGoal.empty())
                            {
                                longTermGoal = previousLongTermGoal;
                                update.plan.longTermGoal = longTermGoal;
                                update.hasUpdate = true;
                                needsShortTermGoals = true;
                            }
                            else
                            {
                                // Long-term planning path (also refreshes short-term goals).
                                if (!g_OllamaBotControlForcedLongTermGoal.empty())
                                {
                                    longTermGoal = g_OllamaBotControlForcedLongTermGoal;

                                    std::string reason;
                                    if (!ValidatePlannerSentence(longTermGoal, reason))
                                    {
                                        rejectAndBackoff("invalid forced long-term goal");
                                        return;
                                    }

                                    update.plan.longTermGoal = longTermGoal;
                                    update.hasUpdate = true;
                                    needsShortTermGoals = !hasShortTermGoals || longTermGoal != previousLongTermGoal;
                                }
                                else
                                {
                                    std::string summary = BuildPlannerStateSummary(snapshot, world);
                                    AppendPlannerStateSummary(botName, summary);
                                    loggedSummary = true;
                                    std::string longTermPrompt = BuildPlannerLongTermPrompt(snapshot, world, std::string());
                                    std::string longTermReply = QueryOllamaLLMOnce(longTermPrompt, g_OllamaBotControlPlannerLongTermModel);
                                    std::string longTermDraft = ExtractPlannerSentence(longTermReply);

                                    if (g_EnableOllamaBotAmigoDebug || g_EnableOllamaBotPlannerDebug)
                                    {
                                        std::string safeReply = EscapeBracesForFmt(longTermReply);
                                        LOG_INFO("server.loading", "[OllamaBotAmigo] Planner long-term draft for '{}':{}", botName, safeReply);
                                    }

                                    if (longTermDraft.empty())
                                    {
                                        rejectAndBackoff("missing long-term goal sentence");
                                        return;
                                    }

                                    {
                                        std::string reason;
                                        if (!ValidatePlannerSentence(longTermDraft, reason))
                                        {
                                            rejectAndBackoff("invalid long-term draft");
                                            return;
                                        }
                                    }

                                    std::string reviewPrompt = BuildLongTermGoalReviewPrompt(snapshot, world, longTermDraft);
                                    std::string reviewReply = QueryOllamaLLMOnce(reviewPrompt, g_OllamaBotControlPlannerLongTermModel);
                                    longTermGoal = ExtractPlannerSentence(reviewReply);

                                    if (g_EnableOllamaBotAmigoDebug || g_EnableOllamaBotPlannerDebug)
                                    {
                                        std::string safeReply = EscapeBracesForFmt(reviewReply);
                                        LOG_INFO("server.loading", "[OllamaBotAmigo] Planner long-term review for '{}':\\n{}", botName, safeReply);
                                    }

                                    if (longTermGoal.empty())
                                    {
                                        rejectAndBackoff("missing long-term goal");
                                        return;
                                    }

                                    {
                                        std::string reason;
                                        if (!ValidatePlannerSentence(longTermGoal, reason))
                                        {
                                            rejectAndBackoff("invalid long-term goal");
                                            return;
                                        }
                                    }

                                    update.plan.longTermGoal = longTermGoal;
                                    update.hasUpdate = true;
                                    needsShortTermGoals = !hasShortTermGoals || longTermGoal != previousLongTermGoal;
                                }
                            }

                            if (needsShortTermGoals)
                            {
                                if (!loggedSummary)
                                {
                                    std::string summary = BuildPlannerStateSummary(snapshot, world);
                                    AppendPlannerStateSummary(botName, summary);
                                    loggedSummary = true;
                                }
                                BotSnapshot::QuestProgress const *focusQuest = FindFocusQuest(snapshot, longTermGoal);
                                std::string focusQuestBlock;
                                if (focusQuest)
                                {
                                    focusQuestBlock = BuildFocusQuestBlock(*focusQuest);
                                }
                                std::string shortTermPrompt = BuildPlannerShortTermPrompt(snapshot, world, std::string(), longTermGoal, focusQuestBlock);
                                std::string shortTermReply = QueryOllamaLLMOnce(shortTermPrompt, g_OllamaBotControlPlannerShortTermModel);

                                if (g_EnableOllamaBotAmigoDebug || g_EnableOllamaBotPlannerDebug)
                                {
                                    std::string safeReply = EscapeBracesForFmt(shortTermReply);
                                    LOG_INFO("server.loading", "[OllamaBotAmigo] Planner short-term goals for '{}':\\n{}", botName, safeReply);
                                }

                                    std::string goal = ParseShortTermGoal(shortTermReply);
                                    std::string reason;
                                    if (focusQuest && MentionsOtherQuest(goal, snapshot.activeQuests, focusQuest->title))
                                    {
                                        rejectAndBackoffShort("short-term goal mentions other quest");
                                        return;
                                    }
                                    if (!ValidateShortTermGoal(goal, reason))
                                    {
                                        rejectAndBackoffShort("invalid short-term goal");
                                        return;
                                    }
                                    update.plan.shortTermGoals = {goal};
                                    update.refreshedShortTermGoals = true;
	                            }

                            // Schedule next planner ticks (separate long vs short intervals).
                            uint32 nowTick = getMSTime();
                            stateRef->nextPlannerShortTickMs.store(nowTick + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                            if (runLongTerm)
                            {
                                stateRef->nextPlannerLongTickMs.store(nowTick + GetPlannerLongTermDelayMs(), std::memory_order_relaxed);
                            }

                            stateRef->loggedStrategicParseError.store(false);
                            EnqueueStrategicUpdate(guid, std::move(update));
                            clearBusy(); })
                .detach();
        }
        // HARD WAIT: if a control request is in flight for this bot, do nothing this tick.
        if (state.controlBusy.load(std::memory_order_relaxed))
        {
            // DO NOT clear or mutate controlBusy here.
            // controlBusy is owned by the response thread only.
            continue;
        }

        bool forceControl = state.forceControl.load(std::memory_order_relaxed);
        if (!g_EnableOllamaBotControl || state.shortTermGoals.empty() ||
            (!forceControl && !state.scheduler.ShouldRunControl(nowMs, guid)))
        {
            continue;
        }

        if (!snapshot.inCombat)
        {
            // Do not plan while already moving (let the movement complete), except allow
            // a stop-grind request so the bot can exit grind mode promptly.
            if (snapshot.isMoving && !snapshot.grindMode)
            {
                continue;
            }

            // If following correctly, avoid unnecessary replans.
            std::string currentActivity;
            std::string activityReason;
            if (TryGetActivityState(bot, currentActivity, activityReason))
            {
                if (NormalizeCommandToken(currentActivity) == "follow" && IsFollowingCorrectly(bot, ai))
                {
                    continue;
                }
            }

            // Cooldown / backoff gate (applies only when not busy).
            uint32 nowAttemptMs = getMSTime();
            uint32 nextAttempt = state.nextAllowedAttemptMs.load(std::memory_order_relaxed);
            if (nowAttemptMs < nextAttempt)
            {
                continue;
            }

            // Set busy ONCE: from here until the response thread clears it, do not plan again.
            if (state.controlBusy.exchange(true, std::memory_order_acq_rel))
            {
                continue; // already waiting on Ollama
            }

            if (forceControl)
            {
                state.forceControl.store(false, std::memory_order_relaxed);
            }

            state.controlState.store(LlmBotState::ControlState::Waiting, std::memory_order_relaxed);
            state.promptInFlight.store(true, std::memory_order_relaxed);

            std::string shortTermGoal = CurrentShortTermGoal(state.shortTermGoals,
                                                             state.shortTermIndex.load(std::memory_order_relaxed));
            std::string prompt = BuildControlPrompt(snapshot, world, state.longTermGoal, state.shortTermGoals,
                                                    state.shortTermIndex.load(std::memory_order_relaxed));
            std::string botName = bot->GetName();
            bool isStopped = ai->HasStrategy("stay", BOT_STATE_NON_COMBAT);
            std::shared_ptr<LlmBotState> stateRef = statePtr;
            size_t shortTermGoalCount = state.shortTermGoals.size();

            std::thread([guid, prompt, botName, snapshot, isStopped, stateRef, shortTermGoalCount]()
                        {
                // Control worker thread that parses tool calls.
                // SINGLE EXIT: all paths funnel through this guard
                auto clearBusy = [&]()
                {
                    stateRef->controlBusy.store(false, std::memory_order_release);
                    stateRef->promptInFlight.store(false, std::memory_order_relaxed);
                };

                auto recordGlobalFailure = [stateRef]()
                {
                    uint32 nowMs = getMSTime();
                    std::lock_guard<std::mutex> lock(globalControlMutex);
                    if (nowMs - globalFailureWindowStartMs > kGlobalFailureWindowMs)
                    {
                        globalFailureWindowStartMs = nowMs;
                        globalFailureCount = 0;
                    }
                    globalFailureCount += 1;
                    if (globalFailureCount >= kGlobalFailureThreshold)
                    {
                        globalControlPauseUntilMs.store(nowMs + kGlobalControlPauseMs, std::memory_order_relaxed);
                        globalFailureWindowStartMs = nowMs;
                        globalFailureCount = 0;
                    }
                };

                auto applyFailureBackoff = [stateRef, recordGlobalFailure, &clearBusy]()
                {
                    uint32 nowMs = getMSTime();
                    uint32 prev = stateRef->ollamaCooldownMs.load(std::memory_order_relaxed);
                    uint32 next = std::min(prev * 2u, kOllamaMaxCooldownMs);
                    stateRef->ollamaCooldownMs.store(std::max(next, kOllamaBaseCooldownMs), std::memory_order_relaxed);
                    uint32 cooldownMs = stateRef->ollamaCooldownMs.load(std::memory_order_relaxed);
                    stateRef->nextAllowedAttemptMs.store(nowMs + cooldownMs, std::memory_order_relaxed);
                    stateRef->failureHoldUntilMs.store(nowMs + kOllamaFailureHoldMs, std::memory_order_relaxed);
                    stateRef->controlState.store(LlmBotState::ControlState::FailureHold, std::memory_order_relaxed);
                    stateRef->nextPlannerShortTickMs.store(nowMs + kPlannerFailureDelayMs, std::memory_order_relaxed);
                    stateRef->nextPlannerLongTickMs.store(nowMs + kPlannerFailureDelayMs, std::memory_order_relaxed);
                    recordGlobalFailure();
                    clearBusy();
                };

                std::string llmReply = QueryOllamaLLMOnce(prompt, g_OllamaBotControlControlModel);

                // If cURL fails, QueryOllamaLLMOnce returns an empty string.
                // Apply exponential backoff to avoid hammering.
                if (llmReply.empty())
                {
                    applyFailureBackoff();
                    return;
                }

                ControlActionState actionState;
                bool hasAction = false;
                std::string trimmed = llmReply;
                size_t start = trimmed.find_first_not_of(" \t\r\n");
                size_t end = trimmed.find_last_not_of(" \t\r\n");
                if (start != std::string::npos && end != std::string::npos)
                {
                    trimmed = trimmed.substr(start, end - start + 1);
                }
                else
                {
                    trimmed.clear();
                }

                if (trimmed.empty())
                {
                    // Treat empty output as a failure and back off.
                    applyFailureBackoff();
                    return;
                }

                ToolCall toolCall;
                std::string toolJson;
                if (!TryExtractSingleToolCall(trimmed, toolCall, toolJson))
                {
                    bool expected = false;
                    if (stateRef->loggedControlParseError.compare_exchange_strong(expected, true))
                    {
                        LOG_ERROR("server.loading", "[OllamaBotAmigo] Control reply rejected: output must be a single <tool_call> block.");
                    }
                    // Parser failures should not retry at tick speed.
                    applyFailureBackoff();
                    return;
                }

                if (g_EnableOllamaBotAmigoDebug || g_EnableOllamaBotControlDebug)
                {
                    std::string safeJson = EscapeBracesForFmt(llmReply);
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Control LLM reply for '{}':\n{}", botName, safeJson);
                }

                ControlToolDefinition definition;
                if (!FindControlToolDefinition(toolCall.name, definition))
                {
                    LogControlToolRejected(toolCall.name, "unknown_tool");
                    stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                    clearBusy();
                    return;
                }
                if (!definition.requiresDirection && !definition.requiresDistance && !definition.requiresQuestId &&
                    !definition.requiresSkill && !definition.requiresIntent && !definition.requiresMessage &&
                    !definition.requiresNavEpoch && !definition.requiresCandidateId &&
                    !toolCall.arguments.empty())
                {
                    LogControlToolRejected(toolCall.name, "unexpected_arguments");
                    stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                    clearBusy();
                    return;
                }

                std::string gateReason = "allowed";
                bool accepted = false;
                ControlAction action;
                action.capability = definition.capability;

                if (definition.capability == ControlAction::Capability::Idle)
                {
                    accepted = true;
                    gateReason = "no_action";
                }
                else if (definition.capability == ControlAction::Capability::MoveHop)
                {
                    uint32 navEpoch = 0;
                    std::string candidateId;
                    if (!ParseMoveHopNavArguments(toolCall.arguments, navEpoch, candidateId))
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "in_grind");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.isMoving)
                    {
                        LogControlToolRejected(toolCall.name, "already_moving");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.travelActive)
                    {
                        LogControlToolRejected(toolCall.name, "travel_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.professionActive)
                    {
                        LogControlToolRejected(toolCall.name, "profession_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (navEpoch != snapshot.navEpoch)
                    {
                        LogControlToolRejected(toolCall.name, "stale_nav_epoch");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    size_t candidateIndex = 0;
                    if (!TryParseNavCandidateIndex(candidateId, candidateIndex) ||
                        candidateIndex >= snapshot.navCandidates.size())
                    {
                        LogControlToolRejected(toolCall.name, "unknown_candidate");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    auto const &cand = snapshot.navCandidates[candidateIndex];
                    if (!cand.canMove)
                    {
                        LogControlToolRejected(toolCall.name, "cannot_move");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (!cand.reachable)
                    {
                        LogControlToolRejected(toolCall.name, "unreachable");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    action.navEpoch = navEpoch;
                    action.navCandidateId = candidateId;
                    accepted = true;
                    gateReason = "out_of_combat";
                }
                else if (definition.capability == ControlAction::Capability::EnterGrind)
                {
                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "already_grinding");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "enter_grind";
                }
                else if (definition.capability == ControlAction::Capability::StopGrind)
                {
                    if (!snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "not_grinding");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "stop_grind";
                }
                else if (definition.capability == ControlAction::Capability::EnterGrind)
                {
                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "out_of_combat";
                }
                else if (definition.capability == ControlAction::Capability::Stay)
                {
                    if (isStopped)
                    {
                        LogControlToolRejected(toolCall.name, "already_stopped");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "stay";
                }
                else if (definition.capability == ControlAction::Capability::Unstay)
                {
                    if (!isStopped)
                    {
                        LogControlToolRejected(toolCall.name, "not_stopped");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    accepted = true;
                    gateReason = "unstay";
                }
                else if (definition.capability == ControlAction::Capability::TalkToQuestGiver)
                {
                    uint32 questId = 0;
                    if (!ParseQuestIdArguments(toolCall.arguments, questId))
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (!HasQuestGiverForQuestId(snapshot, questId))
                    {
                        LogControlToolRejected(toolCall.name, "quest_giver_not_in_range");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    action.questId = questId;
                    accepted = true;
                    gateReason = "quest_giver_in_range";
                }
                else if (definition.capability == ControlAction::Capability::Fish)
                {
                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "in_grind");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.isMoving)
                    {
                        LogControlToolRejected(toolCall.name, "already_moving");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.travelActive)
                    {
                        LogControlToolRejected(toolCall.name, "travel_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.professionActive)
                    {
                        LogControlToolRejected(toolCall.name, "profession_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    // Respect memory cooldowns to avoid spamming fishing attempts.
                    if (BotMemory *mem = BotMemoryRegistry::Get(guid))
                    {
                        FailureStats stats = mem->GetFailureStats("profession:fishing", getMSTime());
                        if (stats.CooldownRemainingMs(getMSTime()) > 0)
                        {
                            LogControlToolRejected(toolCall.name, "cooldown");
                            stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                            stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                            clearBusy();
                            return;
                        }
                        accepted = true;
                        gateReason = "out_of_combat";
                    }
                }
                else if (definition.capability == ControlAction::Capability::UseProfession)
                {
                    std::string skill;
                    std::string intent;
                    if (!ParseProfessionArguments(toolCall.arguments, skill, intent))
                    {
                        LogControlToolRejected(toolCall.name, "invalid_arguments");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    action.professionSkill = skill;
                    action.professionIntent = intent;
                    accepted = true;
                    gateReason = "profession_request";
                }
                else if (definition.capability == ControlAction::Capability::TurnLeft90 ||
                         definition.capability == ControlAction::Capability::TurnRight90 ||
                         definition.capability == ControlAction::Capability::TurnAround)
                {
                    if (snapshot.inCombat)
                    {
                        LogControlToolRejected(toolCall.name, "in_combat");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.grindMode)
                    {
                        LogControlToolRejected(toolCall.name, "in_grind");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.isMoving)
                    {
                        LogControlToolRejected(toolCall.name, "already_moving");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.travelActive)
                    {
                        LogControlToolRejected(toolCall.name, "travel_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }
                    if (snapshot.professionActive)
                    {
                        LogControlToolRejected(toolCall.name, "profession_active");
                        stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                        stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                        clearBusy();
                        return;
                    }

                    accepted = true;
                    gateReason = "turn";
                }

                if (accepted)
                {
                    actionState.action = action;
                    actionState.reasoning = "";
                    hasAction = true;
                    stateRef->loggedControlParseError.store(false);
                    stateRef->lastControlCapability.store(
                        static_cast<uint8>(action.capability), std::memory_order_relaxed);
                    LogControlToolAccepted(toolCall.name, action.capability, gateReason);

                    // Successful parse/accept: reset backoff.
                    uint32 nowMs = getMSTime();
                    stateRef->ollamaCooldownMs.store(kOllamaBaseCooldownMs, std::memory_order_relaxed);
                    if (action.capability == ControlAction::Capability::EnterGrind)
                    {
                        stateRef->nextAllowedAttemptMs.store(nowMs + kPostEnterGrindControlDelayMs, std::memory_order_relaxed);
                        stateRef->controlState.store(LlmBotState::ControlState::Cooldown, std::memory_order_relaxed);
                    }
                    else
                    {
                        stateRef->nextAllowedAttemptMs.store(0, std::memory_order_relaxed);
                    }
                    stateRef->nextPlannerShortTickMs.store(nowMs + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                }

                if (hasAction && actionState.action.capability != ControlAction::Capability::Idle)
                {
                    {
                        std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
                        BotLLMContext &ctx = GetBotLLMContext()[guid];
                        ctx.lastControlSummary = SummarizeControlAction(actionState.action);
                        ctx.lastControlAtMs = GetNowMs();
                    }
                    if (shortTermGoalCount > 0 &&
                        actionState.action.capability != ControlAction::Capability::MoveHop)
                    {
                        size_t currentIndex = stateRef->shortTermIndex.load(std::memory_order_relaxed);
                        size_t nextIndex = (currentIndex + 1) % shortTermGoalCount;
                        stateRef->shortTermIndex.store(nextIndex, std::memory_order_relaxed);
                    }
                    ControlActionRegistry::Instance().Enqueue(guid, actionState);
                }
                if (!hasAction)
                {
                    stateRef->nextPlannerShortTickMs.store(getMSTime() + GetPlannerShortTermDelayMs(), std::memory_order_relaxed);
                }

                // Clear busy ONLY here (response thread).
                stateRef->controlState.store(LlmBotState::ControlState::Idle, std::memory_order_relaxed);
                clearBusy();
            }).detach();
        }
    }

    if (g_OllamaBotControlClearGoalsOnConfigLoad)
    {
        {
            std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
            auto &ctxMap = GetBotLLMContext();
            for (auto &entry : ctxMap)
            {
                BotLLMContext &ctx = entry.second;
                ctx.longTermGoal.clear();
                ctx.shortTermGoals.clear();
                ctx.shortTermIndex = 0;
                ctx.hasActivePlan = false;
                ctx.lastPlanTimeMs = 0;
                ctx.controlStepsForCurrentGoal = 0;
            }
        }
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pendingStrategicUpdates.clear();
        }
        g_OllamaBotControlClearGoalsOnConfigLoad = false;
    }
}
