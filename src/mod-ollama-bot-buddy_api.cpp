#include "mod-ollama-bot-buddy_api.h"
#include "mod-ollama-bot-buddy_config.h"
#include "mod-ollama-bot-buddy_loop.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Log.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Cell.h"
#include "Map.h"
#include <sstream>

namespace BotBuddyAI
{
    bool MoveTo(Player* bot, float x, float y, float z)
    {
        if (!bot) return false;
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, x, y, z);
        return true;
    }

    bool Attack(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        Unit* target = ObjectAccessor::GetUnit(*bot, guid);
        if (!target || !bot->IsHostileTo(target) || !bot->IsWithinLOSInMap(target)) return false;

        bot->SetSelection(target->GetGUID());
        bot->SetFacingToObject(target);
        bot->Attack(target, true);
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MoveChase(target);

        return true;
    }

    bool Interact(Player* bot, ObjectGuid guid)
    {
        if (!bot || !guid) return false;

        if (Creature* creature = ObjectAccessor::GetCreature(*bot, guid))
        {
            bot->SetFacingToObject(creature);
            creature->AI()->sGossipHello(bot);
            return true;
        }
        else if (GameObject* go = ObjectAccessor::GetGameObject(*bot, guid))
        {
            bot->SetFacingToObject(go);
            go->Use(bot);
            return true;
        }
        return false;
    }

    bool CastSpell(Player* bot, uint32 spellId, Unit* target)
    {
        if (!bot) return false;
        if (!target) target = bot->GetVictim();
        if (!target) return false;

        bot->SetFacingToObject(target);
        bot->CastSpell(target, spellId, false);
        return true;
    }

    bool Say(Player* bot, const std::string& msg)
    {
        if (!bot) return false;
        bot->Say(msg, LANG_UNIVERSAL);
        return true;
    }

    bool FollowMaster(Player* bot)
    {
        if (!bot) return false;
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
        if (!ai) return false;
        Player* master = ai->GetMaster();
        if (!master) return false;

        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MoveFollow(master, 1.0f, 0.0f);
        return true;
    }

    bool StopMoving(Player* bot)
    {
        if (!bot) return false;
        bot->GetMotionMaster()->Clear();
        return true;
    }

    bool AcceptQuest(Player* bot, uint32 questId)
    {
        if (!bot) return false;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) return false;
        bot->AddQuest(quest, nullptr);
        return true;
    }

    bool TurnInQuest(Player* bot, uint32 questId)
    {
        if (!bot) return false;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest) return false;
        bot->CompleteQuest(questId);
        bot->RewardQuest(quest, 0, bot);
        return true;
    }

    bool LootNearby(Player* bot)
    {
        if (!bot) return false;

        Map* map = bot->GetMap();
        if (!map) return false;

        for (const auto& pair : map->GetCreatureBySpawnIdStore())
        {
            Creature* creature = pair.second;
            if (!creature || !creature->isDead()) continue;

            if (!bot->IsWithinDistInMap(creature, INTERACTION_DISTANCE)) continue;

            bot->SetFacingToObject(creature);
            bot->PrepareGossipMenu(creature, creature->GetCreatureTemplate()->GossipMenuId);
            bot->SendLoot(creature->GetGUID(), LOOT_CORPSE);
            return true;
        }

        return false;
    }

} // namespace BotBuddyAI

