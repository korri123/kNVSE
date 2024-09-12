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
    std::unordered_map<const NiInterpolator*, NiQuatTransform> additiveInterpBaseTransforms;
#if _DEBUG
    std::unordered_map<const NiInterpolator*, const char*> interpsToTargets;
#endif

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
        CreateBaseTransforms(anim, additiveAnim);
        if (additiveAnim->m_eState != NiControllerSequence::INACTIVE)
            additiveAnim->Deactivate(0.0f, false);
        const auto* currentAnim = animData->animSequence[anim->animGroup->GetSequenceType()];
        float easeInTime = 0.0f;
        if (!additiveAnim->m_spTextKeys->FindFirstByName("noBlend"))
            easeInTime = GetDefaultBlendTime(additiveAnim, currentAnim);
        additiveAnim->Activate(0, true, additiveAnim->m_fSeqWeight, easeInTime, nullptr, false);
    }

    bool IsAdditiveSequence(const BSAnimGroupSequence* sequence) const
    {
        return additiveSequences.contains(sequence);
    }

    bool IsAdditiveInterpolator(const NiInterpolator* interpolator) const
    {
        return additiveInterpBaseTransforms.contains(interpolator);
    }

    bool StopAdditiveSequenceFromParent(const BSAnimGroupSequence* parentSequence, float afEaseOutTime = INVALID_TIME)
    {
        if (const auto iter = animToAdditiveSequenceMap.find(parentSequence); iter != animToAdditiveSequenceMap.end())
        {
            auto* additiveSequence = iter->second;
            playingAdditiveSequences.erase(additiveSequence);

            float easeOutTime = 0.0f;
            if (!additiveSequence->m_spTextKeys->FindFirstByName("noBlend"))
                easeOutTime = afEaseOutTime == INVALID_TIME ? additiveSequence->GetEasingTime() : afEaseOutTime;
            additiveSequence->Deactivate(easeOutTime, false);
            return true;
        }
        return false;
    }

    std::optional<NiQuatTransform> GetBaseTransform(const NiInterpolator* interpolator) const
    {
        if (const auto iter = additiveInterpBaseTransforms.find(interpolator); iter != additiveInterpBaseTransforms.end())
            return iter->second;
        return std::nullopt;
    }

    static AdditiveSequences& Get()
    {
        extern AdditiveSequences g_additiveSequences;
        return g_additiveSequences;
    }

private:
    std::unordered_set<const NiControllerSequence*> hasAdditiveTransforms;

    void CreateBaseTransforms(const NiControllerSequence* baseSequence, const NiControllerSequence* additiveSequence)
    {
        if (hasAdditiveTransforms.contains(additiveSequence))
            return;
        const auto baseIdTags = baseSequence->GetIDTags();
        const auto baseBlocks = baseSequence->GetControlledBlocks();
        for (size_t i = 0; i < baseBlocks.size(); i++)
        {
            const auto& baseBlock = baseBlocks[i];
            NiInterpolator* interpolator = baseBlock.m_spInterpolator;
            const NiInterpController* interpController = baseBlock.m_spInterpCtlr;
            if (interpolator && interpController)
            {
                const auto& idTag = baseIdTags[i];
                NiAVObject* targetNode = interpController->GetTargetNode(idTag);
                if (!targetNode)
                {
#ifdef _DEBUG
                    if (IsDebuggerPresent())
                        DebugBreak();
                    ERROR_LOG("target is null for " + std::string(idTag.m_kAVObjectName.CStr()));
#endif
                    continue;
                }

                const float fOldTime = interpolator->m_fLastTime;
                NiQuatTransform baseTransform;
                const bool bSuccess = interpolator->Update(0.0f, targetNode, baseTransform);
                interpolator->m_fLastTime = fOldTime;
                if (const auto* additiveBlock = additiveSequence->GetControlledBlock(idTag.m_kAVObjectName);
                           bSuccess && additiveBlock && additiveBlock->m_spInterpolator)
                {
                    additiveInterpBaseTransforms[additiveBlock->m_spInterpolator] = baseTransform;
                }
            }
        }
#if _DEBUG
        const auto controlledBlocks = additiveSequence->GetControlledBlocks();
        for (unsigned int i = 0; i < controlledBlocks.size(); i++)
        {
            auto& idTag = additiveSequence->GetIDTags()[i];
            interpsToTargets[controlledBlocks[i].m_spInterpolator] = idTag.m_kAVObjectName.CStr();
        }
#endif
        hasAdditiveTransforms.insert(additiveSequence);
    }

#if _DEBUG
public:
    const char* GetTargetNodeName(const NiInterpolator* interpolator) const
    {
        if (const auto iter = interpsToTargets.find(interpolator); iter != interpsToTargets.end())
            return iter->second;
        return nullptr;
    }
#endif
};


namespace LoopingReloadPauseFix
{
    extern std::unordered_set<std::string_view> g_reloadStartBlendFixes;
}

void WriteDelayedHooks();
