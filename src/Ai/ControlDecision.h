#pragma once

#include <nlohmann/json.hpp>
#include <string>

// Pure selector: all targets and routes come from the world-thread snapshot.
// An empty object is a successful no-op and preserves the execution owner.
inline nlohmann::json SelectControlDecision(nlohmann::json const& state)
{
    auto const& bot = state.at("bot");
    auto lifecycle = bot.value("lifecycle", nlohmann::json::object());
    std::string lane = lifecycle.value("lane", "mission");
    if (bot.value("in_combat", false) || lane == "combat" || lane == "loot" || lane == "needs" ||
        bot.value("lifecycle_active", false) ||
        bot.value("travel", nlohmann::json::object()).value("active", false) ||
        bot.value("profession", nlohmann::json::object()).value("active", false))
        return nlohmann::json::object();

    bool completed = false;
    for (auto const& quest : bot.value("active_quests", nlohmann::json::array()))
        completed = completed || quest.value("status", "") == "complete";

    nlohmann::json selected = nlohmann::json::object();
    int bestPriority = 1000;
    for (auto const& option : state.value("decision_options", nlohmann::json::array()))
    {
        std::string action = option.value("action", "");
        if (action.empty() || action == "request_idle")
            continue;
        bool turnIn = option.value("turn_in", false);
        if (completed && !turnIn)
            continue;
        bool service = option.value("lifecycle_travel", false);
        if (bot.value("is_moving", false) && !service && action != "request_stop_grind")
            continue;
        int priority = option.value("priority", 100);
        if (priority >= bestPriority)
            continue;
        auto arguments = option.value("arguments", nlohmann::json::object());
        if (action == "request_move_hop")
            arguments["nav_epoch"] = state.at("nav").at("nav_epoch");
        selected = {{"name", action}, {"arguments", arguments}};
        bestPriority = priority;
    }
    return selected;
}
