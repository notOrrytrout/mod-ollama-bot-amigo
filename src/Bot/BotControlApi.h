#pragma once
#include "Player.h"
#include "Ai/ControlAction.h"
#include <string>
#include <vector>

enum class BotControlCommandType
{
    // Movement hop using a target coordinate.
    MoveHop,
    // A raw Playerbot command string.
    PlayerbotCommand,
    // No-op command placeholder.
    Idle
};

struct BotControlCommand
{
    BotControlCommandType type;
    // Arguments only used for PlayerbotCommand.
    std::vector<std::string> args;
    // Optional movement target used by MoveHop.
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
    // Optional clamp distance used by MoveHop.
    float distance = 0.0f;
};

// Execute a command immediately against the Playerbot AI.
bool HandleBotControlCommand(Player* bot, const BotControlCommand& command);
// Execute and record success/failure for stuck-memory tracking.
bool HandleBotControlCommandTracked(Player* bot, const BotControlCommand& command);
// Convenience handler for raw command strings.
bool ParseBotControlCommand(Player* bot, const std::string& commandStr);
// Queue a command for the planner applier.
bool EnqueueBotControlCommand(
    Player* bot,
    const BotControlCommand& command,
    std::string const& reasoning);
// Emit delayed logs for strategy changes (command → AI state).
void PollPendingStrategyLogs(Player* bot);

// Debug-friendly string representation of a command.
std::string FormatCommandString(const BotControlCommand& command);

bool ResolveCapabilityCommand(ControlAction::Capability capability,
    BotControlCommand& outCommand,
    std::string& outCommandText);

// Get/set the current high-level activity for the bot.
bool TryGetActivityState(Player* bot, std::string& activity, std::string& reason);
void UpdateActivityState(Player* bot, std::string const& activity, std::string const& reason);
