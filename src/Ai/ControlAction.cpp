#include "Ai/ControlAction.h"

namespace
{
    using Capability = ControlAction::Capability;

    std::vector<ControlActionDefinition> const kCatalog = {
        {"request_idle", "request_idle()", Capability::Idle, false, false, false, false, false, false, false, false, false, false, false,
         "wait for the next update when explicitly requested", "the waiting state remains unchanged", "invalid explicit wait"},
        {"request_move_hop", "request_move_hop(nav_epoch, candidate_id)", Capability::MoveHop, false, false, false, false, false, false, false, true, true, false, false,
         "move to a server-provided navigation candidate", "the candidate reaches its arrival radius", "stale, unreachable, or timed out candidate"},
        {"request_move_hop_npc", "request_move_hop_npc(entry_id)", Capability::MoveHopNpc, false, false, false, true, false, false, false, false, false, false, true,
         "move to a nearby server-selected NPC", "the NPC is in interaction range", "NPC is invalid or unreachable"},
        {"request_enter_grind", "request_enter_grind()", Capability::EnterGrind, false, false, false, false, false, false, false, false, false, false, false,
         "start the mission grind activity", "Playerbots enters grind strategy", "mission scope or objective is invalid"},
        {"request_attack_target", "request_attack_target(entry_id)", Capability::EnterAttackPull, false, false, false, true, false, false, false, false, false, false, true,
         "attack the exact nearby creature target", "the target is acted on or objective progress changes", "target is not an exact live mission target"},
        {"request_gather_target", "request_gather_target(entry_id)", Capability::GatherTarget, false, false, false, true, false, false, false, false, false, false, true,
         "gather the exact nearby game object", "the object is added to Playerbots loot handling", "object is invalid or not gatherable"},
        {"request_use_quest_object", "request_use_quest_object(entry_id)", Capability::UseQuestObject, false, false, false, true, false, false, false, false, false, false, true,
         "use a nearby game object required by an active quest", "the quest objective count increases", "quest object is invalid or out of range"},
        {"request_stop_grind", "request_stop_grind()", Capability::StopGrind, false, false, false, false, false, false, false, false, false, false, false,
         "stop the grind activity", "grind mode is cleared", "bot is not grinding"},
        {"request_stay", "request_stay()", Capability::Stay, false, false, false, false, false, false, false, false, false, false, false,
         "stay in place", "the stay strategy is active", "stay state cannot be changed"},
        {"request_unstay", "request_unstay()", Capability::Unstay, false, false, false, false, false, false, false, false, false, false, false,
         "clear the stay state", "the stay strategy is cleared", "bot is not stopped"},
        {"request_talk_to_quest_giver", "request_talk_to_quest_giver(quest_id)", Capability::TalkToQuestGiver, false, false, true, false, false, false, false, false, false, false, true,
         "interact with a quest giver in range", "the quest is rewarded or accepted", "quest giver or quest is invalid"},
        {"request_hearthstone", "request_hearthstone()", Capability::Hearthstone, false, false, false, false, false, false, false, false, false, false, false,
         "disabled: no retained destination completion check", "unsupported", "unsupported_action", false},
        {"request_taxi", "request_taxi()", Capability::Taxi, false, false, false, false, false, false, false, false, false, false, true,
         "disabled: no retained taxi destination completion check", "unsupported", "unsupported_action", false},
        {"request_fish", "request_fish()", Capability::Fish, false, false, false, false, false, false, false, false, false, false, false,
         "fish from the current location", "the fishing cycle succeeds or times out", "fishing cannot start"},
        {"request_profession", "request_profession(skill, intent)", Capability::UseProfession, false, false, false, false, true, true, false, false, false, false, false,
         "perform a supported profession action; currently fishing only", "the supported profession cycle completes", "unsupported skill or intent"},
        {"request_turn_left_90", "request_turn_left_90()", Capability::TurnLeft90, false, false, false, false, false, false, false, false, false, false, false,
         "turn left by 90 degrees", "orientation changes", "turn action fails"},
        {"request_turn_right_90", "request_turn_right_90()", Capability::TurnRight90, false, false, false, false, false, false, false, false, false, false, false,
         "turn right by 90 degrees", "orientation changes", "turn action fails"},
        {"request_turn_around", "request_turn_around()", Capability::TurnAround, false, false, false, false, false, false, false, false, false, false, false,
         "turn around by 180 degrees", "orientation changes", "turn action fails"},
        {"request_repair", "request_repair()", Capability::Repair, false, false, false, false, false, false, false, false, false, true, false,
         "repair equipment through Playerbots; travel is lifecycle-owned", "durability reaches 100 percent", "service unavailable or timeout"},
        {"request_vendor_sell", "request_vendor_sell()", Capability::VendorSell, false, false, false, false, false, false, false, false, false, true, false,
         "sell gray items; travel is lifecycle-owned", "requested gray-item count reaches zero", "vendor unavailable or timeout"},
        {"request_vendor_buy_useful", "request_vendor_buy_useful()", Capability::VendorBuyUseful, false, false, false, false, false, false, false, false, false, true, false,
         "buy needed supplies; travel is lifecycle-owned", "food, drink, or ammunition count increases", "vendor unavailable or timeout"},
        {"request_trainer", "request_trainer()", Capability::Trainer, false, false, false, false, false, false, false, false, false, true, false,
         "train eligible spells; travel is lifecycle-owned", "an eligible spell is learned or no eligible spell remains", "trainer unavailable or timeout"}
    };
}

std::vector<ControlActionDefinition> const& GetControlActionCatalog()
{
    return kCatalog;
}

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

void ControlActionRegistry::Clear(uint64 botGuid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    actions_.erase(botGuid);
}
