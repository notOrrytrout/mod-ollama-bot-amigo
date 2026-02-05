#include "Ai/LlmContext.h"
#include "Ai/OllamaRuntime.h"

std::unordered_map<uint64, BotLLMContext>& GetBotLLMContext()
{
    // Runtime storage is owned by g_OllamaBotRuntime and initialized at startup.
    return *static_cast<std::unordered_map<uint64, BotLLMContext>*>(g_OllamaBotRuntime.llm_context);
}

std::mutex& GetBotLLMContextMutex()
{
    // Expose the shared mutex for thread-safe context updates.
    return g_OllamaBotRuntime.llm_context_mutex;
}

// Plan state helper implementations. These helpers acquire the shared
// LLM context mutex before examining or mutating the passed-in
// BotLLMContext. They intentionally avoid any LLM calls or
// network operations and are safe to call from the main game
// thread or background planner threads.

bool HasCurrentSTG(BotLLMContext const& ctx)
{
    std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
    if (!ctx.hasActivePlan)
    {
        return false;
    }
    return ctx.shortTermIndex < ctx.shortTermGoals.size();
}

std::string GetCurrentSTG(BotLLMContext const& ctx)
{
    std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
    if (!ctx.hasActivePlan || ctx.shortTermIndex >= ctx.shortTermGoals.size())
    {
        return std::string();
    }
    return ctx.shortTermGoals[ctx.shortTermIndex];
}

void AdvanceSTG(BotLLMContext& ctx)
{
    std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
    if (!ctx.hasActivePlan)
    {
        return;
    }
    if (ctx.shortTermIndex < ctx.shortTermGoals.size())
    {
        ctx.shortTermIndex++;
    }
    ctx.controlStepsForCurrentGoal = 0;
    // If the index now exceeds the list, clear the plan.
    if (ctx.shortTermIndex >= ctx.shortTermGoals.size())
    {
        ctx.hasActivePlan = false;
    }
}

void ClearPlan(BotLLMContext& ctx)
{
    std::lock_guard<std::mutex> lock(GetBotLLMContextMutex());
    ctx.longTermGoal.clear();
    ctx.shortTermGoals.clear();
    ctx.shortTermIndex = 0;
    ctx.hasActivePlan = false;
    ctx.lastPlanTimeMs = 0;
    ctx.controlStepsForCurrentGoal = 0;
}
