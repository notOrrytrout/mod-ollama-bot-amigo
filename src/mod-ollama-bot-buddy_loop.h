#pragma once
#include "ScriptMgr.h"
#include <string>

class OllamaBotControlLoop : public WorldScript
{
public:
    OllamaBotControlLoop();
    void OnUpdate(uint32 diff) override;
};

void AddBotCommandHistory(Player* bot, const std::string& command);
void AddBotReasoningHistory(Player* bot, const std::string& reasoning);

std::vector<std::string> GetBotCommandHistory(Player* bot);
std::vector<std::string> GetBotReasoningHistory(Player* bot);

std::string EscapeBracesForFmt(const std::string& input);
