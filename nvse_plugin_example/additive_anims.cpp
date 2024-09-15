#include "additive_anims.h"

#include "hooks.h"
#include "SafeWrite.h"
#include "utility.h"

namespace AdditiveManager
{
    std::unordered_map<const NiInterpolator*, BSAnimGroupSequence*> interpolatorToSequenceMap;
    std::unordered_set<const BSAnimGroupSequence*> additiveSequences;
    std::unordered_map<const BSAnimGroupSequence*, BSAnimGroupSequence*> animToAdditiveSequenceMap;
    std::unordered_set<const BSAnimGroupSequence*> playingAdditiveSequences;
    std::unordered_map<const NiInterpolator*, NiQuatTransform> additiveInterpBaseTransforms;
    std::unordered_set<const NiControllerSequence*> hasAdditiveTransforms;
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
                    if (NiBlendInterpolator* blendInterpolator = baseBlock.m_pkBlendInterp)
                    {
                        additiveInterpBaseTransforms[additiveBlock->m_spInterpolator] = baseTransform;
                        blendInterpolator->SetHasAdditiveTransforms(true);
                    }
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

    bool IsAdditiveSequence(const BSAnimGroupSequence* sequence)
    {
        return additiveSequences.contains(sequence);
    }

    bool IsAdditiveInterpolator(const NiInterpolator* interpolator)
    {
        return additiveInterpBaseTransforms.contains(interpolator);
    }

    AdditiveTransformType GetAdditiveTransformType(const NiInterpolator* interpolator)
    {
        return AdditiveTransformType::ReferenceFrame;
    }

    bool StopAdditiveSequenceFromParent(const BSAnimGroupSequence* parentSequence, float afEaseOutTime)
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

    std::optional<NiQuatTransform> GetRefFrameTransform(const NiInterpolator* interpolator)
    {
        if (const auto iter = additiveInterpBaseTransforms.find(interpolator); iter != additiveInterpBaseTransforms.end())
            return iter->second;
        return std::nullopt;
    }

#if _DEBUG
    const char* GetTargetNodeName(const NiInterpolator* interpolator)
    {
        if (const auto iter = interpsToTargets.find(interpolator); iter != interpsToTargets.end())
            return iter->second;
        return nullptr;
    }
#endif
}

