#include "Ai/OllamaClient.h"
#include "Script/OllamaBotConfig.h"
#include "Log.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
    // cURL callback: append incoming data into the response buffer.
    std::string* responseBuffer = static_cast<std::string*>(userp);
    size_t totalSize = size * nmemb;
    responseBuffer->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

std::string QueryOllamaLLM(const std::string& model, const std::string& prompt)
{
    // Synchronous call used by external tooling (planner/control loop uses a separate helper).
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo][Ollama] Failed to initialize cURL.");
        return "";
    }

    nlohmann::json requestData = {
        {"model",  model},
        {"prompt", prompt}
    };
    std::string requestDataStr = requestData.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    std::string responseBuffer;
    curl_easy_setopt(curl, CURLOPT_URL, g_OllamaBotControlUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestDataStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(requestDataStr.length()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo][Ollama] Failed to reach Ollama AI. cURL error: {}", curl_easy_strerror(res));
        return "";
    }

    std::stringstream ss(responseBuffer);
    std::string line, extracted;
    while (std::getline(ss, line))
    {
        try
        {
            nlohmann::json jsonResponse = nlohmann::json::parse(line);
            if (jsonResponse.contains("response"))
                extracted += jsonResponse["response"].get<std::string>();
        }
        catch (...) {}
    }
    return extracted;
}
