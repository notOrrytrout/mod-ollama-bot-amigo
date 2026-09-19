#include "Bot/BotControlApi.h"
#include "Ai/ControlAction.h"
#include "Script/OllamaBotConfig.h"
#include "Script/AmigoPlanner.h"
#include "Bot/BotMovement.h"
#include "Util/WorldChecks.h"
#include "Bot/BotTravel.h"
#include "Bot/BotMission.h"
#include "Bot/BotLifecycle.h"
#include "Db/BotMemory.h"
#include "Util/PlayerbotsCompat.h"
#include "Event.h"
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

        // Amigo's internal commands are self-issued. Never impersonate the
        // Playerbots master pointer: group membership or a temporary peer
        // directive must not become command authority.
        Player* sender = bot;
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

    std::string BuildActionKey(BotControlCommand const& command)
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
            case BotControlCommandType::PlayerbotAction:
            case BotControlCommandType::PlayerbotStrategy:
            {
                if (!command.args.empty())
                {
                    std::string action = command.args[0];
                    if (action.size() > 120)
                    {
                        action.resize(120);
                    }
                    char const* prefix = command.type == BotControlCommandType::PlayerbotAction ? "action:" :
                                         command.type == BotControlCommandType::PlayerbotStrategy ? "strategy:" : "command:";
                    return std::string(prefix) + action;
                }
                break;
            }
            default:
                break;
        }
        return "";
    }

    void RecordStuckAttempt(uint64 botGuid, std::string const& actionKey)
    {
        // Increment attempt counts for actions that fail to apply.
        if (!g_EnableAmigoStuckMemory || actionKey.empty())
            return;

        if (BotMemory* memory = BotMemoryRegistry::Get(botGuid))
            memory->RecordFailure(actionKey, FailureType::Retryable, getMSTime());
    }

    void ClearStuckAttempt(uint64 botGuid, std::string const& actionKey)
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

        if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
        {
            return "vendor";
        }
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))
        {
            return "trainer";
        }
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
        {
            return "repair";
        }

        return "";
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

bool ExecutePlayerbotAction(Player* bot, std::string const& action, std::string const& qualifier)
{
    if (!bot || action.empty())
    {
        return false;
    }

    PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
    if (!ai)
    {
        LOG_INFO("server.loading", "[OllamaBotAmigo] Direct Playerbots action rejected: no PlayerbotAI for {}", bot->GetName());
        return false;
    }

    if (action == "attack selected target")
    {
        Unit* target = bot->GetSelectedUnit();
        if (!target || !target->IsInWorld() || !target->IsAlive() || !bot->IsValidAttackTarget(target))
        {
            LOG_INFO("server.loading", "[OllamaBotAmigo] Selected-target attack rejected for {}: invalid target",
                     bot->GetName());
            return false;
        }

        if (bot->IsInCombat() && bot->GetVictim() == target)
        {
            return true;
        }

        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
        {
            return false;
        }

        context->GetValue<Unit*>("current target")->Set(target);
        context->GetValue<GuidVector>("prioritized targets")->Set({ target->GetGUID() });
        bool const ok = ai->DoSpecificAction("melee", Event("ollama_bot_amigo", qualifier, bot), true, qualifier);
        if (ok)
        {
            ai->ChangeEngine(BOT_STATE_COMBAT);
        }

        if (g_EnableOllamaBotAmigoDebug)
        {
            LOG_INFO("server.loading",
                     "[OllamaBotAmigo] Selected-target attack '{}' for {}",
                     ok ? "succeeded" : "failed",
                     bot->GetName());
        }
        return ok;
    }

    // Use the bot as event owner. Actions such as "pull my target" then read
    // the bot's selected target without requiring a human master.
    const bool ok = ai->DoSpecificAction(action, Event("ollama_bot_amigo", qualifier, bot), true, qualifier);
    if (g_EnableOllamaBotAmigoDebug)
    {
        LOG_INFO("server.loading",
                 "[OllamaBotAmigo] Direct Playerbots action '{}'{} for {}",
                 action,
                 ok ? " succeeded" : " failed",
                 bot->GetName());
    }
    return ok;
}