bool HandleBotControlCommand(Player* bot, const BotControlCommand& command)
{
    if (g_EnableOllamaBotBuddyDebug && bot)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] HandleBotControlCommand for '{}', type {}", bot->GetName(), int(command.type));
        LOG_INFO("server.loading", "[OllamaBotBuddy] ================================================================================================");
    }
    if (!bot) return false;
    switch (command.type)
    {
        case BotControlCommandType::MoveTo:
            if (command.args.size() >= 3)
            {
                float x = std::stof(command.args[0]);
                float y = std::stof(command.args[1]);
                float z = std::stof(command.args[2]);
                return BotBuddyAI::MoveTo(bot, x, y, z);
            }
            break;
        case BotControlCommandType::Attack:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
        try {
            lowGuid = std::stoul(command.args[0]);
        } catch (const std::invalid_argument& e) {
            LOG_ERROR("OllamaBotBuddy", "Invalid argument for lowGuid '{}'", command.args[0]);
            return false;
        } catch (const std::out_of_range& e) {
            LOG_ERROR("OllamaBotBuddy", "Out of range value for lowGuid '{}'", command.args[0]);
            return false;
        }

                // Try to find the Creature by LowGuid first
                Creature* creatureTarget = nullptr;
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (!c) continue;
                    if (c->GetGUID().GetCounter() == lowGuid)
                    {
                        creatureTarget = c;
                        break;
                    }
                }

                if (creatureTarget)
                {
                    // Use the actual GUID from the creature, never reconstruct!
                    return BotBuddyAI::Attack(bot, creatureTarget->GetGUID());
                }

                // If not found, try Player
                ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                if (playerTarget)
                {
                    return BotBuddyAI::Attack(bot, playerTarget->GetGUID());
                }

                LOG_INFO("server.loading", "[OllamaBotBuddy] Could not find target with lowGuid {}", lowGuid);
                return false;
            }
            break;
        case BotControlCommandType::Interact:
            if (!command.args.empty())
            {
                uint32 lowGuid = 0;
        try {
            lowGuid = std::stoul(command.args[0]);
        } catch (const std::invalid_argument& e) {
            LOG_ERROR("OllamaBotBuddy", "Invalid argument for lowGuid '{}'", command.args[0]);
            return false;
        } catch (const std::out_of_range& e) {
            LOG_ERROR("OllamaBotBuddy", "Out of range value for lowGuid '{}'", command.args[0]);
            return false;
        }
                Creature* creatureTarget = nullptr;
                GameObject* goTarget = nullptr;

                // Find creature by LowGuid
                for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                {
                    Creature* c = pair.second;
                    if (!c) continue;
                    if (c->GetGUID().GetCounter() == lowGuid)
                    {
                        creatureTarget = c;
                        break;
                    }
                }

                if (creatureTarget)
                {
                    return BotBuddyAI::Interact(bot, creatureTarget->GetGUID());
                }

                // Find gameobject by LowGuid
                for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
                {
                    GameObject* go = pair.second;
                    if (!go) continue;
                    if (go->GetGUID().GetCounter() == lowGuid)
                    {
                        goTarget = go;
                        break;
                    }
                }

                if (goTarget)
                {
                    return BotBuddyAI::Interact(bot, goTarget->GetGUID());
                }

                LOG_INFO("server.loading", "[OllamaBotBuddy] Could not find interact target with lowGuid {}", lowGuid);
                return false;
            }
            break;
        case BotControlCommandType::CastSpell:
            if (!command.args.empty())
            {
                uint32 spellId = 0;
        try {
            spellId = std::stoi(command.args[0]);
        } catch (const std::invalid_argument& e) {
            LOG_ERROR("OllamaBotBuddy", "Invalid argument for spellId '{}'", command.args[0]);
            return false;
        } catch (const std::out_of_range& e) {
            LOG_ERROR("OllamaBotBuddy", "Out of range value for spellId '{}'", command.args[0]);
            return false;
        }
                Unit* target = nullptr;
                if (command.args.size() > 1)
                {
                    uint32 lowGuid = 0;
        try {
            lowGuid = std::stoul(command.args[1]);
        } catch (const std::invalid_argument& e) {
            LOG_ERROR("OllamaBotBuddy", "Invalid argument for lowGuid '{}'", command.args[1]);
            return false;
        } catch (const std::out_of_range& e) {
            LOG_ERROR("OllamaBotBuddy", "Out of range value for lowGuid '{}'", command.args[1]);
            return false;
        }
                    // Try to find creature by lowGuid
                    for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
                    {
                        Creature* c = pair.second;
                        if (c && c->GetGUID().GetCounter() == lowGuid)
                        {
                            target = c;
                            break;
                        }
                    }
                    // Try to find player by lowGuid if not found
                    if (!target)
                    {
                        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
                        Player* playerTarget = ObjectAccessor::FindConnectedPlayer(guid);
                        if (playerTarget) target = playerTarget;
                    }
                }
                else
                {
                    target = bot; // Use bot itself as the target if no guid provided
                }
                return BotBuddyAI::CastSpell(bot, spellId, target);
            }
            break;
        case BotControlCommandType::Say:
            if (!command.args.empty())
            {
                return BotBuddyAI::Say(bot, command.args[0]);
            }
            break;
        case BotControlCommandType::Follow:
            return BotBuddyAI::FollowMaster(bot);
        case BotControlCommandType::Stop:
            return BotBuddyAI::StopMoving(bot);
        case BotControlCommandType::AcceptQuest:
            if (!command.args.empty())
            {
                uint32 questId = std::stoi(command.args[0]);
                return BotBuddyAI::AcceptQuest(bot, questId);
            }
            break;
        case BotControlCommandType::TurnInQuest:
            if (!command.args.empty())
            {
                uint32 questId = std::stoi(command.args[0]);
                return BotBuddyAI::TurnInQuest(bot, questId);
            }
            break;
        case BotControlCommandType::Loot:
            return BotBuddyAI::LootNearby(bot);
        default:
            break;
    }
    return false;
}


