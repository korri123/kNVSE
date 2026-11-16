#pragma once
#include <unordered_set>

#include "commands_animation.h"
#include "GameRTTI.h"
#include "NiObjects.h"
#include "settings.h"

extern bool g_startedAnimation;
extern BSAnimGroupSequence* g_lastLoopSequence;
void ApplyHooks();

BSAnimGroupSequence* GetAnimByFullGroupID(AnimData* animData, UInt16 groupId);
BSAnimGroupSequence* GetAnimByGroupID(AnimData* animData, AnimGroupID ani);
void ApplyDestFrame(NiControllerSequence* sequence, float destFrame);

struct PluginGlobalData
{
    bool isInLoopingReloadPlayAnim = false;
    bool isInConditionFunction = false;
    std::string_view thisAnimScriptPath;
};
extern thread_local PluginGlobalData g_globals;

namespace LoopingReloadPauseFix
{
    // Registers the name of a sequence that has a blendToReloadLoop text key.
    void Add(std::string_view sequenceName);
}

void WriteDelayedHooks();

namespace AllowAttackKey
{
    // Returns true if the actor's attack would start now if attack were pressed. Mirrors the checks in
    // PlayerCharacter::Update and Actor::FiresWeapon, including the allowAttack/interruptLoop text keys and Stewie's
    // Tweaks bFireWhileAiming.
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
