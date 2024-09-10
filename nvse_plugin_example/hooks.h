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
    bool fixMissingPrnKey = false;
    bool fixReloadStartAllowReloadTweak = false;
};
extern PluginINISettings g_pluginSettings;

struct PluginGlobalData
{
    bool isInLoopingReloadPlayAnim = false;
    bool isInConditionFunction = false;
};
extern PluginGlobalData g_globals;

struct AdditiveSequences
{
    std::unordered_map<const NiInterpolator*, BSAnimGroupSequence*> interpolatorToSequenceMap;
    std::unordered_set<const BSAnimGroupSequence*> additiveSequences;
    std::unordered_map<const BSAnimGroupSequence*, BSAnimGroupSequence*> animToAdditiveSequenceMap;
    std::unordered_set<const BSAnimGroupSequence*> playingAdditiveSequences;

    BSAnimGroupSequence* GetSequenceByInterpolator(const NiInterpolator* interpolator)
    {
        if (const auto iter = interpolatorToSequenceMap.find(interpolator); iter != interpolatorToSequenceMap.end())
            return iter->second;
        return nullptr;
    }

    void AddAdditiveSequence(BSAnimGroupSequence* sequence)
    {
        additiveSequences.insert(sequence);
        for (const auto& block : sequence->GetControlledBlocks())
        {
            if (const auto* interpolator = block.m_spInterpolator)
            {
                interpolatorToSequenceMap[interpolator] = sequence;
            }
        }
    }

    void PlayAdditiveAnim(AnimData* animData, const BSAnimGroupSequence* anim, std::string_view additiveAnimPath)
    {
        auto* additiveAnim = FindOrLoadAnim(animData, additiveAnimPath.data());
        if (!additiveAnim)
            return;
        if (!additiveSequences.contains(additiveAnim))
            return;
        animToAdditiveSequenceMap[anim] = additiveAnim;
        playingAdditiveSequences.insert(additiveAnim);
        const auto* currentAnim = animData->animSequence[anim->animGroup->GetSequenceType()];
        if (additiveAnim->m_eState != NiControllerSequence::INACTIVE)
            additiveAnim->Deactivate(0.0f, false);
        const float easeInTime = GetDefaultBlendTime(additiveAnim, currentAnim);
        additiveAnim->Activate(0, true, additiveAnim->m_fSeqWeight, easeInTime, nullptr, false);
    }

    bool IsAdditiveSequence(const BSAnimGroupSequence* sequence) const
    {
        return additiveSequences.contains(sequence);
    }

    bool StopAdditiveSequenceFromParent(const BSAnimGroupSequence* parentSequence, float afEaseOutTime = INVALID_TIME)
    {
        if (const auto iter = animToAdditiveSequenceMap.find(parentSequence); iter != animToAdditiveSequenceMap.end())
        {
            auto* additiveSequence = iter->second;
            playingAdditiveSequences.erase(additiveSequence);

            const auto easeOutTime = afEaseOutTime == INVALID_TIME ? additiveSequence->GetEasingTime() : afEaseOutTime;
            additiveSequence->Deactivate(easeOutTime, false);
            return true;
        }
        return false;
    }

    static AdditiveSequences& Get()
    {
        extern AdditiveSequences g_additiveSequences;
        return g_additiveSequences;
    }
};


namespace LoopingReloadPauseFix
{
    extern std::unordered_set<std::string_view> g_reloadStartBlendFixes;
}

void WriteDelayedHooks();
