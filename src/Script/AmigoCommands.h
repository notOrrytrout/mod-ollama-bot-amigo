#pragma once

#include "ScriptMgr.h"
#include "Chat.h"
#include <string>

class ChatHandler;

class AmigoCommandScript : public CommandScript
{
public:
    AmigoCommandScript();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;

private:
    static bool HandleStatus(ChatHandler* handler);
    static bool HandleReload(ChatHandler* handler);
    static bool HandleLlmTest(ChatHandler* handler, Optional<std::string> prompt);
    static bool HandleState(ChatHandler* handler);
    static bool HandleMock(ChatHandler* handler, Optional<std::string> input);
};
