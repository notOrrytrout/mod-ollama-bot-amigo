#pragma once
#include "Define.h"
#include "ScriptMgr.h"
#include <string>

extern std::string g_OllamaBotControlUrl;
extern std::string g_OllamaBotControlPlannerModel;
extern std::string g_OllamaBotControlPlannerLongTermModel;
extern std::string g_OllamaBotControlPlannerShortTermModel;
extern std::string g_OllamaBotControlControlModel;
extern std::string g_OllamaBotControlPlannerPrompt;
extern std::string g_OllamaBotControlShortTermPrompt;
extern std::string g_OllamaBotControlControlPrompt;
extern std::string g_OllamaBotControlPromptFormat;
extern std::string g_OllamaBotControlBotName;
// LLM timing (milliseconds)
extern uint32 g_OllamaBotControlDelayControlMs; // control request cadence
extern uint32 g_OllamaBotControlDelayStgMs;     // short-term planner delay
extern uint32 g_OllamaBotControlDelayLtgMs;     // long-term planner delay
extern uint32 g_OllamaBotControlDelayStartupMs; // startup delay after bot recognized
extern bool g_EnableOllamaBotAmigoDebug;
extern bool g_EnableOllamaBotPlanner;
extern bool g_EnableOllamaBotControl;
extern bool g_EnableOllamaBotPlannerDebug;
extern bool g_EnableOllamaBotControlDebug;
extern float g_OllamaBotControlNavBaseDistance;
extern float g_OllamaBotControlNavDistanceMultiplier;
extern float g_OllamaBotControlNavMaxDistance;
extern uint32 g_OllamaBotControlNavDistanceBands;
extern bool g_OllamaBotControlClearGoalsOnConfigLoad;
extern bool g_EnableOllamaBotPlannerStateSummaryLog;
extern std::string g_OllamaBotPlannerStateSummaryLogPath;

// Optional planning overrides
extern bool g_OllamaBotControlQuestingOnly;
extern std::string g_OllamaBotControlForcedLongTermGoal;

// Persistent memory toggles (CharacterDatabase)
extern bool g_EnableAmigoPlannerMemory;
extern bool g_EnableAmigoStuckMemory;
extern bool g_EnableAmigoVendorMemory;

// Loads config values and ensures DB tables are present.
class OllamaBotControlConfigWorldScript : public WorldScript
{
public:
    OllamaBotControlConfigWorldScript();
    void OnStartup() override;
    void OnAfterConfigLoad(bool reload) override;

private:
    void LoadConfig();
};
