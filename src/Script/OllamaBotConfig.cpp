#include "Script/OllamaBotConfig.h"
#include "Ai/LlmPrompts.h"
#include "Db/BotMemory.h"
#include "Ai/OllamaRuntime.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"

std::string g_OllamaBotControlUrl = "http://localhost:11434/api/generate";
std::string g_OllamaBotControlPlannerModel = "ministral-3:3b";
std::string g_OllamaBotControlPlannerLongTermModel = "";
std::string g_OllamaBotControlPlannerShortTermModel = "";
std::string g_OllamaBotControlControlModel = "ministral-3:3b";
std::string g_OllamaBotControlPlannerPrompt = "";
std::string g_OllamaBotControlShortTermPrompt = "";
std::string g_OllamaBotControlControlPrompt = "";
std::string g_OllamaBotControlPromptFormat = "debug";
std::string g_OllamaBotControlBotName = "Ollamatest";
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

void OllamaBotControlConfigWorldScript::LoadConfig()
{
    // Read configuration and initialize tables/state as needed.
    g_OllamaBotControlUrl = sConfigMgr->GetOption<std::string>("OllamaBotControl.Url", "http://localhost:11434/api/generate");
    g_OllamaBotControlPlannerModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.Planner", "ministral-3:3b");
    g_OllamaBotControlPlannerLongTermModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.PlannerLongTerm", "");
    g_OllamaBotControlPlannerShortTermModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.PlannerShortTerm", "");
    g_OllamaBotControlControlModel = sConfigMgr->GetOption<std::string>("OllamaBotControl.Model.Control", "ministral-3:3b");
    g_OllamaBotControlBotName = sConfigMgr->GetOption<std::string>("OllamaBotControl.BotName", "Ollamatest");
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
}
