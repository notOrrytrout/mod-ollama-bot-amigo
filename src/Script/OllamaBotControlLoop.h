#pragma once
#include "ScriptMgr.h"
#include <string>

class Player;

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

// Exact compact STATE_JSON most recently supplied to the control model.
std::string GetAmigoLatestControlStateJson();
void RetireAmigoBotState(Player* player);
