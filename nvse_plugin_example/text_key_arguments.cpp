#include "text_key_arguments.h"

#include "NiNodes.h"
#include "string_view_util.h"

std::optional<std::string_view> TextKeys::GetArguments(std::string_view text, std::string_view name)
{
    if (!sv::starts_with_ci(text, name))
        return std::nullopt;
    text.remove_prefix(name.size());
    if (!text.empty() && text.front() != ':' && text.front() != ' ' && text.front() != '\t')
        return std::nullopt;
    text = sv::trim(text);
    if (!text.empty() && text.front() == ':')
        text = sv::trim(text.substr(1));
    return text;
}

std::optional<std::string_view> TextKeys::FindArguments(const NiControllerSequence& sequence, std::string_view name)
{
    if (!sequence.m_spTextKeys)
        return std::nullopt;
    for (const NiTextKey& key : sequence.m_spTextKeys->GetKeys())
        if (const auto arguments = GetArguments(key.m_kText.Str(), name))
            return arguments;
    return std::nullopt;
}

std::optional<std::string_view> TextKeys::ArgumentReader::Next()
{
    if (m_done)
        return std::nullopt;
    const auto comma = m_rest.find(',');
    const auto argument = sv::trim(m_rest.substr(0, comma));
    if (comma == std::string_view::npos)
        m_done = true;
    else
        m_rest.remove_prefix(comma + 1);
    return argument;
}
