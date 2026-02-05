#pragma once

#include <string>

enum class LLMRole
{
    // Planner roles:
    // Planner: legacy role encompassing both long- and short-term planning.
    // PlannerLongTerm: generates a single long-term goal sentence.
    // PlannerShortTerm: generates a single short-term goal sentence.
    Planner,
    Control,
    PlannerLongTerm,
    PlannerShortTerm
};

struct OllamaSettings
{
    // Per-role prompt text loaded from config/defaults.
    std::string planner_prompt;
    std::string short_term_prompt;
    std::string control_prompt;
};

// Aggregate prompt settings from config.
OllamaSettings GetOllamaSettings();
// Select the prompt for a given LLM role.
const std::string& GetPrompt(LLMRole role, const OllamaSettings& settings);
