#include "Ai/OllamaClient.h"
#include "Ai/ControlDecision.h"
#include "Ai/ControlContract.h"
#include "Script/OllamaBotConfig.h"
#include "Log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <curl/curl.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace
{
    std::atomic<bool> gThinkSuppressed{false};
    std::atomic<bool> gThinkProbeSucceeded{false};
    std::atomic<uint32_t> gLastLatencyMs{0};
    std::mutex gErrorMutex;
    std::string gLastError;
    std::mutex gSettingsMutex;
    std::string gEndpointUrl = "http://localhost:11434/api/generate";
    std::string gProvider = "ollama";
    std::string gApiKey;
    uint32_t gThinkLatencyGuardMs = 8000;
    bool gMockEnable = false;
    bool gMockControlEnable = false;
    bool gMockPlannerEnable = false;
    bool gMockChatEnable = false;
    uint32_t gMockLatencyMs = 25;
    uint32_t gMockFailEvery = 0;
    std::string gMockControlTool = "auto";
    std::string gMockControlArguments = "{}";
    std::string gMockPlannerLongTermResponse = "Continue pursuing the current mission using nearby valid objectives.";
    std::string gMockPlannerShortTermResponse = "Advance the nearest valid objective for the current mission.";
    std::string gMockChatResponse = "Sounds good.";
    std::string gMockDefaultResponse = "OK";
    std::atomic<uint64_t> gMockRequestCounter{0};
    bool gMockRuntimeControlOverride = false;

    struct ClientSettings
    {
        std::string url;
        std::string provider;
        std::string apiKey;
        uint32_t thinkLatencyGuardMs = 0;
        bool mockEnable = false;
        bool mockControlEnable = false;
        bool mockPlannerEnable = false;
        bool mockChatEnable = false;
        uint32_t mockLatencyMs = 0;
        uint32_t mockFailEvery = 0;
        std::string mockControlTool;
        std::string mockControlArguments;
        std::string mockPlannerLongTermResponse;
        std::string mockPlannerShortTermResponse;
        std::string mockChatResponse;
        std::string mockDefaultResponse;
    };

    ClientSettings SnapshotSettings()
    {
        std::lock_guard<std::mutex> lock(gSettingsMutex);
        return {
            gEndpointUrl,
            gProvider,
            gApiKey,
            gThinkLatencyGuardMs,
            gMockEnable,
            gMockControlEnable,
            gMockPlannerEnable,
            gMockChatEnable,
            gMockLatencyMs,
            gMockFailEvery,
            gMockControlTool,
            gMockControlArguments,
            gMockPlannerLongTermResponse,
            gMockPlannerShortTermResponse,
            gMockChatResponse,
            gMockDefaultResponse
        };
    }

    enum class MockRequestKind
    {
        Probe,
        Control,
        PlannerLongTerm,
        PlannerShortTerm,
        Chat,
        Generic
    };

    MockRequestKind ClassifyMockRequest(std::string const& prompt)
    {
        if (prompt.find("Reply only with OK.") != std::string::npos)
            return MockRequestKind::Probe;
        if (prompt.find("control-only executor") != std::string::npos ||
            prompt.find("Available control tools (choose exactly one)") != std::string::npos)
            return MockRequestKind::Control;
        if (prompt.find("write exactly ONE short-term goal") != std::string::npos ||
            prompt.find("short-term goal must be a single plain-text sentence") != std::string::npos)
            return MockRequestKind::PlannerShortTerm;
        if (prompt.find("PROPOSED_LONG_TERM_GOAL") != std::string::npos ||
            prompt.find("Write a single-sentence long-term goal") != std::string::npos)
            return MockRequestKind::PlannerLongTerm;
        if (prompt.find("PLAYER MESSAGE") != std::string::npos ||
            prompt.find("Say one brief in-character party line") != std::string::npos ||
            prompt.find("Reply with one short in-character line") != std::string::npos)
            return MockRequestKind::Chat;
        return MockRequestKind::Generic;
    }

    void SetLastError(std::string const& value);

    char const* MockRequestKindName(MockRequestKind kind)
    {
        switch (kind)
        {
            case MockRequestKind::Probe: return "probe";
            case MockRequestKind::Control: return "control";
            case MockRequestKind::PlannerLongTerm: return "planner_long";
            case MockRequestKind::PlannerShortTerm: return "planner_short";
            case MockRequestKind::Chat: return "chat";
            case MockRequestKind::Generic:
            default: return "generic";
        }
    }

    std::string TrimMockArg(std::string value)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    bool ParseMockArguments(std::string raw, nlohmann::json& arguments, std::string& error)
    {
        raw = TrimMockArg(raw);
        if (raw.empty())
        {
            arguments = nlohmann::json::object();
            error.clear();
            return true;
        }

        try
        {
            arguments = nlohmann::json::parse(raw);
            if (!arguments.is_object())
            {
                error = "mock control arguments must be an object";
                return false;
            }
            error.clear();
            return true;
        }
        catch (...)
        {
            // Fall through. Config/command parsing may strip JSON quotes.
        }

        if (raw.size() >= 2 && raw.front() == '{' && raw.back() == '}')
            raw = raw.substr(1, raw.size() - 2);

        arguments = nlohmann::json::object();
        raw = TrimMockArg(raw);
        if (raw.empty())
        {
            error.clear();
            return true;
        }

        size_t start = 0;
        while (start < raw.size())
        {
            size_t comma = raw.find(',', start);
            std::string token = TrimMockArg(raw.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start));

            size_t sep = token.find('=');
            if (sep == std::string::npos)
                sep = token.find(':');

            if (sep == std::string::npos)
            {
                error = "invalid mock argument '" + token + "'; expected key=value";
                return false;
            }

            std::string key = TrimMockArg(token.substr(0, sep));
            std::string value = TrimMockArg(token.substr(sep + 1));

            if (key.size() >= 2 && key.front() == '"' && key.back() == '"')
                key = key.substr(1, key.size() - 2);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);

            if (key.empty())
            {
                error = "mock argument key is empty";
                return false;
            }

            if (value == "true")
                arguments[key] = true;
            else if (value == "false")
                arguments[key] = false;
            else if (value == "null")
                arguments[key] = nullptr;
            else
            {
                char* end = nullptr;
                errno = 0;
                long long integerValue = std::strtoll(value.c_str(), &end, 10);
                if (errno == 0 && end && *end == '\0')
                {
                    arguments[key] = integerValue;
                }
                else
                {
                    errno = 0;
                    end = nullptr;
                    double numberValue = std::strtod(value.c_str(), &end);
                    if (errno == 0 && end && *end == '\0')
                        arguments[key] = numberValue;
                    else
                        arguments[key] = value;
                }
            }

            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }

        error.clear();
        return true;
    }

    bool ExtractMockControlState(std::string const& prompt, nlohmann::json& state)
    {
        size_t marker = prompt.find("S:\n");
        if (marker == std::string::npos)
            marker = prompt.find("STATE_JSON\n");
        if (marker == std::string::npos)
            return false;

        size_t jsonStart = prompt.find('{', marker);
        size_t jsonEnd = prompt.find("\n\nINSTRUCTIONS", jsonStart);
        if (jsonStart == std::string::npos || jsonEnd == std::string::npos || jsonEnd <= jsonStart)
            return false;

        try
        {
            state = nlohmann::json::parse(prompt.substr(jsonStart, jsonEnd - jsonStart));
            return state.is_object();
        }
        catch (nlohmann::json::exception const&)
        {
            return false;
        }
    }

    std::string MockPlannerShortTermGoal(std::string const& prompt, std::string const& fallback)
    {
        size_t quests = prompt.find("Active quests:\n");
        if (quests == std::string::npos)
            return fallback;

        size_t lineStart = quests + std::string("Active quests:\n").size();
        while (lineStart < prompt.size())
        {
            size_t lineEnd = prompt.find('\n', lineStart);
            if (lineEnd == std::string::npos)
                lineEnd = prompt.size();
            std::string line = prompt.substr(lineStart, lineEnd - lineStart);
            size_t complete = line.find(" (complete)");
            if (complete != std::string::npos && line.rfind("- ", 0) == 0)
            {
                std::string title = line.substr(2, complete - 2);
                return "Turn in the completed quest " + title + ".";
            }

            if (lineEnd == prompt.size())
                break;
            lineStart = lineEnd + 1;
        }

        return fallback;
    }

    std::string MockQuestingControlTool(std::string const& prompt, nlohmann::json& arguments, bool& noOp)
    {
        arguments = nlohmann::json::object();
        noOp = true;
        nlohmann::json state;
        if (!ExtractMockControlState(prompt, state))
            return {};
        auto selection = SelectControlDecision(state);
        if (selection.empty())
            return {};
        arguments = selection.at("arguments");
        noOp = false;
        return selection.at("name").get<std::string>();
    }

    bool ShouldMockRequest(ClientSettings const& settings, MockRequestKind kind)
    {
        switch (kind)
        {
            case MockRequestKind::Control:
                return settings.mockControlEnable;
            case MockRequestKind::PlannerLongTerm:
            case MockRequestKind::PlannerShortTerm:
                return settings.mockPlannerEnable;
            case MockRequestKind::Chat:
                return settings.mockChatEnable;
            case MockRequestKind::Probe:
            case MockRequestKind::Generic:
            default:
                // Preserve the legacy master switch for generic diagnostics and
                // think probes. Role-specific switches only affect their role.
                return settings.mockEnable;
        }
    }

    AmigoOllamaResult RunMockRequest(ClientSettings const& settings, std::string const& prompt, bool think)
    {
        AmigoOllamaResult out;
        out.thinkRequested = think;
        out.thinkUsed = think;

        auto started = std::chrono::steady_clock::now();
        if (settings.mockLatencyMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(settings.mockLatencyMs));

        uint64_t requestNumber = gMockRequestCounter.fetch_add(1) + 1;
        if (settings.mockFailEvery > 0 && requestNumber % settings.mockFailEvery == 0)
        {
            out.httpStatus = 503;
            out.error = "mock injected failure";
            out.latencyMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
            gLastLatencyMs.store(out.latencyMs);
            SetLastError(out.error);
            return out;
        }

        MockRequestKind kind = ClassifyMockRequest(prompt);
        switch (kind)
        {
            case MockRequestKind::Probe:
                out.text = "OK";
                break;
            case MockRequestKind::Control:
            {
                std::string tool = settings.mockControlTool.empty() ? "auto" : settings.mockControlTool;
                nlohmann::json arguments = nlohmann::json::object();
                std::string parseError;
                if (tool == "auto")
                    tool = MockQuestingControlTool(prompt, arguments, out.noOp);
                else if (!ParseMockArguments(settings.mockControlArguments, arguments, parseError))
                {
                    out.httpStatus = 400;
                    out.error = std::string("invalid mock control arguments: ") + parseError;
                    out.latencyMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count());
                    gLastLatencyMs.store(out.latencyMs);
                    SetLastError(out.error);
                    return out;
                }

                if (!out.noOp)
                {
                    nlohmann::json payload = {
                        {"name", tool},
                        {"arguments", arguments}
                    };
                    out.text = "<tool_call>" + payload.dump() + "</tool_call>";
                }
                break;
            }
            case MockRequestKind::PlannerLongTerm:
                out.text = settings.mockPlannerLongTermResponse;
                break;
            case MockRequestKind::PlannerShortTerm:
                out.text = MockPlannerShortTermGoal(prompt, settings.mockPlannerShortTermResponse);
                break;
            case MockRequestKind::Chat:
                out.text = settings.mockChatResponse;
                break;
            case MockRequestKind::Generic:
            default:
                out.text = settings.mockDefaultResponse;
                break;
        }

        out.httpStatus = 200;
        out.latencyMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
        gLastLatencyMs.store(out.latencyMs);

        if (out.text.empty() && !out.noOp)
        {
            out.error = std::string("empty mock response for ") + MockRequestKindName(kind);
            SetLastError(out.error);
            return out;
        }

        out.ok = true;
        SetLastError("");
        if (g_EnableOllamaBotAmigoDebug)
        {
            LOG_INFO("server.loading",
                     "[OllamaBotAmigo] Mock LLM response: request={} kind={} latency_ms={}",
                     requestNumber, MockRequestKindName(kind), out.latencyMs);
        }
        return out;
    }

    size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
    {
        std::string* responseBuffer = static_cast<std::string*>(userp);
        size_t totalSize = size * nmemb;
        responseBuffer->append(static_cast<char*>(contents), totalSize);
        return totalSize;
    }

    void SetLastError(std::string const& value)
    {
        std::lock_guard<std::mutex> lock(gErrorMutex);
        gLastError = value;
    }

    AmigoOllamaResult RunRequest(std::string const& model, std::string const& prompt, bool think)
    {
        AmigoOllamaResult out;
        ClientSettings settings = SnapshotSettings();
        out.thinkRequested = think;
        out.thinkUsed = think;
        if (model.empty())
        {
            out.error = "missing model";
            SetLastError(out.error);
            return out;
        }

        MockRequestKind requestKind = ClassifyMockRequest(prompt);
        if (ShouldMockRequest(settings, requestKind))
            return RunMockRequest(settings, prompt, think);

        CURL* curl = curl_easy_init();
        if (!curl)
        {
            out.error = "curl init failed";
            SetLastError(out.error);
            return out;
        }

        nlohmann::json requestData;
        if (settings.provider == "omlx")
        {
            requestData = {
                {"model", model},
                {"messages", nlohmann::json::array({{{"role", "user"}, {"content", prompt}}})},
                {"stream", false},
                {"enable_thinking", think}
            };
        }
        else
        {
            requestData = {{"model", model}, {"prompt", prompt}, {"stream", false}};
            if (think)
                requestData["think"] = true;
        }

        std::string requestDataStr = requestData.dump();
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!settings.apiKey.empty())
        {
            std::string auth = "Authorization: Bearer " + settings.apiKey;
            headers = curl_slist_append(headers, auth.c_str());
        }
        std::string responseBuffer;

        curl_easy_setopt(curl, CURLOPT_URL, settings.url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestDataStr.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(requestDataStr.length()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);

        auto started = std::chrono::steady_clock::now();
        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        out.httpStatus = httpCode;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        out.latencyMs = elapsed > 0 ? static_cast<uint32_t>(elapsed) : 0;
        gLastLatencyMs.store(out.latencyMs);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK)
        {
            out.error = curl_easy_strerror(res);
            SetLastError(out.error);
            return out;
        }
        if (httpCode < 200 || httpCode >= 300)
        {
            out.error = "HTTP " + std::to_string(httpCode) + ": " + responseBuffer;
            SetLastError(out.error);
            return out;
        }

        bool responseFound = false;
        try
        {
            nlohmann::json j = nlohmann::json::parse(responseBuffer);
            if (settings.provider == "omlx")
            {
                if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty())
                {
                    auto const& choice = j["choices"][0];
                    if (choice.contains("message") && choice["message"].is_object())
                    {
                        auto const& message = choice["message"];
                        if (message.contains("content") && message["content"].is_string())
                        {
                            responseFound = true;
                            out.text = message["content"].get<std::string>();
                        }
                    }
                }
            }
            else if (j.contains("response") && j["response"].is_string())
            {
                responseFound = true;
                out.text = j["response"].get<std::string>();
            }
        }
        catch (...)
        {
            // Ollama may still return newline-delimited JSON behind some proxies.
            if (settings.provider != "omlx")
            {
                std::stringstream ss(responseBuffer);
                std::string line;
                while (std::getline(ss, line))
                {
                    try
                    {
                        nlohmann::json j = nlohmann::json::parse(line);
                        if (j.contains("response") && j["response"].is_string())
                        {
                            responseFound = true;
                            out.text += j["response"].get<std::string>();
                        }
                    }
                    catch (...) {}
                }
            }
        }

        if (TrimMockArg(out.text).empty())
        {
            if (responseFound && ClassifyMockRequest(prompt) == MockRequestKind::Control)
            {
                // An empty control response is an intentional no-op. It must
                // not become request_idle or trigger parser-error backoff.
                out.noOp = true;
                out.ok = true;
                out.httpStatus = httpCode;
                SetLastError("");
                return out;
            }
            out.error = "empty LLM response";
            SetLastError(out.error);
            return out;
        }

        out.ok = true;
        SetLastError("");
        return out;
    }

    bool IsThinkRejection(AmigoOllamaResult const& result)
    {
        if (result.httpStatus != 400 && result.httpStatus != 422 && result.httpStatus != 500)
            return false;
        std::string lower = result.error;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("does not support thinking") != std::string::npos ||
               lower.find("does not support think") != std::string::npos ||
               lower.find("thinking is not supported") != std::string::npos ||
               lower.find("unsupported parameter: think") != std::string::npos ||
               lower.find("enable_thinking") != std::string::npos ||
               lower.find("reasoning") != std::string::npos;
    }
}

