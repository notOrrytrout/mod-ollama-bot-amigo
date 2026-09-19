#pragma once
#include "Player.h"
#include "Ai/ControlAction.h"
#include <string>
#include <vector>

enum class BotControlCommandType
{
    // Movement hop using a target coordinate.
    MoveHop,
    // A raw Playerbot command string. Kept only when Playerbots has no direct API.
    PlayerbotCommand,
    // A registered Playerbots Action executed through PlayerbotAI::DoSpecificAction.
    PlayerbotAction,
    // One or more direct Playerbots strategy changes.
    PlayerbotStrategy,
    // No-op command placeholder.
    Idle
};

struct BotControlCommand
{
    BotControlCommandType type;
    // Arguments used by PlayerbotCommand/PlayerbotAction/PlayerbotStrategy.
    // PlayerbotAction: args[0] = action, optional args[1] = qualifier.
    // PlayerbotStrategy: each arg is "nc:<change>", "co:<change>", or "de:<change>".
    std::vector<std::string> args;
    // Optional movement target used by MoveHop.
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
    // Optional clamp distance used by MoveHop.
    float distance = 0.0f;
};

// Execute a named Playerbots action directly through PlayerbotAI::DoSpecificAction.
// Use this for existing Playerbots behaviors such as food, drink, flee, loot,
// hearthstone, repair, and other registered actions that do not need chat parsing.
bool ExecutePlayerbotAction(Player* bot, std::string const& action, std::string const& qualifier = "");

// Execute a command immediately against the Playerbot AI.
bool HandleBotControlCommand(Player* bot, BotControlCommand const& command);
// Execute and record success/failure for stuck-memory tracking.
bool HandleBotControlCommandTracked(Player* bot, BotControlCommand const& command);
// Convenience handler for raw command strings.
bool ParseBotControlCommand(Player* bot, std::string const& commandStr);
// Queue a command for the planner applier.
bool EnqueueBotControlCommand(
    Player* bot,
    BotControlCommand const& command,
    std::string const& reasoning);
// Emit delayed logs for strategy changes (command → AI state).
void PollPendingStrategyLogs(Player* bot);

// Debug-friendly string representation of a command.
std::string FormatCommandString(BotControlCommand const& command);

bool ResolveCapabilityCommand(ControlAction::Capability capability,
    BotControlCommand& outCommand,
    std::string& outCommandText);

// Get/set the current high-level activity for the bot.
bool TryGetActivityState(Player* bot, std::string& activity, std::string& reason);
void UpdateActivityState(Player* bot, std::string const& activity, std::string const& reason);
