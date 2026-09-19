#include "Script/AmigoCommands.h"

#include "Ai/LlmDispatch.h"
#include "Ai/OllamaClient.h"
#include "Script/OllamaBotConfig.h"
#include "Script/OllamaBotControlLoop.h"

#include "Chat.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include <fmt/core.h>
#include <cctype>

using namespace Acore::ChatCommands;

AmigoCommandScript::AmigoCommandScript() : CommandScript("AmigoCommandScript") {}

ChatCommandTable AmigoCommandScript::GetCommands() const
{
    static ChatCommandTable sub = {
        { "status",  HandleStatus,  SEC_ADMINISTRATOR, Console::Yes },
        { "state",   HandleState,   SEC_ADMINISTRATOR, Console::Yes },
        { "mock",    HandleMock,    SEC_ADMINISTRATOR, Console::Yes },
        { "reload",  HandleReload,  SEC_ADMINISTRATOR, Console::Yes },
        { "llmtest", HandleLlmTest, SEC_ADMINISTRATOR, Console::No },
    };
    static ChatCommandTable root = { { "amigo", sub } };
    return root;
}

bool AmigoCommandScript::HandleStatus(ChatHandler* handler)
{
    AmigoLlmDispatchStats stats = AmigoLlmDispatchGetStats();
    handler->SendSysMessage(fmt::format(
        "Amigo LLM: workers={} queued={} in_flight={} completions={} submitted={} completed={} dropped={} failed={}",
        stats.workers, stats.queued, stats.inFlight, stats.pendingCompletions,
        stats.submitted, stats.completed, stats.droppedQueueFull, stats.failed));
    handler->SendSysMessage(fmt::format(
        "Amigo LLM provider={} endpoint='{}' planner='{}' control='{}' think_status={} last_latency_ms={}",
        GetAmigoLlmProvider(), GetAmigoLlmEndpoint(),
        g_OllamaBotControlPlannerModel, g_OllamaBotControlControlModel,
        GetOllamaThinkCapabilityStatus(), GetOllamaLastLatencyMs()));
    handler->SendSysMessage(fmt::format(
        "Amigo mock: enabled={} latency_ms={} fail_every={} tool={} args={}",
        IsAmigoLlmMockEnabled() ? "yes" : "no",
        GetAmigoLlmMockLatencyMs(), GetAmigoLlmMockFailEvery(),
        GetAmigoLlmMockControlTool(), GetAmigoLlmMockControlArguments()));
    handler->SendSysMessage(fmt::format(
        "Amigo mock runtime_override={}",
        IsAmigoLlmMockControlRuntimeOverride() ? "yes" : "no"));
    if (!stats.lastError.empty())
        handler->SendSysMessage(fmt::format("Amigo worker last error: {}", stats.lastError));
    std::string ollamaError = GetOllamaLastError();
    if (!ollamaError.empty())
        handler->SendSysMessage(fmt::format("Amigo LLM last error: {}", ollamaError));
    handler->SendSysMessage(fmt::format(
        "Amigo bot: name={} autologin={} initial_delay_ms={} retry_ms={}",
        g_OllamaBotControlBotName, g_AmigoBotAutoLogin ? "yes" : "no",
        g_AmigoBotAutoLoginDelayMs, g_AmigoBotAutoLoginRetryMs));
    handler->SendSysMessage(fmt::format(
        "Amigo group: authority={} directive_ttl_ms={}",
        g_AmigoGroupAuthority, g_AmigoGroupDirectiveTtlMs));
    handler->SendSysMessage(fmt::format(
        "Amigo social: enabled={} party_only={} personality={} event_chatter={}",
        g_AmigoChatEnable ? "yes" : "no", g_AmigoChatPartyOnly ? "yes" : "no",
        g_AmigoPersonalityEnable ? g_AmigoPersonalityName : "off",
        g_AmigoEventChatterEnable ? "yes" : "no"));
    return true;
}

bool AmigoCommandScript::HandleReload(ChatHandler* handler)
{
    sConfigMgr->Reload();
    handler->SendSysMessage("Amigo: configuration reload requested. Runtime workers/model capability are refreshed by the config hook.");
    return true;
}

bool AmigoCommandScript::HandleState(ChatHandler* handler)
{
    std::string state = GetAmigoLatestControlStateJson();
    if (state.empty())
    {
        handler->SendSysMessage("Amigo state: no control snapshot has been built yet.");
        return true;
    }

    handler->SendSysMessage("Amigo latest control STATE_JSON:");
    constexpr size_t kChunk = 220;
    for (size_t pos = 0; pos < state.size(); pos += kChunk)
        handler->SendSysMessage(state.substr(pos, kChunk));
    return true;
}

bool AmigoCommandScript::HandleMock(ChatHandler* handler, Optional<std::string> input)
{
    if (!input || input->empty())
    {
        handler->SendSysMessage("Usage: amigo mock <tool> <args> | amigo mock clear");
        handler->SendSysMessage("Example: amigo mock request_attack_target entry_id=705");
        return true;
    }

    std::string text = *input;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();

    if (text == "clear")
    {
        ClearAmigoLlmMockControlRuntime();
        handler->SendSysMessage("Amigo mock runtime override cleared; config values are active.");
        return true;
    }

    size_t split = text.find_first_of(" \t");
    std::string tool = split == std::string::npos ? text : text.substr(0, split);
    std::string args = "{}";
    if (split != std::string::npos)
    {
        size_t firstArg = text.find_first_not_of(" \t", split);
        if (firstArg != std::string::npos)
            args = text.substr(firstArg);
    }

    std::string error;
    if (!SetAmigoLlmMockControlRuntime(tool, args, error))
    {
        handler->SendSysMessage(fmt::format("Amigo mock rejected: {}", error));
        return true;
    }

    handler->SendSysMessage(fmt::format(
        "Amigo mock runtime override: tool={} args={}",
        GetAmigoLlmMockControlTool(), GetAmigoLlmMockControlArguments()));
    return true;
}

bool AmigoCommandScript::HandleLlmTest(ChatHandler* handler, Optional<std::string> prompt)
{
    if (!prompt || prompt->empty())
    {
        handler->SendSysMessage("Usage: .amigo llmtest <prompt>");
        return true;
    }

    Player* requester = handler->GetPlayer();
    if (!requester)
    {
        handler->SendSysMessage("Amigo llmtest requires an in-game administrator so the async result has a delivery target.");
        return true;
    }

    uint64 requesterGuid = requester->GetGUID().GetRawValue();
    std::string promptCopy = *prompt;
    std::string model = g_OllamaBotControlControlModel;

    if (!AmigoLlmDispatchSubmit([requesterGuid, promptCopy = std::move(promptCopy), model = std::move(model)]() mutable
    {
        AmigoOllamaResult result = QueryOllamaLLMEx(model, promptCopy, false);
        AmigoLlmDispatchPostCompletion([requesterGuid, result = std::move(result)]() mutable
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(requesterGuid));
            if (!player || !player->GetSession())
                return;
            ChatHandler out(player->GetSession());
            if (result.ok)
                out.SendSysMessage(fmt::format("Amigo llmtest [{} ms]: {}", result.latencyMs, result.text));
            else
                out.SendSysMessage(fmt::format("Amigo llmtest failed [{} ms]: {}", result.latencyMs, result.error));
        });
    }))
    {
        handler->SendSysMessage("Amigo llmtest was dropped because the LLM queue is full.");
        return true;
    }

    handler->SendSysMessage("Amigo llmtest queued.");
    return true;
}
