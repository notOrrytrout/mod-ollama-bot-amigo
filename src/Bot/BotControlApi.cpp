#include "Bot/BotControlApi.h"
#include "Ai/ControlAction.h"
#include "Script/OllamaBotConfig.h"
#include "Script/AmigoPlanner.h"
#include "Bot/BotMovement.h"
#include "Util/WorldChecks.h"
#include "Bot/BotTravel.h"
#include "Db/BotMemory.h"
#include "Util/PlayerbotsCompat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "Map.h"
#include "SharedDefines.h"
#include "Errors.h"
#include <sstream>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cctype>

namespace
{
    // Per-bot activity state surfaced in planner/control prompts.
    struct ActivityState
    {
        std::string activity;
        std::string reason;
    };

    struct PendingStrategyLog
    {
        BotState state = BOT_STATE_NON_COMBAT;
        std::vector<std::string> before;
        std::string command;
        bool pending = false;
    };

    std::unordered_map<uint64, ActivityState> activityStates;
    std::unordered_map<uint64, PendingStrategyLog> pendingStrategyLogs;

    // Extract raw GUID for safe map keys.
    uint64 GetBotGuid(Player* bot)
    {
        return bot ? bot->GetGUID().GetRawValue() : 0;
    }

    // Helper for readable strategy log output.
    std::string JoinStrategyNames(std::vector<std::string> const& strategies)
    {
        std::ostringstream out;
        for (size_t i = 0; i < strategies.size(); ++i)
        {
            if (i != 0)
            {
                out << ", ";
            }
            out << strategies[i];
        }
        return out.str();
    }

    bool IsStrategyCommand(std::string const& command, BotState& outState)
    {
        // "co", "nc", and "de" are the Playerbot strategy prefixes.
        if (command.rfind("co", 0) == 0)
        {
            if (command.size() == 2 || std::isspace(static_cast<unsigned char>(command[2])))
            {
                outState = BOT_STATE_COMBAT;
                return true;
            }
        }
        if (command.rfind("nc", 0) == 0)
        {
            if (command.size() == 2 || std::isspace(static_cast<unsigned char>(command[2])))
            {
                outState = BOT_STATE_NON_COMBAT;
                return true;
            }
        }
        if (command.rfind("de", 0) == 0)
        {
            if (command.size() == 2 || std::isspace(static_cast<unsigned char>(command[2])))
            {
                outState = BOT_STATE_DEAD;
                return true;
            }
        }
        return false;
    }

    bool InjectPlayerbotCommand(Player* bot, std::string const& command, std::string const& origin)
    {
        // Centralized injection so we can log and track strategy changes.
        if (!bot || command.empty())
        {
            return false;
        }

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
        if (!ai)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Injection rejected: no PlayerbotAI for {}", bot->GetName());
            return false;
        }

        Player* sender = ai->GetMaster() ? ai->GetMaster() : bot;
        BotState strategyState = BOT_STATE_NON_COMBAT;
        const bool isStrategy = IsStrategyCommand(command, strategyState);
        if (isStrategy)
        {
            PendingStrategyLog pending;
            pending.state = strategyState;
            pending.before = ai->GetStrategies(strategyState);
            pending.command = command;
            pending.pending = true;
            pendingStrategyLogs[GetBotGuid(bot)] = pending;

            LOG_INFO("server.loading",
                     "[OllamaBotAmigo] Strategy command '{}' queued for {}. Before ({}): [{}]",
                     command,
                     bot->GetName(),
                     static_cast<int>(strategyState),
                     JoinStrategyNames(pending.before));
        }

        LOG_INFO("server.loading",
                 "[OllamaBotAmigo] Injecting Playerbot command via HandleCommand (origin={}): {} -> '{}'",
                 origin,
                 sender ? sender->GetName() : "unknown",
                 command);

        ai->HandleCommand(CHAT_MSG_WHISPER, command, sender);

