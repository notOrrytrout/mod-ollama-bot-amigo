#include "Ai/LlmDispatch.h"
#include "Log.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    std::mutex gQueueMutex;
    std::condition_variable gQueueCv;
    std::deque<std::function<void()>> gQueue;
    std::vector<std::thread> gWorkers;
    std::mutex gCompletionMutex;
    std::deque<std::function<void()>> gCompletions;
    bool gRunning = false;
    bool gAccepting = false;
    uint32_t gMaxQueueDepth = 32;

    std::atomic<uint32_t> gInFlight{0};
    std::atomic<uint64_t> gSubmitted{0};
    std::atomic<uint64_t> gCompleted{0};
    std::atomic<uint64_t> gDroppedQueueFull{0};
    std::atomic<uint64_t> gFailed{0};
    std::mutex gErrorMutex;
    std::string gLastError;

    void RecordError(std::string const& error)
    {
        ++gFailed;
        std::lock_guard<std::mutex> lock(gErrorMutex);
        gLastError = error;
    }

    void WorkerLoop()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(gQueueMutex);
                gQueueCv.wait(lock, [] { return !gRunning || !gQueue.empty(); });
                if (!gRunning && gQueue.empty())
                    return;
                job = std::move(gQueue.front());
                gQueue.pop_front();
                ++gInFlight;
            }

            try
            {
                job();
                ++gCompleted;
            }
            catch (std::exception const& e)
            {
                RecordError(e.what());
                LOG_ERROR("server.loading", "[OllamaBotAmigo] LLM worker exception: {}", e.what());
            }
            catch (...)
            {
                RecordError("unknown worker exception");
                LOG_ERROR("server.loading", "[OllamaBotAmigo] Unknown LLM worker exception.");
            }
            --gInFlight;
        }
    }
}

void AmigoLlmDispatchStart(uint32_t workerCount, uint32_t maxQueueDepth)
{
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gRunning)
        return;
    gRunning = true;
    gAccepting = true;
    gMaxQueueDepth = maxQueueDepth ? maxQueueDepth : 1;
    workerCount = workerCount ? workerCount : 1;
    for (uint32_t i = 0; i < workerCount; ++i)
        gWorkers.emplace_back(WorkerLoop);
}

void AmigoLlmDispatchStop()
{
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        if (!gRunning)
            return;

        // Stop accepting work and discard jobs that have not started yet.
        // Workers that are already inside an HTTP call are allowed to finish,
        // then they observe !gRunning with an empty queue and exit.
        gRunning = false;
        gAccepting = false;
        gQueue.clear();
    }
    gQueueCv.notify_all();

    for (std::thread& worker : gWorkers)
        if (worker.joinable())
            worker.join();
    gWorkers.clear();

    {
        std::lock_guard<std::mutex> lock(gCompletionMutex);
        gCompletions.clear();
    }
}

void AmigoLlmDispatchReconfigure(uint32_t workerCount, uint32_t maxQueueDepth)
{
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        if (gRunning)
        {
            gMaxQueueDepth = maxQueueDepth ? maxQueueDepth : 1;
            uint32_t requestedWorkerCount = workerCount ? workerCount : 1;
            if (requestedWorkerCount != gWorkers.size())
            {
                LOG_INFO("server.loading",
                         "[OllamaBotAmigo] LLM worker count changed in config; restart worldserver to apply it.");
            }
            return;
        }
    }

    AmigoLlmDispatchStart(workerCount, maxQueueDepth);
}

bool AmigoLlmDispatchBeginConfigUpdate()
{
    std::lock_guard<std::mutex> queueLock(gQueueMutex);
    if (!gRunning || !gAccepting || !gQueue.empty() || gInFlight.load())
        return false;

    std::lock_guard<std::mutex> completionLock(gCompletionMutex);
    if (!gCompletions.empty())
        return false;

    gAccepting = false;
    return true;
}

void AmigoLlmDispatchEndConfigUpdate()
{
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gRunning)
        gAccepting = true;
}

bool AmigoLlmDispatchSubmit(std::function<void()> job)
{
    if (!job)
        return false;
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        if (!gRunning || !gAccepting || gQueue.size() >= gMaxQueueDepth)
        {
            ++gDroppedQueueFull;
            return false;
        }
        gQueue.push_back(std::move(job));
        ++gSubmitted;
    }
    gQueueCv.notify_one();
    return true;
}

AmigoLlmDispatchStats AmigoLlmDispatchGetStats()
{
    AmigoLlmDispatchStats stats;
    {
        std::lock_guard<std::mutex> lock(gQueueMutex);
        stats.queued = static_cast<uint32_t>(gQueue.size());
        stats.workers = static_cast<uint32_t>(gWorkers.size());
    }
    {
        std::lock_guard<std::mutex> lock(gCompletionMutex);
        stats.pendingCompletions = static_cast<uint32_t>(gCompletions.size());
    }
    stats.inFlight = gInFlight.load();
    stats.submitted = gSubmitted.load();
    stats.completed = gCompleted.load();
    stats.droppedQueueFull = gDroppedQueueFull.load();
    stats.failed = gFailed.load();
    {
        std::lock_guard<std::mutex> lock(gErrorMutex);
        stats.lastError = gLastError;
    }
    return stats;
}

void AmigoLlmDispatchPostCompletion(std::function<void()> completion)
{
    if (!completion)
        return;
    std::lock_guard<std::mutex> lock(gCompletionMutex);
    gCompletions.push_back(std::move(completion));
}

void AmigoLlmDispatchDrainCompletions(uint32_t maxCallbacks)
{
    for (uint32_t i = 0; i < maxCallbacks; ++i)
    {
        std::function<void()> completion;
        {
            std::lock_guard<std::mutex> lock(gCompletionMutex);
            if (gCompletions.empty())
                break;
            completion = std::move(gCompletions.front());
            gCompletions.pop_front();
        }
        try
        {
            completion();
        }
        catch (std::exception const& e)
        {
            RecordError(std::string("world completion: ") + e.what());
            LOG_ERROR("server.loading", "[OllamaBotAmigo] World completion exception: {}", e.what());
        }
        catch (...)
        {
            RecordError("unknown world completion exception");
            LOG_ERROR("server.loading", "[OllamaBotAmigo] Unknown world completion exception.");
        }
    }
}