void ApplyAdditiveTransforms(
    NiBlendTransformInterpolator& interpolator,
    float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue)
{
    NiPoint3 kFinalTranslate = NiPoint3::ZERO;
    NiQuaternion kFinalRotate = NiQuaternion(1.0f, 0.0f, 0.0f, 0.0f); // Identity quaternion
    float fFinalScale = 0.0f;

    bool bTransChanged = false;
    bool bRotChanged = false;
    bool bScaleChanged = false;
    
    bool bFirstRotation = true;
    for (auto& kItem : interpolator.GetItems())
    {
        if (!AdditiveManager::IsAdditiveInterpolator(kItem.m_spInterpolator.data))
            continue;
        if (kItem.m_fNormalizedWeight != 0.0f)
        {
#ifdef _DEBUG
            // this interpolator should not be factored into the normalized weight since that is reserved for the weighted blend animations
            // if normalized weight is not 0, then this interpolator is part of the weighted blend
            DebugBreakIfDebuggerPresent();
#endif
            continue;
        }

        const float fUpdateTime = interpolator.GetManagerControlled() ? kItem.m_fUpdateTime : fTime;
        if (fUpdateTime == INVALID_TIME)
            continue;
        NiQuatTransform kInterpTransform;
        NiQuatTransform kRefTransform;
        const auto eType = AdditiveManager::GetAdditiveTransformType(kItem.m_spInterpolator.data);
        if (eType == AdditiveTransformType::ReferenceFrame)
        {
            const auto& kRefFrameTransformResult = AdditiveManager::GetRefFrameTransform(kItem.m_spInterpolator.data);
            if (!kRefFrameTransformResult)
                continue;
            kRefTransform = *kRefFrameTransformResult;
        }
        else if (eType == AdditiveTransformType::EachFrame)
        {
            kRefTransform = kValue;
        }
        else
        {
#ifdef _DEBUG
            DebugBreakIfDebuggerPresent();
#endif
            continue;
        }
        if (kItem.m_spInterpolator.data->Update(fUpdateTime, pkInterpTarget, kInterpTransform))
        {
            const float fWeight = kItem.m_fWeight * kItem.m_fEaseSpinner;
            if (kInterpTransform.IsTranslateValid() && kRefTransform.IsTranslateValid())
            {
                const auto& kBaseTranslate = kRefTransform.GetTranslate();
                const auto& kAddTranslate = kInterpTransform.GetTranslate();
                kFinalTranslate += (kAddTranslate - kBaseTranslate) * fWeight;
                bTransChanged = true;
            }
            if (kInterpTransform.IsRotateValid() && kRefTransform.IsRotateValid())
            {
                // nlerp
                auto kBaseRot = kRefTransform.GetRotate();
                const auto kAddRot = kInterpTransform.GetRotate();
                // Reverse the direction of the base rotation.
                kBaseRot.m_fW *= -1;
                const auto kCom = kBaseRot * kAddRot;
                const float fBaseW = kCom.m_fW < 0.0f ? -1.0f : 1.0f;
                NiQuaternion kWeighted (
                    kCom.m_fW * fWeight + (1.0f - fWeight) * fBaseW,
                    kCom.m_fX * fWeight,
                    kCom.m_fY * fWeight,
                    kCom.m_fZ * fWeight
                );
                kWeighted.Normalize();

                kFinalRotate = kFinalRotate * kWeighted;
                bRotChanged = true;
            }
            if (kInterpTransform.IsScaleValid() && kRefTransform.IsScaleValid())
            {
                fFinalScale += (kInterpTransform.GetScale() - kRefTransform.GetScale()) * fWeight;
                bScaleChanged = true;
            }
        }
    }
    
    if (bTransChanged)
    {
        kValue.m_kTranslate += kFinalTranslate;
    }
    if (bRotChanged)
    {
        kValue.m_kRotate = kValue.m_kRotate * kFinalRotate;
        kValue.m_kRotate.Normalize();
    }
    if (bScaleChanged)
    {
        kValue.m_fScale += fFinalScale;
    }
}

void AdditiveManager::WriteHooks()
{
    static UInt32 uiComputeNormalizedWeightsAddr;

    // NiBlendInterpolator::ComputeNormalizedWeights
    WriteRelCall(0xA41147, INLINE_HOOK(void, __fastcall, NiBlendInterpolator* pBlendInterpolator)
    {
        // make sure that no additive interpolator gets factored into the normalized weight
        if (pBlendInterpolator->GetHasAdditiveTransforms())
        {
            unsigned char ucIndex = 0;
            for (auto& item : pBlendInterpolator->GetItems())
            {
                if (item.m_spInterpolator != nullptr && item.m_cPriority != SCHAR_MIN && IsAdditiveInterpolator(item.m_spInterpolator))
                    pBlendInterpolator->SetPriority(SCHAR_MIN, ucIndex);
                ++ucIndex;
            }
        }
        ThisStdCall(uiComputeNormalizedWeightsAddr, pBlendInterpolator);
    }), &uiComputeNormalizedWeightsAddr);

    static UInt32 uiBlendValuesAddr;

    // NiBlendTransformInterpolator::BlendValues
    WriteRelCall(0xA41160, INLINE_HOOK(bool, __fastcall, NiBlendTransformInterpolator* pBlendInterpolator, void*, float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform* kValue)
    {
        const auto result = ThisStdCall<bool>(uiBlendValuesAddr, pBlendInterpolator, fTime, pkInterpTarget, kValue);
        if (!result)
            return false;
        if (pBlendInterpolator->GetHasAdditiveTransforms())
            ApplyAdditiveTransforms(*pBlendInterpolator, fTime, pkInterpTarget, *kValue);
        return true;
    }), &uiBlendValuesAddr);
}