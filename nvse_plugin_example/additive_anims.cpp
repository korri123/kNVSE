#include "additive_anims.h"

#include "hooks.h"
#include "SafeWrite.h"
#include "utility.h"


std::unordered_map<NiInterpolator*, NiQuatTransform> additiveInterpReferenceTransformsMap;
std::unordered_map<NiControllerSequence*, AdditiveAnimMetadata> additiveSequenceMap;
std::unordered_map<BSAnimGroupSequence*, BSAnimGroupSequence*> referenceToAdditiveMap;
#if _DEBUG
std::unordered_map<NiInterpolator*, const char*> interpsToTargetsMap;
#endif

void AdditiveManager::SetAdditiveReferencePose(BSAnimGroupSequence* referenceSequence,
                                               BSAnimGroupSequence* additiveSequence, float timePoint)
{
    if (const auto iter = additiveSequenceMap.find(additiveSequence); iter != additiveSequenceMap.end())
    {
        const auto& metadata = iter->second;
        if (metadata.referencePoseSequence == referenceSequence && metadata.referenceTimePoint == timePoint)
            return;
        additiveSequenceMap.erase(iter);
    }
    const auto baseIdTags = referenceSequence->GetIDTags();
    const auto baseBlocks = referenceSequence->GetControlledBlocks();
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
            const bool bSuccess = interpolator->Update(timePoint, targetNode, baseTransform);
            interpolator->m_fLastTime = fOldTime;
            if (const auto* additiveBlock = additiveSequence->GetControlledBlock(idTag.m_kAVObjectName);
                bSuccess && additiveBlock && additiveBlock->m_spInterpolator)
            {
                if (NiBlendInterpolator* blendInterpolator = baseBlock.m_pkBlendInterp)
                {
                    additiveInterpReferenceTransformsMap[additiveBlock->m_spInterpolator] = baseTransform;
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
        interpsToTargetsMap[controlledBlocks[i].m_spInterpolator] = idTag.m_kAVObjectName.CStr();
    }
#endif
    additiveSequenceMap[additiveSequence] = AdditiveAnimMetadata{referenceSequence, timePoint};
}

void AdditiveManager::PlayManagedAdditiveAnim(AnimData* animData, BSAnimGroupSequence* referenceAnim, BSAnimGroupSequence* additiveAnim)
{
    if (additiveAnim->m_eState != NiControllerSequence::INACTIVE)
    {
        if (referenceAnim->animGroup->GetBaseGroupID() == kAnimGroup_AttackLoop || referenceAnim->animGroup->GetBaseGroupID() ==
            kAnimGroup_AttackLoopIS)
            return;
        additiveAnim->Deactivate(0.0f, false);
    }
    SetAdditiveReferencePose(referenceAnim, additiveAnim);
    referenceToAdditiveMap[referenceAnim] = additiveAnim;

    BSAnimGroupSequence* currentAnim = nullptr;
    if (referenceAnim->animGroup)
        currentAnim = animData->animSequence[referenceAnim->animGroup->GetSequenceType()];
    float easeInTime = 0.0f;
    if (!additiveAnim->m_spTextKeys->FindFirstByName("noBlend"))
        easeInTime = GetDefaultBlendTime(additiveAnim, currentAnim);
    additiveAnim->Activate(0, true, additiveAnim->m_fSeqWeight, easeInTime, nullptr, false);
}

bool AdditiveManager::IsAdditiveSequence(BSAnimGroupSequence* sequence)
{
    return additiveSequenceMap.contains(sequence);
}

void AdditiveManager::EraseAdditiveSequence(BSAnimGroupSequence* sequence)
{
    additiveSequenceMap.erase(sequence);
    for (auto& block: sequence->GetControlledBlocks())
    {
        additiveInterpReferenceTransformsMap.erase(block.m_spInterpolator);
#if _DEBUG
        interpsToTargetsMap.erase(block.m_spInterpolator);
#endif
    }
    std::erase_if(referenceToAdditiveMap, _L(auto& p, p.second == sequence));
}

bool AdditiveManager::IsAdditiveInterpolator(NiInterpolator* interpolator)
{
    return additiveInterpReferenceTransformsMap.contains(interpolator);
}

bool AdditiveManager::StopManagedAdditiveSequenceFromParent(BSAnimGroupSequence* parentSequence, float afEaseOutTime)
{
    if (const auto iter = referenceToAdditiveMap.find(parentSequence); iter != referenceToAdditiveMap.end())
    {
        auto* additiveSequence = iter->second;
        float easeOutTime = 0.0f;
        if (!additiveSequence->m_spTextKeys->FindFirstByName("noBlend"))
            easeOutTime = afEaseOutTime == INVALID_TIME ? additiveSequence->GetEasingTime() : afEaseOutTime;
        additiveSequence->Deactivate(easeOutTime, false);
        return true;
    }
    return false;
}

NiQuatTransform* AdditiveManager::GetRefFrameTransform(NiInterpolator* interpolator)
{
    if (const auto iter = additiveInterpReferenceTransformsMap.find(interpolator); iter != additiveInterpReferenceTransformsMap.end())
        return &iter->second;
    return nullptr;
}

#if _DEBUG
const char* AdditiveManager::GetTargetNodeName(NiInterpolator* interpolator)
{
    if (const auto iter = interpsToTargetsMap.find(interpolator); iter != interpsToTargetsMap.end())
        return iter->second;
    return nullptr;
}
#endif


void ApplyAdditiveTransforms(
    NiBlendTransformInterpolator& kBlendInterp,
    float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue)
{
    NiPoint3 kFinalTranslate = NiPoint3::ZERO;
    NiQuaternion kFinalRotate = NiQuaternion(1.0f, 0.0f, 0.0f, 0.0f); // Identity quaternion
    float fFinalScale = 0.0f;

    bool bTransChanged = false;
    bool bRotChanged = false;
    bool bScaleChanged = false;

    for (auto& kItem : kBlendInterp.GetItems())
    {
        const auto* kRefFrameTransformPtr = AdditiveManager::GetRefFrameTransform(kItem.m_spInterpolator.data);
        if (!kRefFrameTransformPtr)
            continue; // not an additive interpolator
        if (kItem.m_fNormalizedWeight != 0.0f)
        {
#ifdef _DEBUG
            // this interpolator should not be factored into the normalized weight since that is reserved for the weighted blend animations
            // if normalized weight is not 0, then this interpolator is part of the weighted blend
            DebugBreakIfDebuggerPresent();
#endif
            continue;
        }

        if (kItem.m_cPriority < kBlendInterp.m_cNextHighPriority)
            continue;

        const float fUpdateTime = kBlendInterp.GetManagerControlled() ? kItem.m_fUpdateTime : fTime;
        if (fUpdateTime == INVALID_TIME)
            continue;
        const auto& kRefTransform = *kRefFrameTransformPtr;
        
        NiQuatTransform kInterpTransform;
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
        thread_local std::vector<std::pair<unsigned char, NiInterpolator*>> additiveInterpolators;
        // make sure that no additive interpolator gets factored into the normalized weight
        if (pBlendInterpolator->GetHasAdditiveTransforms())
        {
            unsigned char ucIndex = 0;
            bool bChanged = false;
            for (auto& item : pBlendInterpolator->GetItems())
            {
                if (item.m_spInterpolator != nullptr && IsAdditiveInterpolator(item.m_spInterpolator))
                {
                    additiveInterpolators.emplace_back(ucIndex, item.m_spInterpolator.data);
                    item.m_spInterpolator.data = nullptr;
                    pBlendInterpolator->m_ucInterpCount--;
                    bChanged = true;
                }
                ++ucIndex;
            }
            if (bChanged)
            {
                if (pBlendInterpolator->m_ucInterpCount == 1)
                    pBlendInterpolator->m_ucSingleIdx = pBlendInterpolator->GetFirstValidIndex();
                else
                    pBlendInterpolator->RecalculateHighPriorities();
            }
        }
        if (pBlendInterpolator->m_ucInterpCount != 0)
            ThisStdCall(uiComputeNormalizedWeightsAddr, pBlendInterpolator);
        if (!additiveInterpolators.empty())
        {
            if (pBlendInterpolator->m_ucInterpCount == 1)
                pBlendInterpolator->m_ucSingleIdx = INVALID_INDEX;
            for (auto& [index, interpolator] : additiveInterpolators)
            {
                auto& kInterpItem = pBlendInterpolator->m_pkInterpArray[index];
                kInterpItem.m_spInterpolator.data = interpolator;
                pBlendInterpolator->m_ucInterpCount++;
            }
            pBlendInterpolator->RecalculateHighPriorities();
            additiveInterpolators.clear();
        }
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
