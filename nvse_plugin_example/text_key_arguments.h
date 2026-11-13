#pragma once

#include <optional>
#include <string_view>

class NiControllerSequence;

// Text keys of kNVSE's features that take arguments ("aimPitch", "spawn", "despawn", "weaponSpace", "blendCurve",
// "blendInCurve", "blendOutCurve") share one form:
//   "<name>"                     no arguments
//   "<name>: <arg>, <arg>, ..."  the colon may be left out ("<name> <arg>, ..."), spaces and tabs around it are ignored
// The name is matched in any case and must be followed by the end, a colon, a space or a tab, so "aimPitchFoo" is no
// aimPitch key. Arguments are separated by commas; what an argument holds is the feature's.
namespace TextKeys
{
    // The arguments of the key if it is named name, trimmed (empty without any); nullopt for any other key. A view into
    // text
    std::optional<std::string_view> GetArguments(std::string_view text, std::string_view name);

    // The arguments of the sequence's first key named name (GetArguments). Views into its text key: valid while the
    // sequence is
    std::optional<std::string_view> FindArguments(const NiControllerSequence& sequence, std::string_view name);

    // Reads comma separated arguments: Next() returns each one trimmed, empty ones included ("a,,b": "a", "", "b"; "": ""),
    // then nullopt
    class ArgumentReader
    {
    public:
        explicit ArgumentReader(std::string_view arguments) : m_rest(arguments) {}
        std::optional<std::string_view> Next();

    private:
        std::string_view m_rest;
        bool m_done = false;
    };
}
