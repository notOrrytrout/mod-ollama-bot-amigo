#pragma once

#include <string>

// Built-in prompts used when config values are empty.
std::string const& GetDefaultPlannerPrompt();
std::string const& GetDefaultControlPrompt();
std::string const& GetDefaultShortTermPrompt();
