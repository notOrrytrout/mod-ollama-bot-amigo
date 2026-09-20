#include "Script/OllamaBotConfig.h"
#include "Ai/LlmPrompts.h"
#include "Db/BotMemory.h"
#include "Ai/OllamaRuntime.h"
#include "Ai/LlmDispatch.h"
#include "Ai/OllamaClient.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <cctype>

std::string g_OllamaBotControlUrl = "http://localhost:11434/api/generate";
std::string g_AmigoLlmProvider = "ollama";
std::string g_AmigoLlmApiKey = "";
bool g_AmigoMockEnable = false;
bool g_AmigoMockControlEnable = false;
bool g_AmigoMockPlannerEnable = false;
bool g_AmigoMockChatEnable = false;
uint32 g_AmigoMockLatencyMs = 25;
uint32 g_AmigoMockFailEvery = 0;
std::string g_AmigoMockControlTool = "auto";
std::string g_AmigoMockControlArguments = "{}";
std::string g_AmigoMockPlannerLongTermResponse = "Continue pursuing the current mission using nearby valid objectives.";
std::string g_AmigoMockPlannerShortTermResponse = "Advance the nearest valid objective for the current mission.";
std::string g_AmigoMockChatResponse = "Sounds good.";
std::string g_AmigoMockDefaultResponse = "OK";
std::string g_OllamaBotControlPlannerModel = "ministral-3:3b";
std::string g_OllamaBotControlPlannerLongTermModel = "";
std::string g_OllamaBotControlPlannerShortTermModel = "";
std::string g_OllamaBotControlControlModel = "ministral-3:3b";
std::string g_OllamaBotControlChatModel = "";
std::string g_OllamaBotControlPlannerPrompt = "";
std::string g_OllamaBotControlShortTermPrompt = "";
std::string g_OllamaBotControlControlPrompt = "";
std::string g_OllamaBotControlPromptFormat = "debug";
std::string g_OllamaBotControlBotName = "Ollamatest";
bool g_AmigoBotAutoLogin = true;
uint32 g_AmigoBotAutoLoginDelayMs = 10000;
uint32 g_AmigoBotAutoLoginRetryMs = 30000;
std::string g_AmigoGroupAuthority = "peer";
uint32 g_AmigoGroupDirectiveTtlMs = 120000;
uint32 g_OllamaBotControlDelayControlMs = 15000;
uint32 g_OllamaBotControlDelayStgMs = 15000;
uint32 g_OllamaBotControlDelayLtgMs = 30000;
uint32 g_OllamaBotControlDelayStartupMs = 15000;
bool g_EnableOllamaBotAmigoDebug = false;
bool g_EnableOllamaBotPlanner = true;
bool g_EnableOllamaBotControl = true;
bool g_EnableOllamaBotPlannerDebug = false;
bool g_EnableOllamaBotControlDebug = false;
bool g_EnableAmigoPlannerMemory = true;
bool g_EnableAmigoStuckMemory = true;
bool g_EnableAmigoVendorMemory = true;
float g_OllamaBotControlNavBaseDistance = 6.0f;
float g_OllamaBotControlNavDistanceMultiplier = 2.0f;
float g_OllamaBotControlNavMaxDistance = 60.0f;
uint32 g_OllamaBotControlNavDistanceBands = 3;
bool g_OllamaBotControlClearGoalsOnConfigLoad = false;
bool g_EnableOllamaBotPlannerStateSummaryLog = false;
std::string g_OllamaBotPlannerStateSummaryLogPath = "ollama_planner_state_summary.log";
bool g_OllamaBotControlQuestingOnly = false;
std::string g_OllamaBotControlForcedLongTermGoal = "";
std::string g_OllamaBotControlMissionKind = "quest";
std::string g_OllamaBotControlMissionTarget = "";
std::string g_OllamaBotControlMissionRole = "";
uint32 g_AmigoLlmWorkerThreads = 2;
uint32 g_AmigoLlmMaxQueueDepth = 32;
bool g_AmigoThinkPlanner = true;
bool g_AmigoThinkControl = false;
uint32 g_AmigoThinkMaxLatencyMs = 8000;
bool g_AmigoChatEnable = false;
bool g_AmigoChatPartyOnly = true;
uint32 g_AmigoChatHistorySize = 8;
std::string g_AmigoChatCommandPrefixes = ".,!,/,#";
bool g_AmigoEventChatterEnable = false;
uint32 g_AmigoEventChatterCooldownMs = 60000;
bool g_AmigoPersonalityEnable = false;
std::string g_AmigoPersonalityName = "default";
std::string g_AmigoPersonalityPrompt = "";

