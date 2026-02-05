#pragma once

#include "Define.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct BotLLMContext
{
    // Last planner goal and control action summaries per bot.
    std::string lastPlan;
    std::string lastControlSummary;
    uint64 lastControlAtMs = 0;
    bool controlBusy = false;

    // Persistent plan state for long/short-term planning and control.
    // The long-term goal currently being pursued. Empty when no plan is active.
    std::string longTermGoal;
    // Ordered list of short-term goals derived from the long-term goal.
    std::vector<std::string> shortTermGoals;
    // Index into shortTermGoals indicating the current short-term goal.
    uint32 shortTermIndex = 0;
    // Whether an active plan exists for this bot. When false, the planner must run.
    bool hasActivePlan = false;
    // Timestamp of the last plan generation (ms since epoch). Used for cooling down planner runs.
    uint64 lastPlanTimeMs = 0;
    // Counter of control steps taken toward the current short-term goal. Reset on advancement.
    uint32 controlStepsForCurrentGoal = 0;
};

// Helpers for manipulating plan state on a per-bot basis. These functions
// operate on the passed-in context and protect access with the shared
// LLM context mutex. They must not call the LLM or enqueue any
// commands.
// Returns true if the context has an active short-term goal.
bool HasCurrentSTG(BotLLMContext const& ctx);
// Returns the current short-term goal or an empty string if none exists.
std::string GetCurrentSTG(BotLLMContext const& ctx);
// Advances to the next short-term goal (clearing the plan if exhausted).
void AdvanceSTG(BotLLMContext& ctx);
// Clears any active long-term and short-term goals and resets plan state.
void ClearPlan(BotLLMContext& ctx);

// Shared runtime context map (keyed by bot GUID).
std::unordered_map<uint64, BotLLMContext>& GetBotLLMContext();
// Shared mutex guarding the LLM context map.
std::mutex& GetBotLLMContextMutex();
