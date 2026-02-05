#include "Ai/LlmRoles.h"
#include "Script/OllamaBotConfig.h"

OllamaSettings GetOllamaSettings()
{
    // Load prompt text from the current config globals.
    return {
        g_OllamaBotControlPlannerPrompt,
        g_OllamaBotControlShortTermPrompt,
        g_OllamaBotControlControlPrompt
    };
}
const std::string& GetPrompt(LLMRole role, const OllamaSettings& settings)
{
    // Choose which prompt to send based on role.
    switch (role)
    {
        case LLMRole::Planner:
            return settings.planner_prompt;
        case LLMRole::Control:
            return settings.control_prompt;
        case LLMRole::PlannerLongTerm:
            // Use the planner prompt for long-term planning by default.
            return settings.planner_prompt;
        case LLMRole::PlannerShortTerm:
            // Prefer the dedicated short-term prompt; fall back to planner prompt.
            return settings.short_term_prompt.empty() ? settings.planner_prompt : settings.short_term_prompt;
}

    return settings.control_prompt;
}
