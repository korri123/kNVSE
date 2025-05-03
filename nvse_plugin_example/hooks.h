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
    bool fixSpineBlendBug = false;
    bool fixAttackISTransition = false;
    bool fixBlendSamePriority = false;
    bool fixLoopingReloadStart = false;
    bool disableFirstPersonTurningAnims = false;
    bool fixEndKeyTimeShorterThanStopTime = false;
    bool fixWrongAKeyInRespectEndKeyAnim = false;
    bool fixWrongPrnKey = false;
    bool fixWrongAnimName = false;
    bool fixMissingPrnKey = false;
    bool fixReloadStartAllowReloadTweak = false;
    bool blendSmoothing = false;
    bool fixSpiderHands = true;
    bool poseInterpolators = false;
    float blendSmoothingRate = 0.075f;

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
