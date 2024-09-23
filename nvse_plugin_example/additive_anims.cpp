#include "additive_anims.h"

#include "hooks.h"
#include "SafeWrite.h"
#include "utility.h"


std::unordered_map<NiPointer<NiInterpolator>, std::pair<BSAnimGroupSequence*, NiQuatTransform>> additiveInterpReferenceTransformsMap;
std::unordered_map<NiControllerSequence*, AdditiveAnimMetadata> additiveSequenceMap;
std::unordered_map<BSAnimGroupSequence*, std::vector<BSAnimGroupSequence*>> referenceToAdditiveMap;
#if _DEBUG
std::unordered_map<NiInterpolator*, const char*> interpsToTargetsMap;
#endif

std::shared_mutex g_additiveManagerMutex;

void AdditiveManager::InitAdditiveSequence(Actor* actor, BSAnimGroupSequence* additiveSequence, AdditiveAnimMetadata metadata)
{
    NiNode* node = actor->GetNiNode();
    if (!node)
        return;
    const auto [referencePoseSequence, timePoint, ignorePriorities] = metadata;
    std::unique_lock lock(g_additiveManagerMutex);
    if (const auto iter = additiveSequenceMap.find(additiveSequence); iter != additiveSequenceMap.end())
    {
        const auto& existingMetadata = iter->second;
        if (metadata == existingMetadata)
            return;
        additiveSequenceMap.erase(iter);
    }
    const auto baseIdTags = referencePoseSequence->GetIDTags();
    const auto baseBlocks = referencePoseSequence->GetControlledBlocks();
    for (size_t i = 0; i < baseBlocks.size(); i++)
    {
        const auto& baseBlock = baseBlocks[i];
        NiInterpolator* interpolator = baseBlock.m_spInterpolator;
        const NiInterpController* interpController = baseBlock.m_spInterpCtlr;
        if (interpolator && interpController)
        {
            const auto& idTag = baseIdTags[i];
            NiAVObject* targetNode = node->GetObjectByName(idTag.m_kAVObjectName);
            if (!targetNode)
                continue;

            const float fOldTime = interpolator->m_fLastTime;
            NiQuatTransform baseTransform;
            const bool bSuccess = interpolator->Update(timePoint, targetNode, baseTransform);
            interpolator->m_fLastTime = fOldTime;
            if (const auto* additiveBlock = additiveSequence->GetControlledBlock(idTag.m_kAVObjectName);
                bSuccess && additiveBlock && additiveBlock->m_spInterpolator)
            {
                additiveInterpReferenceTransformsMap[additiveBlock->m_spInterpolator] = { additiveSequence, baseTransform };
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
    additiveSequenceMap[additiveSequence] = metadata;
    MarkInterpolatorsAsAdditive(additiveSequence);
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
    InitAdditiveSequence(animData->actor, referenceAnim, AdditiveAnimMetadata{additiveAnim, 0.0f});

    {
        std::unique_lock lock(g_additiveManagerMutex);
        referenceToAdditiveMap[referenceAnim].push_back(additiveAnim);
    }

    BSAnimGroupSequence* currentAnim = nullptr;
    if (referenceAnim->animGroup)
        currentAnim = animData->animSequence[referenceAnim->animGroup->GetSequenceType()];
    float easeInTime = 0.0f;
    if (!additiveAnim->m_spTextKeys->FindFirstByName("noBlend"))
        easeInTime = GetDefaultBlendTime(additiveAnim, currentAnim);
    additiveAnim->Activate(0, true, additiveAnim->m_fSeqWeight, easeInTime, nullptr, false);
}

void AdditiveManager::MarkInterpolatorsAsAdditive(BSAnimGroupSequence* additiveSequence)
{
    for (const auto& block : additiveSequence->GetControlledBlocks())
    {
        if (block.m_pkBlendInterp && !block.m_pkBlendInterp->GetHasAdditiveTransforms())
            block.m_pkBlendInterp->SetHasAdditiveTransforms(true);
    }
}

bool AdditiveManager::IsAdditiveSequence(BSAnimGroupSequence* sequence)
{
    std::shared_lock lock(g_additiveManagerMutex);
    return additiveSequenceMap.contains(sequence);
}

void AdditiveManager::EraseAdditiveSequence(BSAnimGroupSequence* sequence)
{
    std::unique_lock lock(g_additiveManagerMutex);
    additiveSequenceMap.erase(sequence);
    for (auto& block: sequence->GetControlledBlocks())
    {
        additiveInterpReferenceTransformsMap.erase(block.m_spInterpolator);
#if _DEBUG
        interpsToTargetsMap.erase(block.m_spInterpolator);
#endif
    }
    for (auto& additiveSequences : referenceToAdditiveMap | std::views::values)
    {
        std::erase_if(additiveSequences, _L(auto& p, p == sequence));
    }
}

bool AdditiveManager::IsAdditiveInterpolator(NiInterpolator* interpolator)
{
    std::shared_lock lock(g_additiveManagerMutex);
    return additiveInterpReferenceTransformsMap.contains(interpolator);
}

bool AdditiveManager::StopManagedAdditiveSequenceFromParent(BSAnimGroupSequence* parentSequence, float afEaseOutTime)
{
    std::shared_lock lock(g_additiveManagerMutex);
    if (const auto iter = referenceToAdditiveMap.find(parentSequence); iter != referenceToAdditiveMap.end())
    {
        for (const auto& additiveSequences = iter->second; auto* additiveSequence : additiveSequences)
        {
            float easeOutTime = 0.0f;
            if (!additiveSequence->m_spTextKeys->FindFirstByName("noBlend"))
                easeOutTime = afEaseOutTime == INVALID_TIME ? additiveSequence->GetEaseInTime() : afEaseOutTime;
            additiveSequence->Deactivate(easeOutTime, false);
        }
        return true;
    }
    return false;
}

bool DoesIgnorePriorities(BSAnimGroupSequence* sequence)
{
    if (const auto iter = additiveSequenceMap.find(sequence); iter != additiveSequenceMap.end())
        return iter->second.ignorePriorities;
    return false;
}

std::pair<BSAnimGroupSequence*, NiQuatTransform>* AdditiveManager::GetSequenceAndRefFrameTransform(NiInterpolator* interpolator)
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
    
    std::shared_lock lock(g_additiveManagerMutex);
    for (auto& kItem : kBlendInterp.GetItems())
    {
        const auto* kContext = AdditiveManager::GetSequenceAndRefFrameTransform(kItem.m_spInterpolator.data);
        if (!kContext)
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

        const auto& [sequence, kRefTransform] = *kContext;

        if (kItem.m_cPriority < kBlendInterp.m_cNextHighPriority && !DoesIgnorePriorities(sequence))
            continue;

        const float fUpdateTime = kBlendInterp.GetManagerControlled() ? kItem.m_fUpdateTime : fTime;
        if (fUpdateTime == INVALID_TIME)
            continue;
        
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
    lock.unlock();
    
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

void NiBlendInterpolator::ComputeNormalizedWeightsAdditive()
{
    if (!GetComputeNormalizedWeights())
    {
        return;
    }

    SetComputeNormalizedWeights(false);
    
    if (m_ucInterpCount == 1)
    {
        if (AdditiveManager::IsAdditiveInterpolator(m_pkInterpArray[m_ucSingleIdx].m_spInterpolator))
            m_pkInterpArray[m_ucSingleIdx].m_fNormalizedWeight = 0.0f;
        else
            m_pkInterpArray[m_ucSingleIdx].m_fNormalizedWeight = 1.0f;
        return;
    }
#if 0
    if (m_ucInterpCount == 2)
    {
        const bool bIsFirstAdditive = AdditiveManager::IsAdditiveInterpolator(m_pkInterpArray[0].m_spInterpolator);
        const bool bIsSecondAdditive = AdditiveManager::IsAdditiveInterpolator(m_pkInterpArray[1].m_spInterpolator);
        if (bIsFirstAdditive && !bIsSecondAdditive)
        {
            m_pkInterpArray[0].m_fNormalizedWeight = 0.0f;
            m_pkInterpArray[1].m_fNormalizedWeight = 1.0f;
            return;
        }
        if (bIsSecondAdditive && !bIsFirstAdditive)
        {
            m_pkInterpArray[0].m_fNormalizedWeight = 1.0f;
            m_pkInterpArray[1].m_fNormalizedWeight = 0.0f;
            return;
        }
        if (bIsFirstAdditive && bIsSecondAdditive)
        {
            m_pkInterpArray[0].m_fNormalizedWeight = 0.0f;
            m_pkInterpArray[1].m_fNormalizedWeight = 0.0f;
            return;
        }
        ThisStdCall(0xA36BD0, this); // NiBlendInterpolator::ComputeNormalizedWeightsFor2
        return;
    }
#endif

    thread_local std::vector<InterpArrayItem*> kItems;
    thread_local std::vector<unsigned int> kIndices;
    kItems.clear();
    kIndices.clear();
    
    kItems.reserve(m_ucArraySize);
    for (int i = 0; i < m_ucArraySize; i++)
    {
        auto& item = m_pkInterpArray[i];
        if (item.m_spInterpolator != nullptr)
        {
            if (!AdditiveManager::IsAdditiveInterpolator(item.m_spInterpolator))
            {
                kItems.push_back(&item);
                kIndices.push_back(i);
            }
            else
            {
                item.m_fNormalizedWeight = 0.0f;
            }
        }
    }
    
    if (m_fHighSumOfWeights == -NI_INFINITY)
    {
        // Compute sum of weights for highest and next highest priorities,
        // along with highest ease spinner for the highest priority.
        m_fHighSumOfWeights = 0.0f;
        m_fNextHighSumOfWeights = 0.0f;
        m_fHighEaseSpinner = 0.0f;
        for (auto* kItemPtr : kItems)
        {
            auto& kItem = *kItemPtr;
            if (kItem.m_spInterpolator != NULL)
            {
                float fRealWeight = kItem.m_fWeight * kItem.m_fEaseSpinner;
                if (kItem.m_cPriority == m_cHighPriority)
                {
                    m_fHighSumOfWeights += fRealWeight;
                    if (kItem.m_fEaseSpinner > m_fHighEaseSpinner)
                    {
                        m_fHighEaseSpinner = kItem.m_fEaseSpinner;
                    }
                }
                else if (kItem.m_cPriority == m_cNextHighPriority)
                {
                    m_fNextHighSumOfWeights += fRealWeight;
                }
            }
        }
    }


    const float fOneMinusHighEaseSpinner = 1.0f - m_fHighEaseSpinner;
    const float fTotalSumOfWeights = m_fHighEaseSpinner * m_fHighSumOfWeights +
        fOneMinusHighEaseSpinner * m_fNextHighSumOfWeights;
    const float fOneOverTotalSumOfWeights =
        fTotalSumOfWeights > 0.0f ? 1.0f / fTotalSumOfWeights : 0.0f;


    // Compute normalized weights.
    for (auto* kItemPtr : kItems)
    {
        auto& kItem = *kItemPtr;
        if (kItem.m_spInterpolator != nullptr)
        {
            if (kItem.m_cPriority == m_cHighPriority)
            {
                kItem.m_fNormalizedWeight = m_fHighEaseSpinner *
                    kItem.m_fWeight * kItem.m_fEaseSpinner *
                    fOneOverTotalSumOfWeights;
            }
            else if (kItem.m_cPriority == m_cNextHighPriority)
            {
                kItem.m_fNormalizedWeight = fOneMinusHighEaseSpinner *
                    kItem.m_fWeight * kItem.m_fEaseSpinner *
                    fOneOverTotalSumOfWeights;
            }
            else
            {
                kItem.m_fNormalizedWeight = 0.0f;
            }
        }
    }

    // Exclude weights below threshold, computing new sum in the process.
    float fSumOfNormalizedWeights = 1.0f;
    if (m_fWeightThreshold > 0.0f)
    {
        fSumOfNormalizedWeights = 0.0f;
        for (auto* kItemPtr : kItems)
        {
            auto& kItem = *kItemPtr;
            if (kItem.m_spInterpolator != nullptr &&
                kItem.m_fNormalizedWeight != 0.0f)
            {
                if (kItem.m_fNormalizedWeight < m_fWeightThreshold)
                {
                    kItem.m_fNormalizedWeight = 0.0f;
                }
                fSumOfNormalizedWeights += kItem.m_fNormalizedWeight;
            }
        }
    }

    // Renormalize weights if any were excluded earlier.
    if (fSumOfNormalizedWeights != 1.0f)
    {
        // Renormalize weights.
        float fOneOverSumOfNormalizedWeights =
            (fSumOfNormalizedWeights > 0.0f) ? (1.0f / fSumOfNormalizedWeights) : 0.0f;

        for (auto* kItemPtr : kItems)
        {
            auto& kItem = *kItemPtr;
            if (kItem.m_fNormalizedWeight != 0.0f)
            {
                kItem.m_fNormalizedWeight = kItem.m_fNormalizedWeight *
                    fOneOverSumOfNormalizedWeights;
            }
        }
    }

    // Only use the highest weight, if so directed.
    if (GetOnlyUseHighestWeight())
    {
        float fHighest = -1.0f;
        unsigned char ucHighIndex = INVALID_INDEX;
        for (auto uc : kIndices)
        {
            if (m_pkInterpArray[uc].m_fNormalizedWeight > fHighest)
            {
                ucHighIndex = uc;
                fHighest = m_pkInterpArray[uc].m_fNormalizedWeight;
            }
            m_pkInterpArray[uc].m_fNormalizedWeight = 0.0f;
        }

        // Set the highest index to 1.0
        m_pkInterpArray[ucHighIndex].m_fNormalizedWeight = 1.0f;
    }
}

void NiControllerSequence::AttachInterpolatorsAdditive(char cPriority) const
{
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem& kItem = m_pkInterpArray[ui];
        if (kItem.m_spInterpolator != nullptr && kItem.m_pkBlendInterp != nullptr)
        {
            if (NOT_TYPE(kItem.m_pkBlendInterp, NiBlendTransformInterpolator))
            {
                // make sure that only blend transform interpolators are attached
                kItem.m_ucBlendIdx = INVALID_INDEX;
                continue;
            }
            kItem.m_ucBlendIdx = kItem.m_pkBlendInterp->AddInterpInfo(
                kItem.m_spInterpolator,
                m_fSeqWeight,
                kItem.m_ucPriority != INVALID_INDEX ? kItem.m_ucPriority : cPriority,
                1.0f);
        }
    }
}

void NiControllerSequence::DetachInterpolatorsAdditive() const
{
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem &kItem = m_pkInterpArray[ui];
        if (kItem.m_pkBlendInterp && IS_TYPE(kItem.m_pkBlendInterp, NiBlendTransformInterpolator))
        {
            kItem.m_pkBlendInterp->RemoveInterpInfo(kItem.m_ucBlendIdx);
        }
    }
}

void NiBlendInterpolator::CalculatePrioritiesAdditive()
{
    m_cNextHighPriority = INVALID_INDEX;
    m_cHighPriority = INVALID_INDEX;
    for (auto& item : GetItems())
    {
        if (item.m_spInterpolator != nullptr && !AdditiveManager::IsAdditiveInterpolator(item.m_spInterpolator))
        {
            if (item.m_cPriority > m_cNextHighPriority)
            {
                if (item.m_cPriority > m_cHighPriority)
                {
                    m_cNextHighPriority = m_cHighPriority;
                    m_cHighPriority = item.m_cPriority;
                }
                else if (item.m_cPriority < m_cHighPriority)
                {
                    m_cNextHighPriority = item.m_cPriority;
                }
            }
        }
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
            pBlendInterpolator->CalculatePrioritiesAdditive();
            pBlendInterpolator->ComputeNormalizedWeightsAdditive();
            return;
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

    // NiControllerSequence::AttachInterpolators
    static UInt32 uiAttachInterpolatorsAddr;
    WriteRelCall(0xA34F71, INLINE_HOOK(void, __fastcall, NiControllerSequence* pkSequence, void*, char cPriority)
    {
        if (IsAdditiveSequence(static_cast<BSAnimGroupSequence*>(pkSequence)))
        {
            pkSequence->AttachInterpolatorsAdditive(cPriority);
            return;
        }
        ThisStdCall(uiAttachInterpolatorsAddr, pkSequence, cPriority);
        
    }), &uiAttachInterpolatorsAddr);

    // NiControllerSequence::DetachInterpolators
    static UInt32 uiDetachInterpolatorsAddr;
    WriteRelCall(0xA350C5, INLINE_HOOK(void, __fastcall, NiControllerSequence* pkSequence)
    {
        if (IsAdditiveSequence(static_cast<BSAnimGroupSequence*>(pkSequence)))
        {
            pkSequence->DetachInterpolatorsAdditive();
            return;
        }
        ThisStdCall(uiDetachInterpolatorsAddr, pkSequence);
        
    }), &uiDetachInterpolatorsAddr);
}