AmigoOllamaResult QueryOllamaLLMEx(std::string const& model, std::string const& prompt, bool requestThink)
{
    ClientSettings settings = SnapshotSettings();
    bool tryThink = requestThink && !gThinkSuppressed.load();
    AmigoOllamaResult result = RunRequest(model, prompt, tryThink);

    if (tryThink && !result.ok && IsThinkRejection(result))
    {
        // Only a clear model/parameter rejection disables think. Network and
        // generic server failures must not permanently change capability.
        gThinkSuppressed.store(true);
        LOG_INFO("server.loading", "[OllamaBotAmigo] LLM provider/model rejected think mode; retrying without think for this session.");
        result = RunRequest(model, prompt, false);
        result.thinkRequested = true;
    }

    if (tryThink && result.ok)
        gThinkProbeSucceeded.store(true);

    if (tryThink && result.ok && settings.thinkLatencyGuardMs > 0 && result.latencyMs > settings.thinkLatencyGuardMs)
    {
        gThinkSuppressed.store(true);
        LOG_INFO("server.loading", "[OllamaBotAmigo] Think mode latency {} ms exceeded guard {} ms; disabling think for this session.",
                 result.latencyMs, settings.thinkLatencyGuardMs);
    }
    return result;
}

std::string QueryOllamaLLM(std::string const& model, std::string const& prompt)
{
    return QueryOllamaLLMEx(model, prompt, false).text;
}