bool ResolveCapabilityCommand(ControlAction::Capability capability,
    BotControlCommand& outCommand,
    std::string& outCommandText)
{
    // Map high-level capabilities to Playerbot commands.
    outCommand = BotControlCommand{};
    outCommandText.clear();

    switch (capability)
    {
        case ControlAction::Capability::EnterGrind:
            // Native equivalent of Playerbots' GrindChatShortcutAction.
            outCommand.type = BotControlCommandType::PlayerbotStrategy;
            outCommand.args = { "nc:+grind,-passive,-stay" };
            outCommandText = "strategy nc:+grind,-passive,-stay";
            return true;
        case ControlAction::Capability::StopGrind:
            // Native equivalent of Playerbots' FollowChatShortcutAction strategy changes.
            outCommand.type = BotControlCommandType::PlayerbotStrategy;
            outCommand.args = {
                "nc:+follow,-passive,-grind,-move from group",
                "co:-stay,-follow,-passive,-grind,-move from group"
            };
            outCommandText = "strategy follow/stop-grind";
            return true;
        case ControlAction::Capability::Stay:
            // Native equivalent of Playerbots' StayChatShortcutAction strategy changes.
            outCommand.type = BotControlCommandType::PlayerbotStrategy;
            outCommand.args = {
                "nc:+stay,-passive,-move from group",
                "co:+stay,-follow,-passive,-move from group"
            };
            outCommandText = "strategy stay";
            return true;
        case ControlAction::Capability::Unstay:
            outCommand.type = BotControlCommandType::PlayerbotStrategy;
            outCommand.args = { "nc:-stay", "co:-stay" };
            outCommandText = "strategy -stay";
            return true;
        case ControlAction::Capability::VendorSell:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "sell", "vendor" };
            outCommandText = "action sell(vendor)";
            return true;
        case ControlAction::Capability::VendorBuyUseful:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "buy", "vendor" };
            outCommandText = "action buy(vendor)";
            return true;
        case ControlAction::Capability::Repair:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "repair" };
            outCommandText = "action repair";
            return true;
        case ControlAction::Capability::Trainer:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "trainer" };
            outCommandText = "action trainer";
            return true;
        case ControlAction::Capability::Hearthstone:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "hearthstone" };
            outCommandText = "action hearthstone";
            return true;
        case ControlAction::Capability::Taxi:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "taxi" };
            outCommandText = "action taxi";
            return true;
        case ControlAction::Capability::Loot:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "loot" };
            outCommandText = "action loot";
            return true;
        case ControlAction::Capability::Food:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "food" };
            outCommandText = "action food";
            return true;
        case ControlAction::Capability::Drink:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "drink" };
            outCommandText = "action drink";
            return true;
        case ControlAction::Capability::Maintenance:
            outCommand.type = BotControlCommandType::PlayerbotAction;
            outCommand.args = { "maintenance" };
            outCommandText = "action maintenance";
            return true;
        case ControlAction::Capability::TalkToQuestGiver:
            // Playerbots' chat "talk" trigger fans out into gossip and quest packet actions.
            // Keep this as a command fallback until we can provide those packet events directly.
            outCommand.type = BotControlCommandType::PlayerbotCommand;
            outCommandText = "talk";
            outCommand.args = { outCommandText };
            return true;
        case ControlAction::Capability::TurnLeft90:
            // No registered direct Playerbots action exists for these chat-only turn helpers.
            outCommand.type = BotControlCommandType::PlayerbotCommand;
            outCommandText = "turnleft";
            outCommand.args = { outCommandText };
            return true;
        case ControlAction::Capability::TurnRight90:
            outCommand.type = BotControlCommandType::PlayerbotCommand;
            outCommandText = "turnright";
            outCommand.args = { outCommandText };
            return true;
        case ControlAction::Capability::TurnAround:
            outCommand.type = BotControlCommandType::PlayerbotCommand;
            outCommandText = "turnaround";
            outCommand.args = { outCommandText };
            return true;
        case ControlAction::Capability::Idle:
        case ControlAction::Capability::MoveHop:
        case ControlAction::Capability::MoveHopNpc:
        default:
            return false;
    }
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

