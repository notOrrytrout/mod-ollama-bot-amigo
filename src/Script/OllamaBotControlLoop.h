#pragma once
#include "ScriptMgr.h"
#include <string>

enum class LlmView : uint8
{
    Planner,
    Control
};

// Main world script that orchestrates planner + control ticks.
class OllamaBotControlLoop : public WorldScript
{
public:
    OllamaBotControlLoop();
    // Called every world update tick to run planner/control state machines.
    void OnUpdate(uint32 diff) override;
};


// Escape braces for fmt-style logging.
std::string EscapeBracesForFmt(const std::string& input);
