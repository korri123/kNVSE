#pragma once
#include <unordered_set>

#include "commands_animation.h"

extern bool g_startedAnimation;
extern BSAnimGroupSequence* g_lastLoopSequence;
void ApplyHooks();

BSAnimGroupSequence* GetAnimByFullGroupID(AnimData* animData, UInt16 groupId);
BSAnimGroupSequence* GetAnimByGroupID(AnimData* animData, AnimGroupID ani);
void ApplyDestFrame(NiControllerSequence* sequence, float destFrame);

struct PluginINISettings
{
    bool fixSpineBlendBug = false;
    bool fixAttackISTransition = false;
    bool fixBlendSamePriority = false;
    bool fixLoopingReloadStart = false;
    bool disableFirstPersonTurningAnims = false;
    bool fixEndKeyTimeShorterThanStopTime = false;
    bool fixWrongAKeyInRespectEndKeyAnim = false;
    bool fixWrongPrnKey = false;
    bool fixWrongAnimName = false;
};

extern PluginINISettings g_pluginSettings;

namespace LoopingReloadPauseFix
{
    extern std::unordered_set<std::string_view> g_reloadStartBlendFixes;
}

namespace PointerMapAnimData
{
    void Set(NiTPointerMap<AnimSequenceBase>* map, AnimData* animData);
}
