#include "Ai/ControlAction.h"

ControlActionRegistry& ControlActionRegistry::Instance()
{
    // Single shared queue for all bots.
    static ControlActionRegistry instance;
    return instance;
}

void ControlActionRegistry::Enqueue(uint64 botGuid, ControlActionState const& action)
{
    if (!botGuid)
    {
        return;
    }

    // Protect the per-bot queue against concurrent writers.
    std::lock_guard<std::mutex> lock(mutex_);
    actions_[botGuid].push_back(action);
}

bool ControlActionRegistry::TryDequeue(uint64 botGuid, ControlActionState& outAction)
{
    // Consume the oldest action (FIFO) for a bot if available.
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = actions_.find(botGuid);
    if (it == actions_.end() || it->second.empty())
    {
        return false;
    }

    outAction = it->second.front();
    it->second.pop_front();
    return true;
}