        LOG_INFO("server.loading", "[OllamaBotAmigo] Playerbot HandleCommand accepted command for {}", bot->GetName());
        return true;
    }

    std::string BuildActionKey(const BotControlCommand& command)
    {
        // Build a key that is stable across retries for stuck-memory tracking.
        switch (command.type)
        {
            case BotControlCommandType::MoveHop:
            {
                int x = static_cast<int>(std::round(command.targetX));
                int y = static_cast<int>(std::round(command.targetY));
                int z = static_cast<int>(std::round(command.targetZ));
                std::ostringstream key;
                key << "move_hop:" << x << ":" << y << ":" << z;
                return key.str();
            }
            case BotControlCommandType::PlayerbotCommand:
            {
                if (!command.args.empty())
                {
                    std::string action = command.args[0];
                    if (action.size() > 120)
                    {
                        action.resize(120);
                    }
                    return "command:" + action;
                }
                break;
            }
            default:
                break;
        }
        return "";
    }

    void RecordStuckAttempt(uint64 botGuid, const std::string& actionKey)
    {
        // Increment attempt counts for actions that fail to apply.
        if (!g_EnableAmigoStuckMemory || actionKey.empty())
            return;

        if (BotMemory* memory = BotMemoryRegistry::Get(botGuid))
            memory->RecordFailure(actionKey, FailureType::Retryable, getMSTime());
    }

    void ClearStuckAttempt(uint64 botGuid, const std::string& actionKey)
    {
        // Remove the stuck record once a command succeeds.
        if (!g_EnableAmigoStuckMemory || actionKey.empty())
            return;

        if (BotMemory* memory = BotMemoryRegistry::Get(botGuid))
            memory->ClearFailures(actionKey);
    }

    std::string GetNpcRole(Creature* creature)
    {
        // We only track vendor-like roles for memory.
        if (!creature)
        {
            return "";
        }

        if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR))
        {
            return "vendor";
        }
        if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER))
        {
            return "trainer";
        }
        if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_REPAIR))
        {
            return "repair";
        }

        return "";
    }

    Creature* FindNearestHostileCreature(Player* bot, PlayerbotAI* ai)
    {
        // Pick the closest hostile NPC to auto-select for attack pull.
        if (!bot || !ai)
        {
            return nullptr;
        }

        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
        {
            return nullptr;
        }

        Creature* best = nullptr;
        float bestDistance = 0.0f;
        GuidVector npcs = context->GetValue<GuidVector>("nearest npcs")->Get();
        for (ObjectGuid const& guid : npcs)
        {
            Creature* creature = ai->GetCreature(guid);
            if (!creature)
            {
                continue;
            }
            if (!creature->IsAlive())
            {
                continue;
            }
            if (!creature->IsHostileTo(bot))
            {
                continue;
            }

            float distance = bot->GetDistance(creature);
            if (!best || distance < bestDistance)
            {
                best = creature;
                bestDistance = distance;
            }
        }

        return best;
    }

    void RememberVendorFromSelectedTarget(Player* bot)
    {
        // Persist vendor/trainer/etc. NPCs for later planning context.
        if (!g_EnableAmigoVendorMemory || !bot)
            return;

        BotMemory* memory = BotMemoryRegistry::Get(bot->GetGUID().GetRawValue());
        if (!memory)
            return;

        Unit* targetUnit = bot->GetSelectedUnit();
        if (!targetUnit)
        {
            ObjectGuid targetGuid = bot->GetTarget();
            if (!targetGuid.IsEmpty())
            {
                targetUnit = ObjectAccessor::GetUnit(*bot, targetGuid);
            }
        }
        if (!targetUnit)
        {
            return;
        }

        Creature* npc = targetUnit->ToCreature();
        if (!npc)
        {
            return;
        }

        std::string role = GetNpcRole(npc);
        if (role.empty())
        {
            return;
        }

        uint32 nowMs = getMSTime();
        WorldPosition pos(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
        memory->UpsertVendor(npc->GetEntry(), npc->GetName(), role, bot->GetZoneId(), pos, nowMs);
    }
}

