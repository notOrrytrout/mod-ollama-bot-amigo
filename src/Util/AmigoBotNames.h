#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

inline std::vector<std::string> ParseAmigoBotNames(std::string_view configuredNames)
{
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= configuredNames.size())
    {
        std::size_t end = configuredNames.find(',', start);
        if (end == std::string_view::npos)
            end = configuredNames.size();

        std::string_view name = configuredNames.substr(start, end - start);
        std::size_t first = name.find_first_not_of(" \t\r\n");
        if (first != std::string_view::npos)
        {
            std::size_t last = name.find_last_not_of(" \t\r\n");
            name = name.substr(first, last - first + 1);
            names.emplace_back(name);
        }

        if (end == configuredNames.size())
            break;
        start = end + 1;
    }

    return names;
}

inline bool IsAmigoBotNameAllowed(std::string_view configuredNames, std::string_view botName)
{
    if (configuredNames.empty())
        return true;

    for (std::string const& name : ParseAmigoBotNames(configuredNames))
        if (name == botName)
            return true;

    return false;
}
