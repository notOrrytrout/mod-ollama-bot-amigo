#pragma once

#include <string>

// Built-in prompts used when config values are empty.
const std::string& GetDefaultPlannerPrompt();
const std::string& GetDefaultControlPrompt();
const std::string& GetDefaultShortTermPrompt();
