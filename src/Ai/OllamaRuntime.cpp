#include "Ai/OllamaRuntime.h"
#include "Ai/LlmContext.h"
#include <unordered_map>

OllamaBotRuntimeConfig g_OllamaBotRuntime;

namespace
{
    // Storage for per-bot LLM context; referenced through g_OllamaBotRuntime.
    std::unordered_map<uint64, BotLLMContext> s_BotLLMContext;

    struct RuntimeInitializer
    {
        RuntimeInitializer()
        {
            // Publish the shared context map into the runtime config.
            g_OllamaBotRuntime.llm_context = &s_BotLLMContext;
        }
    };

    // Ensure runtime pointers are initialized before scripts run.
    RuntimeInitializer s_RuntimeInitializer;
}
