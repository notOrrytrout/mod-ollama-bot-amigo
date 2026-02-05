#include "Bot/BotNavState.h"

#include "Util/WorldPositionCompat.h"

#include <utility>

std::mutex& BotNavStateRegistry::Mutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<uint64, std::deque<BotNavState>>& BotNavStateRegistry::Storage()
{
    static std::unordered_map<uint64, std::deque<BotNavState>> storage;
    return storage;
}

void BotNavStateRegistry::SetState(uint64 guid, BotNavState const& state)
{
    constexpr size_t kMaxHistory = 32;
    std::lock_guard<std::mutex> lock(Mutex());
    auto& history = Storage()[guid];
    if (!history.empty() && history.back().navEpoch == state.navEpoch)
    {
        history.back() = state;
        return;
    }

    history.push_back(state);
    while (history.size() > kMaxHistory)
    {
        history.pop_front();
    }
}

bool BotNavStateRegistry::TryResolve(
    uint64 guid,
    uint32 navEpoch,
    std::string const& candidateId,
    WorldPosition& outDest,
    bool& outReachable,
    bool& outHasLOS,
    bool& outCanMove)
{
    std::lock_guard<std::mutex> lock(Mutex());

    auto& storage = Storage();
    auto it = storage.find(guid);
    if (it == storage.end())
    {
        return false;
    }

    auto const& history = it->second;
    for (auto stateIt = history.rbegin(); stateIt != history.rend(); ++stateIt)
    {
        if (stateIt->navEpoch != navEpoch)
        {
            continue;
        }

        for (auto const& c : stateIt->candidates)
        {
            if (c.candidateId == candidateId)
            {
                // WorldPosition is an engine type used by Playerbots/Trinity.
                outDest = WorldPosition(c.mapId, c.x, c.y, c.z);
                outReachable = c.reachable;
                outHasLOS = c.hasLOS;
                outCanMove = c.canMove;
                return true;
            }
        }
        return false;
    }

    return false;
}

void BotNavStateRegistry::Clear(uint64 guid)
{
    std::lock_guard<std::mutex> lock(Mutex());
    Storage().erase(guid);
}