bool ResolveCapabilityCommand(ControlAction::Capability capability,
    BotControlCommand& outCommand,
    std::string& outCommandText)
{
    // Map high-level capabilities to Playerbot commands.
    outCommand = BotControlCommand{};
    outCommandText.clear();

    // TODO: keep capability mappings in sync with Playerbots command handler strategy.
    switch (capability)
    {
        case ControlAction::Capability::EnterGrind:
            outCommandText = "grind";
            break;
        case ControlAction::Capability::StopGrind:
            outCommandText = "follow";
            break;
        case ControlAction::Capability::Stay:
            outCommandText = "stay";
            break;
        case ControlAction::Capability::Unstay:
            outCommandText = "nc -stay";
            break;
        case ControlAction::Capability::TalkToQuestGiver:
            outCommandText = "talk";
            break;
        case ControlAction::Capability::TurnLeft90:
            // Rotate left by 90 degrees via Playerbot command.
            outCommandText = "turnleft";
            break;
        case ControlAction::Capability::TurnRight90:
            // Rotate right by 90 degrees via Playerbot command.
            outCommandText = "turnright";
            break;
        case ControlAction::Capability::TurnAround:
            // Rotate 180 degrees via Playerbot command.
            outCommandText = "turnaround";
            break;
        case ControlAction::Capability::Idle:
        case ControlAction::Capability::MoveHop:
        default:
            return false;
    }

    outCommand.type = BotControlCommandType::PlayerbotCommand;
    outCommand.args = { outCommandText };
    return true;
}

void PollPendingStrategyLogs(Player* bot)
{
    // Emit a log once the AI strategy list differs from the queued command.
    if (!bot)
    {
        return;
    }

    uint64 guid = GetBotGuid(bot);
    auto it = pendingStrategyLogs.find(guid);
    if (it == pendingStrategyLogs.end() || !it->second.pending)
    {
        return;
    }

    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
    if (!ai)
    {
        return;
    }

    std::vector<std::string> after = ai->GetStrategies(it->second.state);
    if (after != it->second.before)
    {
        LOG_INFO("server.loading",
                 "[OllamaBotAmigo] Strategy update applied for {} via '{}'. Before ({}): [{}] After: [{}]",
                 bot->GetName(),
                 it->second.command,
                 static_cast<int>(it->second.state),
                 JoinStrategyNames(it->second.before),
                 JoinStrategyNames(after));
        it->second.pending = false;
    }
}

bool HandleBotControlCommand(Player* bot, const BotControlCommand& command)
{
    // Execute an immediate command (move hop or raw Playerbot instruction).
    if (!bot)
    {
        return false;
    }

    if (g_EnableOllamaBotAmigoDebug)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] HandleBotControlCommand for '{}', type {}", bot->GetName(), int(command.type));
    }

    switch (command.type)
    {
        case BotControlCommandType::MoveHop:
{
    // Start a stateful, path-based movement (no teleports, no manual Z).
    if (bot->IsInCombat())
    {
        if (g_EnableOllamaBotAmigoDebug)
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Move hop ignored during combat for {}", bot->GetName());
        }
        return false;
    }

    uint64 guid = GetBotGuid(bot);
    BotMovement* movement = BotMovementRegistry::Get(guid);
    BotTravel* travel = BotTravelRegistry::Get(guid);
    if (!movement)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Move hop rejected (reason=no_movement) for {}", bot->GetName());
        return false;
    }
    if (!travel)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Move hop rejected (reason=no_travel) for {}", bot->GetName());
        return false;
    }
    if (travel->Active())
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Move hop rejected (reason=travel_active) for {}", bot->GetName());
        return false;
    }

    WorldPosition dest(bot->GetMapId(), command.targetX, command.targetY, command.targetZ);
    if (!WorldChecks::CanReach(bot, dest))
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Move hop rejected (reason=unreachable) for {}", bot->GetName());
        return false;
    }
    if (!movement->StartPathMove(bot, dest, MoveReason::Travel))
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Move hop path start failed for {}", bot->GetName());
        return false;
    }

    // Record semantic completion target.
    uint32 nowMs = getMSTime();
    uint32 timeoutMs = static_cast<uint32>(std::clamp(command.distance * 1800.0f, 30000.0f, 180000.0f));
    std::string key = BuildActionKey(command);
    if (key.empty())
        key = "move_hop:api";
    else
        key = "api:" + key;
    AmigoTravelTarget targetSpec{key, dest, 2.5f, timeoutMs};
    travel->Begin(targetSpec, nowMs);

    if (g_EnableOllamaBotAmigoDebug)
    {
        LOG_INFO("server.loading",
                 "[OllamaBotAmigo] Move hop started for {} -> ({},{},{})",
                 bot->GetName(),
                 command.targetX,
                 command.targetY,
                 command.targetZ);
    }

    return true;
}
case BotControlCommandType::PlayerbotCommand:

        {
            // Forward raw command to Playerbot AI (with special-case attack pull).
            if (command.args.empty())
            {
                return false;
            }

            if (command.args[0] == "co +pull")
            {
                PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
                if (!ai)
                {
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Attack pull rejected (reason=no_ai) for {}", bot->GetName());
                    return false;
                }

                Unit* selected = bot->GetSelectedUnit();
                if (!selected || !selected->IsAlive() || !selected->IsHostileTo(bot))
                {
                    Creature* target = FindNearestHostileCreature(bot, ai);
                    if (!target)
                    {
                        LOG_INFO("server.loading", "[OllamaBotAmigo] Attack pull rejected (reason=no_target) for {}", bot->GetName());
                        return false;
                    }

                    bot->SetSelection(target->GetGUID());
                    LOG_INFO("server.loading",
                             "[OllamaBotAmigo] Attack pull auto-selected target {} (entry={}) for {}",
                             target->GetName(),
                             target->GetEntry(),
                             bot->GetName());
                }
            }

            return InjectPlayerbotCommand(bot, command.args[0], "playerbot_command");
        }
        case BotControlCommandType::Idle:
        {
            ASSERT(command.args.empty());
            ASSERT(command.distance == 0.0f);
            return false;
        }
        default:
            break;
    }

    return false;
}

