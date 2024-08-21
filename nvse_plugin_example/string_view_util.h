#pragma once
#include "utility.h"

namespace sv
{
    inline std::string_view get_file_extension(std::string_view path)
    {
        const auto pos = path.find_last_of('.');
        if (pos == std::string::npos)
            return "";
        return path.substr(pos);
    }

    inline std::string_view get_file_name(std::string_view path)
    {
        const auto pos = path.find_last_of("\\/");
        if (pos == std::string::npos)
            return "";
        return path.substr(pos + 1);
    }

    inline std::string_view get_file_stem(std::string_view path)
    {
        const auto pos = path.find_last_of("\\/");
        if (pos == std::string::npos)
            return "";
        const auto dotPos = path.find_last_of('.');
        if (dotPos == std::string::npos)
            return "";
        return path.substr(pos + 1, dotPos - pos - 1);
    }

    inline bool equals_ci(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size())
            return false;
        for (size_t i = 0; i < left.size(); ++i)
        {
            if (std::tolower(left[i]) != std::tolower(right[i]))
                return false;
        }
        return true;
    }

    inline bool contains_ci(std::string_view left, std::string_view right)
    {
        return FindStringCI(left, right);
    }
}
