#include "Ai/OllamaClient.h"
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

    std::string MockQuestingControlTool(std::string const& prompt, nlohmann::json& arguments)
    {
        arguments = nlohmann::json::object();
        nlohmann::json state;
        if (ExtractMockControlState(prompt, state))
        {
            nlohmann::json const& bot = state["bot"];
            bool inCombat = bot.value("in_combat", false);
            bool isMoving = bot.value("is_moving", false);
            if (inCombat || isMoving)
                return "request_idle";

            std::string missionKind = bot["mission"].value("kind", "");
            std::string missionTarget = bot["mission"].value("target", "");
            bool matchingObjectiveFound = false;
            bool matchingObjectiveRemaining = false;
            for (nlohmann::json const& quest : bot.value("active_quests", nlohmann::json::array()))
            {
                // Finish pending turn-ins before starting another engagement.
                // Item objective names can differ from their source creature.
                if (quest.value("status", "") == "complete")
                {
                    if (bot.value("grind_mode", false))
                        return "request_stop_grind";
                    uint32 questId = quest.value("id", 0u);
                    for (auto const& giver : state.value("quest_givers_in_range", nlohmann::json::array()))
                    {
                        for (auto const& id : giver.value("turn_in_quest_ids", nlohmann::json::array()))
                        {
                            if (id == questId)
                            {
                                arguments["quest_id"] = questId;
                                return "request_talk_to_quest_giver";
                            }
                        }
                    }
                    for (auto const& entity : state.value("nearby_entities", nlohmann::json::array()))
                    {
                        if (entity.value("type", "") != "npc")
                            continue;
                        for (auto const& id : entity.value("turn_in_quest_ids", nlohmann::json::array()))
                        {
                            if (id == questId && entity.value("entry_id", 0u) != 0)
                            {
                                arguments["entry_id"] = entity["entry_id"];
                                return "request_move_hop_npc";
                            }
                        }
                    }
                    // Use only server-provided turn-in POIs on this map.
                    for (auto const& poi : quest.value("poi", nlohmann::json::array()))
                    {
                        if (!poi.value("is_turn_in", false) ||
                            poi.value("map_id", 0u) != bot.value("map_id", 0u))
                            continue;
                        std::string direction = poi.value("direction", "");
                        if (direction.empty() || !state.contains("nav"))
                            continue;
                        auto const& nav = state["nav"];
                        for (auto const& candidate : nav.value("candidates", nlohmann::json::array()))
                        {
                            if (candidate.value("can_move", false) && candidate.value("reachable", false) &&
                                candidate.value("direction", "") == direction)
                            {
                                arguments["nav_epoch"] = nav["nav_epoch"];
                                arguments["candidate_id"] = candidate["candidate_id"];
                                return "request_move_hop";
                            }
                        }
                    }
                    return "request_idle";
                }
                if (quest.value("status", "") != "incomplete")
                    continue;
                for (nlohmann::json const& objective : quest.value("objectives", nlohmann::json::array()))
                {
                    if (objective.value("target_name", "") != missionTarget)
                        continue;
                    matchingObjectiveFound = true;
                    if (objective.value("current", 0u) < objective.value("required", 0u))
                        matchingObjectiveRemaining = true;
                }
            }

            if (missionKind == "grind" && matchingObjectiveFound && !matchingObjectiveRemaining)
            {
                if (bot.value("grind_mode", false))
                    return "request_stop_grind";
            }

            if (missionKind == "grind" && !missionTarget.empty())
            {
                if (!matchingObjectiveFound || matchingObjectiveRemaining)
                {
                    for (nlohmann::json const& entity : state.value("nearby_entities", nlohmann::json::array()))
                    {
                        if (entity.value("type", "") == "npc" && entity.value("name", "") == missionTarget)
                        {
                            arguments["entry_id"] = entity.value("entry_id", 0u);
                            return "request_attack_target";
                        }
                    }

                    if (!bot.value("grind_mode", false))
                        return "request_enter_grind";
                }
            }
        }

        if (prompt.find("\"post_grind_decision\":true") == std::string::npos &&
            prompt.find("\"post_grind_decision\": true") == std::string::npos)
            return "request_idle";

        auto firstOptionId = [&](std::string const& prefix) -> uint32
        {
            size_t pos = prompt.find(prefix);
            if (pos == std::string::npos)
                return 0;
            pos += prefix.size();
            size_t end = pos;
            while (end < prompt.size() && std::isdigit(static_cast<unsigned char>(prompt[end])))
                ++end;
            if (end == pos)
                return 0;
            try { return static_cast<uint32>(std::stoul(prompt.substr(pos, end - pos))); }
            catch (...) { return 0; }
        };

        uint32 questId = firstOptionId("turn_in_quest:");
        if (questId == 0)
            questId = firstOptionId("accept_quest:");
        if (questId != 0)
        {
            arguments["quest_id"] = questId;
            return "request_talk_to_quest_giver";
        }
        if (prompt.find("\"repair |") != std::string::npos)
            return "request_repair";
        if (prompt.find("\"sell_grays |") != std::string::npos)
            return "request_vendor_sell";
        if (prompt.find("continue_quest:") != std::string::npos ||
            prompt.find("continue_grind") != std::string::npos)
            return "request_enter_grind";
        return "request_idle";
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
                std::string tool = settings.mockControlTool.empty() ? "request_idle" : settings.mockControlTool;
                nlohmann::json arguments = nlohmann::json::object();
                std::string parseError;
                if (tool == "auto")
                    tool = MockQuestingControlTool(prompt, arguments);
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

                nlohmann::json payload = {
                    {"name", tool},
                    {"arguments", arguments}
                };
                out.text = "<tool_call>" + payload.dump() + "</tool_call>";
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

        if (out.text.empty())
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

        if (settings.mockEnable)
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
                            out.text = message["content"].get<std::string>();
                    }
                }
            }
            else if (j.contains("response") && j["response"].is_string())
            {
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
                            out.text += j["response"].get<std::string>();
                    }
                    catch (...) {}
                }
            }
        }

        if (out.text.empty())
        {
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