bool HandleBotControlCommandTracked(Player* bot, const BotControlCommand& command)
{
    // Wrap command execution with stuck-memory bookkeeping.
    if (!bot)
    {
        return false;
    }

    std::string actionKey = BuildActionKey(command);
    bool ok = HandleBotControlCommand(bot, command);

    if (!actionKey.empty())
    {
        if (ok)
        {
            ClearStuckAttempt(bot->GetGUID().GetRawValue(), actionKey);
        }
        else
        {
            RecordStuckAttempt(bot->GetGUID().GetRawValue(), actionKey);
        }
    }

    if (ok && command.type == BotControlCommandType::PlayerbotCommand && !command.args.empty())
    {
        if (command.args[0].rfind("talk", 0) == 0)
        {
            RememberVendorFromSelectedTarget(bot);
        }
    }

    return ok;
}

bool ParseBotControlCommand(Player* bot, const std::string& commandStr)
{
    // Treat any non-empty string as a Playerbot command.
    if (!bot)
    {
        return false;
    }

    std::istringstream iss(commandStr);
    std::string cmd;
    iss >> cmd;

    if (!commandStr.empty())
    {
        BotControlCommand command;
        command.type = BotControlCommandType::PlayerbotCommand;
        command.args = { commandStr };
        return HandleBotControlCommand(bot, command);
    }

    return false;
}

std::string FormatCommandString(const BotControlCommand& command)
{
    // Friendly formatter for debug logs.
    std::ostringstream ss;
    switch (command.type)
    {
        case BotControlCommandType::MoveHop:
            ss << "move_hop " << command.targetX << ";" << command.targetY << ";" << command.targetZ;
            break;
        case BotControlCommandType::PlayerbotCommand:
            ss << "playerbot_command";
            for (const auto& arg : command.args)
            {
                ss << " " << arg;
            }
            break;
        case BotControlCommandType::Idle:
            ss << "idle";
            break;
        default:
            ss << "unknown";
            break;
    }
    return ss.str();
}

bool TryGetActivityState(Player* bot, std::string& activity, std::string& reason)
{
    // Lookup the last recorded activity for this bot.
    uint64 guid = GetBotGuid(bot);
    if (!guid)
    {
        return false;
    }

    auto it = activityStates.find(guid);
    if (it == activityStates.end())
    {
        return false;
    }

    activity = it->second.activity;
    reason = it->second.reason;
    return true;
}

void UpdateActivityState(Player* bot, std::string const& activity, std::string const& reason)
{
    // Update per-bot activity used by the planner and control layers.
    uint64 guid = GetBotGuid(bot);
    if (!guid)
    {
        return;
    }

    activityStates[guid] = ActivityState { activity, reason };
}

bool EnqueueBotControlCommand(Player* bot,
    const BotControlCommand& command,
    std::string const& reasoning)
{
    // Helper used by other scripts to enqueue bot control commands.
    if (!bot)
    {
        return false;
    }

    AmigoPlannerState plan;
    plan.command = command;
    plan.reasoning = reasoning;

    AmigoPlannerRegistry::Instance().Enqueue(bot, plan);
    return true;
}