bool HandleBotControlCommand(Player* bot, BotControlCommand const& command)
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
case BotControlCommandType::PlayerbotAction:
        {
            if (command.args.empty())
            {
                return false;
            }
            const std::string qualifier = command.args.size() > 1 ? command.args[1] : std::string();
            return ExecutePlayerbotAction(bot, command.args[0], qualifier);
        }
case BotControlCommandType::PlayerbotStrategy:
        {
            if (command.args.empty())
            {
                return false;
            }

            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            if (!ai)
            {
                LOG_INFO("server.loading", "[OllamaBotAmigo] Strategy change rejected: no PlayerbotAI for {}", bot->GetName());
                return false;
            }

            bool changed = false;
            for (std::string const& spec : command.args)
            {
                if (spec.size() < 4 || spec[2] != ':')
                {
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Invalid native strategy spec '{}' for {}", spec, bot->GetName());
                    return false;
                }

                BotState state;
                if (spec.rfind("nc:", 0) == 0) state = BOT_STATE_NON_COMBAT;
                else if (spec.rfind("co:", 0) == 0) state = BOT_STATE_COMBAT;
                else if (spec.rfind("de:", 0) == 0) state = BOT_STATE_DEAD;
                else
                {
                    LOG_INFO("server.loading", "[OllamaBotAmigo] Unknown native strategy state in '{}' for {}", spec, bot->GetName());
                    return false;
                }

                std::string const change = spec.substr(3);
                std::vector<std::string> before = ai->GetStrategies(state);
                ai->ChangeStrategy(change, state);
                std::vector<std::string> after = ai->GetStrategies(state);
                changed = changed || before != after;

                if (g_EnableOllamaBotAmigoDebug)
                {
                    LOG_INFO("server.loading",
                             "[OllamaBotAmigo] Native strategy change for {}: state={} change='{}' before=[{}] after=[{}]",
                             bot->GetName(), static_cast<int>(state), change, JoinStrategyNames(before), JoinStrategyNames(after));
                }
            }
            return changed || !command.args.empty();
        }
case BotControlCommandType::PlayerbotCommand:

        {
            // Forward the remaining chat-only command to Playerbot AI.
            if (command.args.empty())
            {
                return false;
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

bool HandleBotControlCommandTracked(Player* bot, BotControlCommand const& command)
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

    if (ok && command.type == BotControlCommandType::PlayerbotAction && !command.args.empty())
    {
        std::string const& action = command.args[0];
        if (action == "sell" || action == "buy")
            UpdateActivityState(bot, "vendor", action == "sell" ? "native Playerbots sell" : "native Playerbots buy useful");
        else if (action == "repair")
            UpdateActivityState(bot, "repair", "native Playerbots repair");
        else if (action == "trainer")
            UpdateActivityState(bot, "trainer", "native Playerbots trainer");
        else if (action == "hearthstone")
            UpdateActivityState(bot, "travel", "native Playerbots hearthstone");
        else if (action == "taxi")
            UpdateActivityState(bot, "travel", "native Playerbots taxi");
        else if (action == "loot")
            UpdateActivityState(bot, "loot", "native Playerbots loot");
        else if (action == "food" || action == "drink")
            UpdateActivityState(bot, "recover", std::string("native Playerbots ") + action);
        else if (action == "maintenance")
            UpdateActivityState(bot, "maintenance", "native Playerbots maintenance");
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

bool ParseBotControlCommand(Player* bot, std::string const& commandStr)
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

std::string FormatCommandString(BotControlCommand const& command)
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
            for (auto const& arg : command.args) ss << " " << arg;
            break;
        case BotControlCommandType::PlayerbotAction:
            ss << "playerbot_action";
            for (auto const& arg : command.args) ss << " " << arg;
            break;
        case BotControlCommandType::PlayerbotStrategy:
            ss << "playerbot_strategy";
            for (auto const& arg : command.args) ss << " " << arg;
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
    BotControlCommand const& command,
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
    plan.missionRevision = BotMissionRegistry::Instance().Get(bot->GetGUID().GetRawValue()).revision;
    plan.lifecycleGeneration = GetBotLifecycleGeneration(bot);

    AmigoPlannerRegistry::Instance().Enqueue(bot, plan);
    return true;
}
