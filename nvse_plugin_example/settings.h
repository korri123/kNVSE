#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The values of kNVSE.ini. Read where they are used (most every time a feature runs); declared, loaded, saved and
// described in one place: Settings (settings.cpp)
struct PluginINISettings
{
    bool blendSmoothing{};
    float blendSmoothness{};
    bool fixSpineBlendBug{};
    bool fixWeaponHandBlend{};
    bool keepLeftHandOnWeapon{};
#if _DEBUG
    bool weaponHandBlendDebugLog{};
#endif
    bool fixBlendFromPose{};
    bool fixWeaponParentSwitch{};
    bool easeWeaponHolster{};
    bool fixGaitSwitch{};
    bool fixMovementMorphKeys{};
    bool fixOverlappingBlends{};
    bool movementTransitions{};
    bool aimTransitions{};
    bool fixAttackISTransition{};
    bool fixBlendSamePriority{};
    bool fixEndKeyTimeShorterThanStopTime{};
    bool fixWrongAKeyInRespectEndKeyAnim{};
    bool fixWrongPrnKey{};
    bool fixWrongAnimName{};
    bool fixMissingPrnKey{};
    bool fixReloadStartAllowReloadTweak{};
    bool fixWeaponStance{};
    bool fixShortGaitBlend{};
    std::string legacyAnimTimePathsText;
    bool fixLoopingReloads{};
    bool fixModReloadAnims{};
    bool fixNPCLoopingReloads{};
    bool fixAccumRootBlend{};
    bool fixAimUpDownVariants{};
    bool fixIdleStrafing{};
    bool fixBlendAnimMultipliers{};

    // not in the INI
    bool fixDeactivateControllerManagers = true;
    // legacyAnimTimePathsText split at its commas
    std::vector<std::string> legacyAnimTimePaths;
};
extern PluginINISettings g_pluginSettings;

namespace Settings
{
    // When a changed value takes effect
    enum class Apply : unsigned char
    {
        Live,
        // read when an animation is loaded: those loaded before keep what they had
        NewAnimations,
        // switching it off works at once, on only after a restart (its hooks are written at load if it is on)
        RestartToEnable,
        Restart,
    };

    struct Setting
    {
        const char* section;
        const char* key;
        std::variant<bool*, int*, float*, std::string*> value;
        double defaultValue;
        const char* defaultText; // string settings
        // the range a value is clamped to, when min < max
        double min;
        double max;
        // what the menu's arrows change it by: 0 for one (whole numbers) or a twentieth of the range
        double step;
        const char* label;
        const char* category;
        Apply apply;
        // keys of bool settings it does nothing without
        std::array<const char*, 2> requirements;
        // one line for the menu
        const char* summary;
        // the INI comment
        const char* description;

        bool IsBool() const { return std::holds_alternative<bool*>(value); }
        bool IsString() const { return std::holds_alternative<std::string*>(value); }
    };

    std::span<const Setting> All();
    // By INI key, or by the key without its type letter and a leading "Fix" (blendSmoothing, overlappingBlends);
    // case-insensitive
    const Setting* Find(std::string_view name);

    // Reads kNVSE.ini, adds the keys it lacks (with their defaults and comments) and saves it. At load, before any hook
    void Load();
    // Writes the values changed since the last Load or Save to kNVSE.ini. Whether it succeeded
    bool Save();

    double GetNumber(const Setting& setting);
    std::string GetText(const Setting& setting);
    // Clamped to the setting's range
    void SetNumber(const Setting& setting, double number);
    // Parses the text by the setting's type. Whether it was a valid value
    bool SetText(const Setting& setting, std::string_view text);
    void ResetToDefault(const Setting& setting);
    bool IsDefault(const Setting& setting);

    // Whether every setting it requires is on
    bool IsRequirementMet(const Setting& setting);
    // Whether its current value takes effect only after a restart
    bool NeedsRestart(const Setting& setting);
}