bool ParseBotControlCommand(Player* bot, const std::string& commandStr)
{
    if (g_EnableOllamaBotBuddyDebug && bot)
    {
        LOG_INFO("server.loading", "[OllamaBotBuddy] ParseBotControlCommand for '{}': {}", bot->GetName(), commandStr);
    }
    std::istringstream iss(commandStr);
    std::string cmd;
    iss >> cmd;
    if (cmd == "move")
    {
        std::string to;
        iss >> to;
        if (to != "to") return false;
        float x, y, z;
        iss >> x >> y >> z;
        BotControlCommand command = {BotControlCommandType::MoveTo, {std::to_string(x), std::to_string(y), std::to_string(z)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "attack")
    {
        std::string guid;
        iss >> guid;
        BotControlCommand command = {BotControlCommandType::Attack, {guid}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "interact")
    {
        std::string guid;
        iss >> guid;
        BotControlCommand command = {BotControlCommandType::Interact, {guid}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "say")
    {
        std::string msg;
        std::getline(iss, msg);
        BotControlCommand command = {BotControlCommandType::Say, {msg}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "loot")
    {
        BotControlCommand command = {BotControlCommandType::Loot, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "follow")
    {
        BotControlCommand command = {BotControlCommandType::Follow, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "stop")
    {
        BotControlCommand command = {BotControlCommandType::Stop, {}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "acceptquest")
    {
        uint32 questId;
        iss >> questId;
        BotControlCommand command = {BotControlCommandType::AcceptQuest, {std::to_string(questId)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "turninquest")
    {
        uint32 questId;
        iss >> questId;
        BotControlCommand command = {BotControlCommandType::TurnInQuest, {std::to_string(questId)}};
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    else if (cmd == "spell")
    {
        uint32 spellId;
        iss >> spellId;
        std::string targetGuid;
        iss >> targetGuid;
        BotControlCommand command;
        if (!targetGuid.empty())
        {
            command = {BotControlCommandType::CastSpell, {std::to_string(spellId), targetGuid}};
        }
        else
        {
            command = {BotControlCommandType::CastSpell, {std::to_string(spellId)}};
        }
        bool result = HandleBotControlCommand(bot, command);
        if (result)
        {
            AddBotCommandHistory(bot, FormatCommandString(command));
        }
        return result;
    }
    return false;
}

std::string FormatCommandString(const BotControlCommand& command)
{
    std::ostringstream ss;
    switch (command.type)
    {
        case BotControlCommandType::MoveTo:
            ss << "move to";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Attack:
            ss << "attack";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Interact:
            ss << "interact";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::CastSpell:
            ss << "cast";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Loot:
            ss << "loot";
            break;
        case BotControlCommandType::Follow:
            ss << "follow";
            break;
        case BotControlCommandType::Say:
            ss << "say";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::AcceptQuest:
            ss << "acceptquest";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::TurnInQuest:
            ss << "turninquest";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
        case BotControlCommandType::Stop:
            ss << "stop";
            break;
        default:
            ss << "unknown command";
            for (const auto& arg : command.args)
                ss << " " << arg;
            break;
    }
    return ss.str();
}


