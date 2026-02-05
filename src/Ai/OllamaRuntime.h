#pragma once

#include <mutex>

struct OllamaBotRuntimeConfig
{
    // Enable flags
    bool enable_control = true;

    // Timing (milliseconds)
    int control_tick_ms = 100;
    int control_startup_delay_ms = 20000;

    // Shared LLM runtime state
    void* llm_context = nullptr;
    std::mutex llm_context_mutex;
};

// Global runtime configuration shared across scripts.
extern OllamaBotRuntimeConfig g_OllamaBotRuntime;
