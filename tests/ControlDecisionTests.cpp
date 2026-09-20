#include "Ai/ControlDecision.h"
#include "Bot/BotNavigationPenalty.h"

#include <cstdlib>
#include <iostream>

using Json = nlohmann::json;

void Check(bool condition, char const* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main()
{
    BotNavigationPenalty penalty;
    penalty.Penalize("nav_24", 1000, 90000);
    Check(penalty.IsPenalized("nav_24", 1001), "timed-out candidate must be penalized");
    Check(penalty.Remaining("nav_24", 1001) == 89999, "candidate penalty must retain its duration");
    Check(!penalty.IsPenalized("nav_25", 1001), "other candidates must remain available");
    Check(!penalty.IsPenalized("nav_24", 91000), "candidate penalty must expire");

    Json state = {{"bot", {{"active_quests", Json::array({{{"status", "complete"}}})}}},
                  {"nav", {{"nav_epoch", 7}}}, {"decision_options", Json::array()}};
    auto attack = Json{{"action", "request_attack_target"}, {"priority", 20},
                       {"arguments", {{"entry_id", 705}}}};
    auto move = Json{{"action", "request_move_hop"}, {"priority", 2}, {"turn_in", true},
                     {"arguments", {{"candidate_id", "nav_2"}}}};
    auto talk = Json{{"action", "request_talk_to_quest_giver"}, {"priority", 1}, {"turn_in", true},
                     {"arguments", {{"quest_id", 179}}}};
    state["decision_options"] = Json::array({attack});
    Check(SelectControlDecision(state).empty(), "completed quest must block attack");
    state["decision_options"].push_back(move);
    auto selection = SelectControlDecision(state);
    Check(selection.at("name") == "request_move_hop", "remote turn-in must select movement");
    Check(selection.at("arguments").at("nav_epoch") == 7, "movement must echo the current epoch");
    state["decision_options"].push_back(talk);
    Check(SelectControlDecision(state).at("name") == "request_talk_to_quest_giver", "local turn-in takes precedence");
    state["bot"]["in_combat"] = true;
    Check(SelectControlDecision(state).empty(), "combat must preserve its owner");
    state["bot"]["in_combat"] = false;
    state["bot"]["travel"] = {{"active", true}};
    Check(SelectControlDecision(state).empty(), "travel must preserve its owner");
    state["bot"]["travel"]["active"] = false;
    state["bot"]["active_quests"] = Json::array();
    state["bot"]["is_moving"] = true;
    state["decision_options"] = Json::array({attack, {{"action", "request_repair"}, {"priority", 10},
        {"lifecycle_travel", true}, {"arguments", Json::object()}}});
    Check(SelectControlDecision(state).at("name") == "request_repair", "service may take unrelated movement");
    state["bot"]["lifecycle_active"] = true;
    Check(SelectControlDecision(state).empty(), "active maintenance must preserve its owner");
    state["bot"]["lifecycle_active"] = false;
    state["decision_options"] = Json::array();
    Check(SelectControlDecision(state).empty(), "no legal action must be a no-op");
    std::cout << "Control decision checks passed\n";
}
