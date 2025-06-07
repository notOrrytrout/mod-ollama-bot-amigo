#pragma once
#include "ScriptMgr.h"
#include <string>

extern bool g_EnableOllamaBotControl;
extern std::string g_OllamaBotControlUrl;
extern std::string g_OllamaBotControlModel;
extern bool g_EnableOllamaBotBuddyDebug;
extern bool g_EnableBotBuddyAddon;

class OllamaBotControlConfigWorldScript : public WorldScript
{
public:
    OllamaBotControlConfigWorldScript();
    void OnStartup() override;
};