void PublishOllamaClientConfig()
{
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    gEndpointUrl = g_OllamaBotControlUrl;
    gProvider = g_AmigoLlmProvider;
    std::transform(gProvider.begin(), gProvider.end(), gProvider.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (gProvider != "omlx")
        gProvider = "ollama";
    gApiKey = g_AmigoLlmApiKey;
    gThinkLatencyGuardMs = g_AmigoThinkMaxLatencyMs;
    gMockEnable = g_AmigoMockEnable;
    gMockControlEnable = g_AmigoMockControlEnable;
    gMockPlannerEnable = g_AmigoMockPlannerEnable;
    gMockChatEnable = g_AmigoMockChatEnable;
    gMockLatencyMs = g_AmigoMockLatencyMs;
    gMockFailEvery = g_AmigoMockFailEvery;
    if (!gMockRuntimeControlOverride)
    {
        gMockControlTool = g_AmigoMockControlTool;
        gMockControlArguments = g_AmigoMockControlArguments;
    }
    gMockPlannerLongTermResponse = g_AmigoMockPlannerLongTermResponse;
    gMockPlannerShortTermResponse = g_AmigoMockPlannerShortTermResponse;
    gMockChatResponse = g_AmigoMockChatResponse;
    gMockDefaultResponse = g_AmigoMockDefaultResponse;
}

void ResetOllamaThinkCapability()
{
    gThinkSuppressed.store(false);
    gThinkProbeSucceeded.store(false);
    gLastLatencyMs.store(0);
    gMockRequestCounter.store(0);
    SetLastError("");
}

bool IsOllamaThinkSuppressed() { return gThinkSuppressed.load(); }
std::string GetOllamaThinkCapabilityStatus()
{
    if (gThinkSuppressed.load())
        return "unsupported_or_latency_disabled";
    if (gThinkProbeSucceeded.load())
        return "supported";
    return "unknown";
}

uint32_t GetOllamaLastLatencyMs() { return gLastLatencyMs.load(); }
std::string GetOllamaLastError()
{
    std::lock_guard<std::mutex> lock(gErrorMutex);
    return gLastError;
}

std::string GetAmigoLlmProvider()
{
    return SnapshotSettings().provider;
}

std::string GetAmigoLlmEndpoint()
{
    return SnapshotSettings().url;
}

bool IsAmigoLlmMockEnabled()
{
    return SnapshotSettings().mockEnable;
}

bool IsAmigoLlmMockControlEnabled()
{
    return SnapshotSettings().mockControlEnable;
}

bool IsAmigoLlmMockPlannerEnabled()
{
    return SnapshotSettings().mockPlannerEnable;
}

bool IsAmigoLlmMockChatEnabled()
{
    return SnapshotSettings().mockChatEnable;
}

uint32_t GetAmigoLlmMockLatencyMs()
{
    return SnapshotSettings().mockLatencyMs;
}

uint32_t GetAmigoLlmMockFailEvery()
{
    return SnapshotSettings().mockFailEvery;
}

std::string GetAmigoLlmMockControlTool()
{
    return SnapshotSettings().mockControlTool;
}

std::string GetAmigoLlmMockControlArguments()
{
    return SnapshotSettings().mockControlArguments;
}

bool SetAmigoLlmMockControlRuntime(std::string const& tool, std::string const& argumentsJson, std::string& error)
{
    if (tool.empty())
    {
        error = "tool name is empty";
        return false;
    }

    nlohmann::json arguments;
    if (!ParseMockArguments(argumentsJson.empty() ? "{}" : argumentsJson, arguments, error))
        return false;
    if (tool != "auto")
    {
        auto const* definition = FindControlAction(tool);
        if (!definition)
        {
            error = "unsupported_action";
            return false;
        }
        if (!ValidateControlArguments(*definition, arguments, error))
            return false;
    }

    std::lock_guard<std::mutex> lock(gSettingsMutex);
    gMockControlTool = tool;
    gMockControlArguments = arguments.dump();
    gMockRuntimeControlOverride = true;
    error.clear();
    return true;
}

void ClearAmigoLlmMockControlRuntime()
{
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    gMockRuntimeControlOverride = false;
    gMockControlTool = g_AmigoMockControlTool;
    gMockControlArguments = g_AmigoMockControlArguments;
}

bool IsAmigoLlmMockControlRuntimeOverride()
{
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    return gMockRuntimeControlOverride;
}
