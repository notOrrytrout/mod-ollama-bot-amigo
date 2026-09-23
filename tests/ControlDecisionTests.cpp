#include "Ai/ControlDecision.h"
#include "Bot/BotNavigationPenalty.h"
#include "Bot/BotTaskProgress.h"

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
    BotTaskProgress route;
    BotTaskProgress travelDetour;
    travelDetour.Reset(0, 0); // No spline has been issued yet.
    travelDetour.Observe(60, 1000); // Native route starts.
    for (uint32_t now = 2000; now <= 20000; now += 1000)
    {
        travelDetour.Observe(61.0f - now / 1000.0f, now);
        Check(!travelDetour.Stalled(now), "native route progress must survive a long outward detour");
    }
    Check(travelDetour.Stalled(28000), "a stopped native route must still time out");
    Check(AmigoPointInterrupted(true, false, false, false, false, false), "moving replacement must interrupt immediately");
    Check(AmigoPointInterrupted(true, false, false, true, true, true), "replacement at old endpoint must still interrupt");
    Check(!AmigoPointInterrupted(true, false, true, true, true, true), "normal point completion must not interrupt");
    Check(!AmigoPointInterrupted(true, true, false, false, false, false), "same generator speed update must retain ownership");
    Check(AmigoOwnsPoint(true, 12, 12, true), "cancel must stop the issued point");
    Check(!AmigoOwnsPoint(true, 12, 13, true), "cancel must preserve a newer point");
    Check(!AmigoOwnsPoint(true, 12, 12, false), "cancel must preserve combat chase");
    Check(AmigoSearchTakeover(true, true, 179, true, false, true, true), "active search must permit takeover");
    Check(!AmigoSearchTakeover(true, true, 179, true, true, true, true), "combat must block takeover");
    Check(!AmigoSearchTakeover(true, true, 179, true, false, false, true), "stale mission must block takeover");
    Check(!AmigoSearchTakeover(true, true, 0, true, false, true, true), "unspecified quest must block takeover");
    route.Reset(100, 0);
    route.Observe(90, 7000);
    Check(!route.Stalled(8000), "route progress must allow a detour");
    route.Observe(95, 9000);
    route.Observe(90, 14000);
    Check(route.Stalled(15000), "oscillation must not renew the progress deadline");
    route.Retry(120, 15000);
    route.Observe(110, 16000);
    route.Retry(130, 24000);
    Check(route.Exhausted(), "progress must not replenish the recovery budget");
    Check(!route.Stalled(24001), "failed retries must still wait before another attempt");

    BotLootPending loot;
    loot.Observe(42, BotLootPhase::Approach, 20.0f, 100);
    Check(loot.Ready(100), "a new target must be ready");
    loot.Issued(100);
    loot.Observe(42, BotLootPhase::Approach, 20.0f, 101);
    Check(!loot.Ready(101), "unchanged loot must not repeat its action");
    loot.Issued(3100);
    loot.Issued(6100);
    Check(loot.Exhausted(9100), "failed actions must exhaust a bounded retry budget");
    loot.Observe(42, BotLootPhase::Open, 0.0f, 9100);
    Check(loot.Ready(9100) && !loot.Exhausted(9100), "range transition must allow opening");
    loot.Issued(9100);
    loot.Observe(43, BotLootPhase::Open, 0.0f, 9101);
    Check(loot.Ready(9101), "a changed target must not inherit the old retry deadline");

    BotLootPending detour;
    detour.Observe(99, BotLootPhase::Approach, 10, 0);
    detour.ObserveRoute(7, 0, 0);
    for (uint32_t now = 10000; now <= 60000; now += 10000)
    {
        // Direct distance increases while the native route advances.
        detour.Observe(99, BotLootPhase::Approach, 10 + now, now);
        detour.ObserveRoute(7, now, now);
        detour.Issued(now, true);
        Check(!detour.Exhausted(now), "successful progressing detour must not expire");
    }
    detour.ObserveRoute(8, 0, 89000);
    Check(detour.Exhausted(90000), "new spline without progress must not renew deadline");

    BotLootPending collection;
    Check(!collection.Collecting(0, true, 100), "unrelated spell must not acquire loot ownership");
    collection.BeginCollection(99, 100);
    Check(collection.Collecting(0, false, 101), "successful open must retain ownership before response");
    Check(collection.Collecting(99, false, 200), "open loot must retain collection ownership");
    Check(!collection.Collecting(0, false, 201), "release must end collection ownership");
    collection.BeginCollection(99, 300);
    Check(!collection.Collecting(0, false, 5300), "missing response must have a bounded wait");

    BotNavigationPenalty penalty;
    penalty.Penalize("destination:1:100:200:40", 1000, 90000);
    Check(penalty.IsPenalized("destination:1:100:200:40", 1001), "timed-out destination must be penalized");
    Check(penalty.Remaining("destination:1:100:200:40", 1001) == 89999, "destination penalty must retain its duration");
    Check(!penalty.IsPenalized("destination:1:100:100:40", 1001), "other destinations must remain available");
    Check(!penalty.IsPenalized("destination:1:100:200:40", 91000), "destination penalty must expire");

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
    state["decision_options"] = Json::array({
        Json{{"action", "request_move_hop_npc"}, {"priority", 2}, {"turn_in", true}, {"arguments", {{"entry_id", 123}}}},
        Json{{"action", "request_move_hop"}, {"priority", 2}, {"turn_in", true}, {"arguments", {{"candidate_id", "nav_24"}}}}});
    Check(SelectControlDecision(state).at("name") == "request_move_hop_npc", "live quest giver approach must precede search movement");
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
    state["nav"]["candidates"] = Json::array({
        Json{{"candidate_id", "nav_4"}, {"can_move", true}, {"reachable", true}, {"temporarily_blocked", false}}});
    state["bot"]["is_moving"] = false;
    auto fallback = SelectControlDecision(state);
    Check(fallback.at("name") == "request_move_hop", "auto mode must use a reachable navigation fallback");
    Check(fallback.at("arguments").at("nav_epoch") == 7, "navigation fallback must use the current epoch");
    Check(fallback.at("arguments").at("candidate_id") == "nav_4", "navigation fallback must use the server candidate");
    state["bot"]["is_moving"] = true;
    Check(SelectControlDecision(state).empty(), "moving bot must retain movement ownership");

    state["bot"]["is_moving"] = false;
    state["bot"]["active_quests"] = Json::array({{{"status", "incomplete"}}});
    state["decision_options"] = Json::array({
        Json{{"action", "request_move_hop"}, {"priority", 20},
             {"objective_type", "creature"}, {"objective_target_id", 1234},
             {"arguments", {{"candidate_id", "nav_9"}}}}});
    auto objectiveMove = SelectControlDecision(state);
    Check(objectiveMove.at("name") == "request_move_hop", "mock auto must move toward an incomplete creature objective");
    Check(objectiveMove.at("arguments").at("candidate_id") == "nav_9", "objective movement must use the server candidate");

    state["decision_options"] = Json::array({
        Json{{"action", "request_move_hop"}, {"priority", 4}, {"quest_id", 180},
             {"arguments", {{"candidate_id", "nav_6"}}}},
        Json{{"action", "request_attack_target"}, {"priority", 20}, {"objective_type", "creature"},
             {"arguments", {{"entry_id", 705}}}}});
    auto questGiverMove = SelectControlDecision(state);
    Check(questGiverMove.at("name") == "request_move_hop", "quest giver movement must precede quest objectives");
    Check(questGiverMove.at("arguments").at("candidate_id") == "nav_6", "quest giver movement must use its candidate");
    state["decision_options"] = Json::array();
    Check(SelectControlDecision(state).empty(), "active quest must not use generic mountain-prone navigation fallback");
    state["decision_options"] = Json::array({
        Json{{"action", "request_attack_target"}, {"priority", 20}, {"quest_id", 313},
             {"objective_type", "item"}, {"objective_target_id", 2671}, {"source_target", "quest loot source"},
             {"arguments", {{"entry_id", 1961}}}}});
    Check(SelectControlDecision(state).at("arguments").at("entry_id") == 1961,
        "item objective must attack the mapped creature entry, not the item entry");
    state["decision_options"][0]["action"] = "request_gather_target";
    Check(SelectControlDecision(state).at("name") == "request_gather_target",
        "item objective must permit a server-approved object collection");
    state["decision_options"][0]["action"] = "request_use_quest_object";
    state["decision_options"][0]["objective_type"] = "game_object";
    Check(SelectControlDecision(state).at("name") == "request_use_quest_object",
        "in-range quest object must be used");
    state["decision_options"] = Json::array({
        Json{{"action", "request_move_hop"}, {"priority", 20}, {"quest_id", 313},
             {"objective_type", "game_object"}, {"arguments", {{"candidate_id", "nav_11"}}}}});
    Check(SelectControlDecision(state).at("arguments").at("candidate_id") == "nav_11",
        "out-of-range quest object must use its approved approach candidate");
    state["bot"]["active_quests"][0]["status"] = "complete";
    Check(SelectControlDecision(state).empty(), "completed quest must suppress further objective actions");
    std::cout << "Control decision checks passed\n";
}