std::string ExpandPromptEscapes(std::string const& value)
{
    // Convert escaped sequences from config files into literal characters.
    std::string output;
    output.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        char c = value[i];
        if (c == '\\' && i + 1 < value.size())
        {
            char next = value[i + 1];
            switch (next)
            {
                case 'n':
                    output.push_back('\n');
                    ++i;
                    continue;
                case 'r':
                    output.push_back('\r');
                    ++i;
                    continue;
                case 't':
                    output.push_back('\t');
                    ++i;
                    continue;
                case '\\':
                    output.push_back('\\');
                    ++i;
                    continue;
                case '"':
                    output.push_back('"');
                    ++i;
                    continue;
                default:
                    break;
            }
        }
        output.push_back(c);
    }
    return output;
}

OllamaBotControlConfigWorldScript::OllamaBotControlConfigWorldScript() : WorldScript("OllamaBotControlConfigWorldScript") {}

void OllamaBotControlConfigWorldScript::OnStartup()
{
    LoadConfig();
}

void OllamaBotControlConfigWorldScript::OnAfterConfigLoad(bool /*reload*/)
{
    LoadConfig();
}

void OllamaBotControlConfigWorldScript::OnShutdown()
{
    LOG_INFO("server.loading", "[OllamaBotAmigo] Stopping LLM dispatcher for world shutdown.");
    AmigoLlmDispatchStop();
}

