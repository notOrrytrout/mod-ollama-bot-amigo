#pragma once
#include "Define.h"
#include "ScriptMgr.h"
#include <string>

extern std::string g_OllamaBotControlUrl;
// LLM HTTP provider: ollama or omlx.
extern std::string g_AmigoLlmProvider;
// Optional bearer token for OpenAI-compatible providers such as remote oMLX.
extern std::string g_AmigoLlmApiKey;

// Mock LLM overlay. When enabled, requests still pass through the normal
// dispatcher/planner/control/chat paths, but the HTTP call is replaced with
// deterministic canned responses.
extern bool g_AmigoMockEnable;
extern uint32 g_AmigoMockLatencyMs;
extern uint32 g_AmigoMockFailEvery;
extern std::string g_AmigoMockControlTool;
extern std::string g_AmigoMockControlArguments;
extern std::string g_AmigoMockPlannerLongTermResponse;
extern std::string g_AmigoMockPlannerShortTermResponse;
extern std::string g_AmigoMockChatResponse;
extern std::string g_AmigoMockDefaultResponse;
extern std::string g_OllamaBotControlPlannerModel;
extern std::string g_OllamaBotControlPlannerLongTermModel;
extern std::string g_OllamaBotControlPlannerShortTermModel;
extern std::string g_OllamaBotControlControlModel;
extern std::string g_OllamaBotControlPlannerPrompt;
extern std::string g_OllamaBotControlShortTermPrompt;
extern std::string g_OllamaBotControlControlPrompt;
extern std::string g_OllamaBotControlPromptFormat;
extern std::string g_OllamaBotControlBotName;
extern bool g_AmigoBotAutoLogin;
extern uint32 g_AmigoBotAutoLoginDelayMs;
extern uint32 g_AmigoBotAutoLoginRetryMs;
// Group authority policy. "peer" prevents Playerbots from treating a human
// group member/leader as Amigo's permanent owner.
extern std::string g_AmigoGroupAuthority;
extern uint32 g_AmigoGroupDirectiveTtlMs;
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
// Typed mission assignment. Kind: quest/gather/grind/pvp_bg/party/raid/goal.
extern std::string g_OllamaBotControlMissionKind;
extern std::string g_OllamaBotControlMissionTarget;
extern std::string g_OllamaBotControlMissionRole;

// Bounded LLM dispatch / model capability controls.
extern uint32 g_AmigoLlmWorkerThreads;
extern uint32 g_AmigoLlmMaxQueueDepth;
extern bool g_AmigoThinkPlanner;
extern bool g_AmigoThinkControl;
extern uint32 g_AmigoThinkMaxLatencyMs;

// Social/personality controls. These never grant gameplay capabilities.
extern bool g_AmigoChatEnable;
extern bool g_AmigoChatPartyOnly;
extern uint32 g_AmigoChatHistorySize;
extern std::string g_AmigoChatCommandPrefixes;
extern bool g_AmigoEventChatterEnable;
extern uint32 g_AmigoEventChatterCooldownMs;
extern bool g_AmigoPersonalityEnable;
extern std::string g_AmigoPersonalityName;
extern std::string g_AmigoPersonalityPrompt;

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
