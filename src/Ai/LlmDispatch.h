#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct AmigoLlmDispatchStats
{
    uint32_t queued = 0;
    uint32_t inFlight = 0;
    uint32_t workers = 0;
    uint32_t pendingCompletions = 0;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t droppedQueueFull = 0;
    uint64_t failed = 0;
    std::string lastError;
};

void AmigoLlmDispatchStart(uint32_t workerCount, uint32_t maxQueueDepth);
void AmigoLlmDispatchStop();
void AmigoLlmDispatchReconfigure(uint32_t workerCount, uint32_t maxQueueDepth);
bool AmigoLlmDispatchBeginConfigUpdate();
void AmigoLlmDispatchEndConfigUpdate();
bool AmigoLlmDispatchSubmit(std::function<void()> job);
AmigoLlmDispatchStats AmigoLlmDispatchGetStats();
// Queue a callback for world-thread delivery. Worker jobs may call this with
// callbacks that capture only copied data / GUIDs. Player/Map objects must be
// resolved inside the callback when AmigoLlmDispatchDrainCompletions() runs.
void AmigoLlmDispatchPostCompletion(std::function<void()> completion);
void AmigoLlmDispatchDrainCompletions(uint32_t maxCallbacks = 64);