void OllamaBotControlConfigWorldScript::LoadConfig()
{
    // Read configuration and initialize tables/state as needed.
    g_AmigoLlmProvider = sConfigMgr->GetOption<std::string>("OllamaBotControl.Llm.Provider", "ollama");
    g_AmigoLlmApiKey = sConfigMgr->GetOption<std::string>("OllamaBotControl.Llm.ApiKey", "");
    g_AmigoMockEnable = sConfigMgr->GetOption<bool>("OllamaBotControl.Llm.Mock.Enable", false);
    // Per-role mock switches override the legacy master default. This keeps old
    // configs working while allowing control/planner/chat to be mixed freely.
    g_AmigoMockControlEnable = sConfigMgr->GetOption<bool>(
        "OllamaBotControl.Llm.Mock.Control.Enable", g_AmigoMockEnable);
    g_AmigoMockPlannerEnable = sConfigMgr->GetOption<bool>(
        "OllamaBotControl.Llm.Mock.Planner.Enable", g_AmigoMockEnable);
    g_AmigoMockChatEnable = sConfigMgr->GetOption<bool>(
        "OllamaBotControl.Llm.Mock.Chat.Enable", g_AmigoMockEnable);
    g_AmigoMockLatencyMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.Llm.Mock.LatencyMs", 25);
    g_AmigoMockFailEvery = sConfigMgr->GetOption<uint32>("OllamaBotControl.Llm.Mock.FailEvery", 0);
    g_AmigoMockControlTool = sConfigMgr->GetOption<std::string>(
        "OllamaBotControl.Llm.Mock.ControlTool", "auto");
    g_AmigoMockControlArguments = sConfigMgr->GetOption<std::string>(
        "OllamaBotControl.Llm.Mock.ControlArguments", "{}");
    g_AmigoMockPlannerLongTermResponse = ExpandPromptEscapes(sConfigMgr->GetOption<std::string>(
        "OllamaBotControl.Llm.Mock.PlannerLongTermResponse",
        "Continue pursuing the current mission using nearby valid objectives."));
    g_AmigoMockPlannerShortTermResponse = ExpandPromptEscapes(sConfigMgr->GetOption<std::string>(
        "OllamaBotControl.Llm.Mock.PlannerShortTermResponse",
        "Advance the nearest valid objective for the current mission."));
    g_AmigoMockChatResponse = ExpandPromptEscapes(sConfigMgr->GetOption<std::string>(
        "OllamaBotControl.Llm.Mock.ChatResponse", "Sounds good."));
    g_AmigoMockDefaultResponse = ExpandPromptEscapes(sConfigMgr->GetOption<std::string>(
        "OllamaBotControl.Llm.Mock.DefaultResponse", "OK"));
    std::string providerLower = g_AmigoLlmProvider;
    for (char& c : providerLower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    g_AmigoLlmProvider = providerLower == "omlx" ? "omlx" : "ollama";
    std::string defaultLlmUrl = g_AmigoLlmProvider == "omlx"
        ? "http://localhost:8000/v1/chat/completions"
        : "http://localhost:11434/api/generate";
    g_OllamaBotControlUrl = sConfigMgr->GetOption<std::string>("OllamaBotControl.Url", defaultLlmUrl);
    // Keep old configs convenient: switching only Provider to omlx should not
    // accidentally keep posting OpenAI JSON to Ollama's /api/generate path.
    if (g_OllamaBotControlUrl.empty() ||
        (g_AmigoLlmProvider == "omlx" && g_OllamaBotControlUrl == "http://localhost:11434/api/generate"))
        g_OllamaBotControlUrl = defaultLlmUrl;
    g_OllamaBotControlPlannerModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.Planner", "ministral-3:3b");
    g_OllamaBotControlPlannerLongTermModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.PlannerLongTerm", "");
    g_OllamaBotControlPlannerShortTermModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.PlannerShortTerm", "");
    g_OllamaBotControlControlModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.Control", "ministral-3:3b");
    g_OllamaBotControlChatModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.Chat", "");
    if (g_OllamaBotControlChatModel.empty())
        g_OllamaBotControlChatModel = g_OllamaBotControlControlModel;
    g_OllamaBotControlBotName = sConfigMgr->GetOption<std::string>("OllamaBotControl.BotName", "Ollamatest");
    g_AmigoBotAutoLogin = sConfigMgr->GetOption<bool>("OllamaBotControl.Bot.AutoLogin", true);
    g_AmigoBotAutoLoginDelayMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.Bot.AutoLoginDelayMs", 10000);
    g_AmigoBotAutoLoginRetryMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.Bot.AutoLoginRetryMs", 30000);
    g_AmigoGroupAuthority = sConfigMgr->GetOption<std::string>("OllamaBotControl.Group.Authority", "peer");
    for (char& c : g_AmigoGroupAuthority)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (g_AmigoGroupAuthority != "playerbots")
        g_AmigoGroupAuthority = "peer";
    g_AmigoGroupDirectiveTtlMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.Group.DirectiveTtlMs", 120000);
    g_OllamaBotControlDelayControlMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.DelayMs.Control", 15000);
    g_OllamaBotControlDelayStgMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.DelayMs.STG", 15000);
    g_OllamaBotControlDelayLtgMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.DelayMs.LTG", 30000);
    g_OllamaBotControlDelayStartupMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.DelayMs.Startup", 15000);
    g_EnableOllamaBotAmigoDebug = sConfigMgr->GetOption<bool>("OllamaBotControl.Debug", false);
    g_EnableOllamaBotPlanner = sConfigMgr->GetOption<bool>("OllamaBotControl.Planner.Enable", true);
    g_EnableOllamaBotControl = sConfigMgr->GetOption<bool>("OllamaBotControl.Control.Enable", true);
    g_EnableOllamaBotPlannerDebug = sConfigMgr->GetOption<bool>("OllamaBotControl.Planner.Debug", false);
    g_EnableOllamaBotControlDebug = sConfigMgr->GetOption<bool>("OllamaBotControl.Control.Debug", false);
    g_EnableAmigoPlannerMemory = sConfigMgr->GetOption<bool>("OllamaBotControl.EnablePlannerMemory", true);
    g_EnableAmigoStuckMemory = sConfigMgr->GetOption<bool>("OllamaBotControl.EnableStuckMemory", true);
    g_EnableAmigoVendorMemory = sConfigMgr->GetOption<bool>("OllamaBotControl.EnableVendorMemory", true);
    g_OllamaBotControlNavBaseDistance = sConfigMgr->GetOption<float>("OllamaBotControl.Nav.BaseDistance", 6.0f);
    g_OllamaBotControlNavDistanceMultiplier = sConfigMgr->GetOption<float>("OllamaBotControl.Nav.DistanceMultiplier", 2.0f);
    g_OllamaBotControlNavMaxDistance = sConfigMgr->GetOption<float>("OllamaBotControl.Nav.MaxDistance", 60.0f);
    g_OllamaBotControlNavDistanceBands = sConfigMgr->GetOption<uint32>("OllamaBotControl.Nav.DistanceBands", 3);
    g_OllamaBotControlClearGoalsOnConfigLoad = sConfigMgr->GetOption<bool>("OllamaBotControl.ClearGoalsOnConfigLoad", false);
    g_EnableOllamaBotPlannerStateSummaryLog = sConfigMgr->GetOption<bool>("OllamaBotControl.Planner.StateSummaryLog.Enable", false);
    g_OllamaBotPlannerStateSummaryLogPath = sConfigMgr->GetOption<std::string>(
        "OllamaBotControl.Planner.StateSummaryLog.Path", "ollama_planner_state_summary.log");
    g_OllamaBotControlQuestingOnly = sConfigMgr->GetOption<bool>("OllamaBotControl.QuestingOnly", false);
    g_OllamaBotControlForcedLongTermGoal = ExpandPromptEscapes(
        sConfigMgr->GetOption<std::string>("OllamaBotControl.Planner.ForcedLongTermGoal", ""));
    g_OllamaBotControlMissionKind = sConfigMgr->GetOption<std::string>("OllamaBotControl.Mission.Kind", "quest");
    g_OllamaBotControlMissionTarget = ExpandPromptEscapes(
        sConfigMgr->GetOption<std::string>("OllamaBotControl.Mission.Target", ""));
    g_OllamaBotControlMissionRole = sConfigMgr->GetOption<std::string>("OllamaBotControl.Mission.Role", "");
    g_AmigoLlmWorkerThreads = sConfigMgr->GetOption<uint32>("OllamaBotControl.Llm.WorkerThreads", 2);
    g_AmigoLlmMaxQueueDepth = sConfigMgr->GetOption<uint32>("OllamaBotControl.Llm.MaxQueueDepth", 32);
    g_AmigoThinkPlanner = sConfigMgr->GetOption<bool>("OllamaBotControl.Llm.Think.Planner", true);
    g_AmigoThinkControl = sConfigMgr->GetOption<bool>("OllamaBotControl.Llm.Think.Control", false);
    g_AmigoThinkMaxLatencyMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.Llm.Think.MaxLatencyMs", 8000);
    g_AmigoChatEnable = sConfigMgr->GetOption<bool>("OllamaBotControl.Chat.Enable", false);
    g_AmigoChatPartyOnly = sConfigMgr->GetOption<bool>("OllamaBotControl.Chat.PartyOnly", true);
    g_AmigoChatHistorySize = sConfigMgr->GetOption<uint32>("OllamaBotControl.Chat.HistorySize", 8);
    g_AmigoChatCommandPrefixes = sConfigMgr->GetOption<std::string>("OllamaBotControl.Chat.CommandPrefixes", ".,!,/,#");
    g_AmigoEventChatterEnable = sConfigMgr->GetOption<bool>("OllamaBotControl.Chat.EventChatter.Enable", false);
    g_AmigoEventChatterCooldownMs = sConfigMgr->GetOption<uint32>("OllamaBotControl.Chat.EventChatter.CooldownMs", 60000);
    g_AmigoPersonalityEnable = sConfigMgr->GetOption<bool>("OllamaBotControl.Personality.Enable", false);
    g_AmigoPersonalityName = sConfigMgr->GetOption<std::string>("OllamaBotControl.Personality.Name", "default");
    g_AmigoPersonalityPrompt = ExpandPromptEscapes(sConfigMgr->GetOption<std::string>("OllamaBotControl.Personality.Prompt", ""));
    if (g_OllamaBotControlQuestingOnly && g_OllamaBotControlForcedLongTermGoal.empty())
    {
        g_OllamaBotControlForcedLongTermGoal = "Pick up all available nearby quests, complete their objectives, then turn them in.";
    }
    g_OllamaBotControlPromptFormat = sConfigMgr->GetOption<std::string>("OllamaBotControl.PromptFormat", "debug");
    g_OllamaBotControlPlannerPrompt = ExpandPromptEscapes(
        sConfigMgr->GetOption<std::string>("OllamaBotControl.SystemPrompt.Planner", GetDefaultPlannerPrompt()));
    g_OllamaBotControlShortTermPrompt = ExpandPromptEscapes(
        sConfigMgr->GetOption<std::string>("OllamaBotControl.SystemPrompt.ShortTerm", GetDefaultShortTermPrompt()));
    g_OllamaBotControlControlPrompt = ExpandPromptEscapes(
        sConfigMgr->GetOption<std::string>("OllamaBotControl.SystemPrompt.Control", GetDefaultControlPrompt()));

    if (g_OllamaBotControlPlannerLongTermModel.empty())
    {
        g_OllamaBotControlPlannerLongTermModel = g_OllamaBotControlPlannerModel;
    }
    if (g_OllamaBotControlPlannerShortTermModel.empty())
    {
        g_OllamaBotControlPlannerShortTermModel = g_OllamaBotControlPlannerModel;
    }

    // Memory schema creation and housekeeping is centralized in BotMemory.
    BotMemory::EnsureSchema(g_EnableAmigoPlannerMemory, g_EnableAmigoStuckMemory, g_EnableAmigoVendorMemory);

    g_OllamaBotRuntime.enable_control = sConfigMgr->GetOption<bool>("OllamaBotControl.Enable", true);
    g_OllamaBotRuntime.control_tick_ms = static_cast<int32>(g_OllamaBotControlDelayControlMs);
    g_OllamaBotRuntime.control_startup_delay_ms = static_cast<int32>(g_OllamaBotControlDelayStartupMs);

    // Publish immutable worker settings before restarting the queue.
    PublishOllamaClientConfig();
    // Safe on startup and config reload. Existing queued work is discarded on reload.
    AmigoLlmDispatchReconfigure(g_AmigoLlmWorkerThreads, g_AmigoLlmMaxQueueDepth);
    ResetOllamaThinkCapability();
    if (g_AmigoThinkPlanner && !g_OllamaBotControlPlannerModel.empty())
    {
        std::string probeModel = g_OllamaBotControlPlannerModel;
        AmigoLlmDispatchSubmit([probeModel]()
        {
            QueryOllamaLLMEx(probeModel, "Reply only with OK.", true);
        });
    }
}
