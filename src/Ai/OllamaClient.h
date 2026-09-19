#pragma once

#include <cstdint>
#include <string>

struct AmigoOllamaResult
{
    bool ok = false;
    std::string text;
    std::string error;
    uint32_t latencyMs = 0;
    long httpStatus = 0;
    bool thinkRequested = false;
    bool thinkUsed = false;
};

// Synchronous worker-side request. Never call this from the world thread.
AmigoOllamaResult QueryOllamaLLMEx(std::string const& model, std::string const& prompt, bool requestThink);
std::string QueryOllamaLLM(std::string const& model, std::string const& prompt);

// Publish an immutable worker-side copy of endpoint settings after config load/reload.
void PublishOllamaClientConfig();

// Clears session capability/latency suppression, for model/config reload.
void ResetOllamaThinkCapability();
bool IsOllamaThinkSuppressed();
std::string GetOllamaThinkCapabilityStatus();
std::string GetOllamaLastError();
uint32_t GetOllamaLastLatencyMs();
std::string GetAmigoLlmProvider();
std::string GetAmigoLlmEndpoint();

bool IsAmigoLlmMockEnabled();
uint32_t GetAmigoLlmMockLatencyMs();
uint32_t GetAmigoLlmMockFailEvery();

std::string GetAmigoLlmMockControlTool();
std::string GetAmigoLlmMockControlArguments();

// Runtime-only mock control override. This bypasses ConfigMgr string parsing
// while still passing the generated response through the real control parser.
bool SetAmigoLlmMockControlRuntime(std::string const& tool, std::string const& argumentsJson, std::string& error);
void ClearAmigoLlmMockControlRuntime();
bool IsAmigoLlmMockControlRuntimeOverride();
