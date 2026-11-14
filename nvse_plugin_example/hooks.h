#pragma once
#include <unordered_set>

#include "commands_animation.h"
#include "GameRTTI.h"
#include "NiObjects.h"

extern bool g_startedAnimation;
extern BSAnimGroupSequence* g_lastLoopSequence;
void ApplyHooks();

BSAnimGroupSequence* GetAnimByFullGroupID(AnimData* animData, UInt16 groupId);
BSAnimGroupSequence* GetAnimByGroupID(AnimData* animData, AnimGroupID ani);
void ApplyDestFrame(NiControllerSequence* sequence, float destFrame);

struct PluginINISettings
{
    bool fixSpineBlendBug = true;
    bool fixAttackISTransition = true;
    bool fixBlendSamePriority = false;
    bool fixLoopingReloadStart = false;
    bool disableFirstPersonTurningAnims = false;
    bool fixEndKeyTimeShorterThanStopTime = false;
    bool fixWrongAKeyInRespectEndKeyAnim = false;
    bool fixWrongPrnKey = false;
    bool fixWrongAnimName = false;
    bool fixMissingPrnKey = false;
    bool fixReloadStartAllowReloadTweak = false;
    bool fixReloadStance = true;
    bool blendSmoothing = true;
    float blendSmoothingRate = 0.075f;
    float blendEaseCurve = 0.7f;
    bool fixWeaponHandBlend = true;
    bool fixBlendFromPose = true;
    bool fixWeaponParentSwitch = true;
    bool fixGaitSwitch = true;
    bool fixMovementMorphKeys = true;
    bool fixOverlappingBlends = true;
    bool movementTransitions = true;
    bool aimTransitions = true;
    bool fixAccumRootBlend = true;
#if _DEBUG
    bool weaponHandBlendDebugLog = false;
#endif

    bool fixDeactivateControllerManagers = true;
    std::vector<std::string> legacyAnimTimePaths;
};
extern PluginINISettings g_pluginSettings;

struct PluginGlobalData
{
    bool isInLoopingReloadPlayAnim = false;
    bool isInConditionFunction = false;
    std::string_view thisAnimScriptPath;
};
extern thread_local PluginGlobalData g_globals;

namespace LoopingReloadPauseFix
{
    extern std::unordered_set<std::string_view> g_reloadStartBlendFixes;
}

void WriteDelayedHooks();

namespace AllowAttackKey
{
    // Whether the actor's attack would start now if attack were pressed: the checks of PlayerCharacter::Update and
    // Actor::FiresWeapon, with the allowAttack/interruptLoop text keys and Stewie's Tweaks bFireWhileAiming
    bool CanFireWeapon(Actor* actor);
}

inline float GetDefaultBlendTime()
{
    return GetIniFloat(0x11C56FC);
}

inline float GetDefaultAnimMult()
{
    return GetIniFloat(0x11C5724);
}
