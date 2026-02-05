// Architecture (AzerothCore):
// LLM produces commands → enqueue as planner outputs
// Planner outputs are applied FIFO in PlayerScript::OnPlayerAfterUpdate
// Exactly one planner output per bot per interval
// No bot actions occur in the LLM loop
#pragma once

#include "ScriptMgr.h"
#include "Player.h"
#include "Bot/BotControlApi.h"
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

struct AmigoPlannerState
{
    // Command + reasoning produced by the LLM planner/control pipeline.
    BotControlCommand command;
    std::string reasoning;
};

class AmigoPlannerRegistry
{
public:
    // Singleton FIFO queue per bot for planner output.
    static AmigoPlannerRegistry& Instance();
    void Enqueue(Player* bot, const AmigoPlannerState& plan);
    void Enqueue(uint64 botGuid, const AmigoPlannerState& plan);
    bool TryDequeue(uint64 botGuid, AmigoPlannerState& out);

private:
    std::mutex mutex_;
    std::unordered_map<uint64, std::deque<AmigoPlannerState>> plans_;
};

class AmigoPlannerApplierScript : public PlayerScript
{
public:
    AmigoPlannerApplierScript();
    // Apply queued planner commands on the main thread.
    void OnPlayerAfterUpdate(Player* player, uint32 /*diff*/) override;
};

class AmigoBotLoginScript : public PlayerScript
{
public:
    AmigoBotLoginScript();
    // Reset bot strategies when the bot logs in.
    void OnPlayerLogin(Player* player) override;
};
