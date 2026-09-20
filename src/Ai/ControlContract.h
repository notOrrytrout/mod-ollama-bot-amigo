#pragma once

#include "Ai/ControlAction.h"
#include <limits>
#include <nlohmann/json.hpp>

inline ControlActionDefinition const* FindControlAction(std::string const& name)
{
    for (auto const& definition : GetControlActionCatalog())
        if (definition.supported && name == definition.name)
            return &definition;
    return nullptr;
}

inline bool ValidateControlArguments(ControlActionDefinition const& definition,
                                     nlohmann::json const& arguments, std::string& reason)
{
    reason = "invalid_arguments";
    if (!arguments.is_object())
        return false;
    std::vector<std::string> required;
    if (definition.requiresQuestId) required.emplace_back("quest_id");
    if (definition.requiresEntryId) required.emplace_back("entry_id");
    if (definition.requiresNavEpoch) required.emplace_back("nav_epoch");
    if (definition.requiresCandidateId) required.emplace_back("candidate_id");
    if (definition.requiresSkill) required.emplace_back("skill");
    if (definition.requiresIntent) required.emplace_back("intent");
    if (arguments.size() != required.size())
        return false;
    for (auto const& key : required)
    {
        if (!arguments.contains(key))
            return false;
        auto const& value = arguments.at(key);
        if (key == "entry_id" || key == "quest_id" || key == "nav_epoch")
        {
            if (!value.is_number_integer() || value.get<int64_t>() < 0 ||
                value.get<uint64_t>() > std::numeric_limits<uint32_t>::max() ||
                (key != "nav_epoch" && value.get<uint64_t>() == 0))
                return false;
        }
        else if (!value.is_string() || value.get<std::string>().empty())
            return false;
    }
    if (definition.requiresSkill && (arguments.at("skill") != "fishing" || arguments.at("intent") != "fish"))
    {
        reason = "unsupported_profession_intent";
        return false;
    }
    reason.clear();
    return true;
}
