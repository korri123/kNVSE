﻿#include "additive_anims.h"

#include "hooks.h"
#include "SafeWrite.h"
#include "utility.h"

struct AdditiveInterpMetadata
{
    NiControllerSequence* additiveSequence;
    NiQuatTransform refTransform;
    bool ignorePriorities;
};

using AdditiveInterpMetadataMap = std::unordered_map<NiInterpolator*, AdditiveInterpMetadata>;

struct AdditiveSequenceMetadata
{
    NiFixedString referencePoseSequenceName;
    float referenceTimePoint;
    bool ignorePriorities;
    NiControllerManager* controllerManager;
    bool firstPerson;
    bool operator==(const AdditiveSequenceMetadata& other) const = default;
};

std::unordered_map<NiControllerSequence*, AdditiveSequenceMetadata> additiveSequenceMetadataMap;
AdditiveInterpMetadataMap additiveInterpMetadataMap;

std::unordered_map<NiControllerSequence*, std::vector<NiControllerSequence*>> referenceToAdditiveMap;
#if _DEBUG
std::unordered_map<NiBlendInterpolator*, const char*> debugBlendInterpsToTargetsMap;
std::unordered_map<NiBlendInterpolator*, NiControllerSequence*> debugBlendInterpToSequenceMap;
#endif

std::shared_mutex g_additiveManagerMutex;

void AddReferencePoseTransforms(AnimData* animData, NiControllerSequence* additiveSequence,
                                   NiControllerSequence* referencePoseSequence, float timePoint, bool ignorePriorities,
                                   AdditiveInterpMetadataMap& refPoseTransforms)
{
    const auto refIdTags = referencePoseSequence->GetIDTags();
    const auto refBlocks = referencePoseSequence->GetControlledBlocks();

    NiNode* node = animData->nBip01;
    if (!node)
        return;
    for (size_t i = 0; i < refBlocks.size(); i++)
    {
        const auto& idTag = refIdTags[i];
        const auto& refBlock = refBlocks[i];
        NiInterpolator* refInterp = refBlock.m_spInterpolator;
        const auto* additiveBlock = additiveSequence->GetControlledBlock(idTag.m_kAVObjectName);
        if (!refInterp || !refBlock.m_pkBlendInterp || NOT_TYPE(refBlock.m_pkBlendInterp, NiBlendTransformInterpolator) || !additiveBlock)
            continue;
        NiInterpolator* additiveInterp = additiveBlock->m_spInterpolator;
        if (!additiveInterp)
            continue;
        
        NiAVObject* targetNode = node->GetObjectByName(idTag.m_kAVObjectName);
        if (!targetNode)
            continue;

        const float fOldTime = refInterp->m_fLastTime;
        NiQuatTransform refTransform;
        const bool bSuccess = refInterp->Update(timePoint, targetNode, refTransform);
        refInterp->m_fLastTime = fOldTime;
        if (bSuccess)
        {
            refPoseTransforms.insert_or_assign(additiveInterp, AdditiveInterpMetadata{additiveSequence, refTransform, ignorePriorities});
        }
    }
}

void InitDebugData(NiControllerSequence* additiveSequence)
{
#if _DEBUG
    const auto controlledBlocks = additiveSequence->GetControlledBlocks();
    for (unsigned int i = 0; i < controlledBlocks.size(); i++)
    {
        auto& idTag = additiveSequence->GetIDTags()[i];
        debugBlendInterpsToTargetsMap[controlledBlocks[i].m_pkBlendInterp] = idTag.m_kAVObjectName.CStr();
        debugBlendInterpToSequenceMap[controlledBlocks[i].m_pkBlendInterp] = additiveSequence;
    }
#endif
}

void MarkInterpolatorsAsAdditive(const NiControllerSequence* additiveSequence)
{
    for (const auto& block : additiveSequence->GetControlledBlocks())
    {
        if (block.m_pkBlendInterp && !block.m_pkBlendInterp->GetHasAdditiveTransforms() && IS_TYPE(block.m_pkBlendInterp, NiBlendTransformInterpolator))
            block.m_pkBlendInterp->SetHasAdditiveTransforms(true);
    }
}

void AdditiveManager::InitAdditiveSequence(AnimData* animData, NiControllerSequence* additiveSequence, NiControllerSequence* referencePoseSequence, float timePoint, bool ignorePriorities)
{
    const auto metadata = AdditiveSequenceMetadata {
        .referencePoseSequenceName = referencePoseSequence->m_kName,
        .referenceTimePoint = timePoint,
        .ignorePriorities = ignorePriorities,
        .controllerManager = animData->controllerManager,
        .firstPerson = animData == g_thePlayer->firstPersonAnimData
    };
    std::unique_lock lock(g_additiveManagerMutex);
    if (const auto iter = additiveSequenceMetadataMap.find(additiveSequence); iter != additiveSequenceMetadataMap.end())
    {
        if (iter->second == metadata)
            return;
    }
    additiveSequenceMetadataMap.insert_or_assign(additiveSequence, metadata);
    AddReferencePoseTransforms(animData, additiveSequence, referencePoseSequence, timePoint, ignorePriorities, additiveInterpMetadataMap);
    MarkInterpolatorsAsAdditive(additiveSequence);
    InitDebugData(additiveSequence);
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
    InitAdditiveSequence(animData, additiveAnim, referenceAnim, 0.0f, false);

    {
        std::unique_lock lock(g_additiveManagerMutex);
        referenceToAdditiveMap[referenceAnim].push_back(additiveAnim);
    }

    const BSAnimGroupSequence* currentAnim = nullptr;
    if (referenceAnim->animGroup)
        currentAnim = animData->animSequence[referenceAnim->animGroup->GetSequenceType()];
    float easeInTime = 0.0f;
    if (!additiveAnim->m_spTextKeys->FindFirstByName("noBlend"))
        easeInTime = GetDefaultBlendTime(additiveAnim, currentAnim);
    additiveAnim->Activate(0, true, additiveAnim->m_fSeqWeight, easeInTime, nullptr, false);
}

bool AdditiveManager::IsAdditiveSequence(NiControllerSequence* sequence)
{
    std::shared_lock lock(g_additiveManagerMutex);
    return additiveSequenceMetadataMap.contains(sequence);
}

void AdditiveManager::EraseAdditiveSequence(NiControllerSequence* sequence)
{
    std::unique_lock lock(g_additiveManagerMutex);
    additiveSequenceMetadataMap.erase(sequence);
    for (auto& additiveSequences : referenceToAdditiveMap | std::views::values)
    {
        std::erase_if(additiveSequences, _L(auto& p, p == sequence));
    }
}

bool IsAdditiveInterpolator(NiInterpolator* interpolator)
{
    return additiveInterpMetadataMap.contains(interpolator);
}

bool AdditiveManager::StopManagedAdditiveSequenceFromParent(BSAnimGroupSequence* parentSequence, float afEaseTime)
{
    std::shared_lock lock(g_additiveManagerMutex);
    if (const auto iter = referenceToAdditiveMap.find(parentSequence); iter != referenceToAdditiveMap.end())
    {
        for (const auto& additiveSequences = iter->second; auto* additiveSequence : additiveSequences)
        {
            auto* additiveAnimGroupSequence = static_cast<BSAnimGroupSequence*>(additiveSequence);
            if (NOT_TYPE(additiveAnimGroupSequence, BSAnimGroupSequence))
                continue;
            const float easeOutTime = afEaseTime == INVALID_TIME ? additiveAnimGroupSequence->GetEaseInTime() : afEaseTime;
            additiveSequence->Deactivate(easeOutTime, false);
        }
        return true;
    }
    return false;
}

AdditiveInterpMetadata* GetAdditiveInterpMetadata(NiInterpolator* interpolator)
{
    if (const auto iter = additiveInterpMetadataMap.find(interpolator); iter != additiveInterpMetadataMap.end())
        return &iter->second;
    return nullptr; // not an additive interpolator
}

void check_float(float value) {
#if _DEBUG
    if (std::isnan(value)) {
        DebugBreakIfDebuggerPresent();
    }
    else if (std::isinf(value)) {
        DebugBreakIfDebuggerPresent();
    }
    else if (value == FLT_MAX) {
        DebugBreakIfDebuggerPresent();
    }
    else if (value == -FLT_MAX) {
        DebugBreakIfDebuggerPresent();
    }
    else if (value == FLT_MIN) {
        DebugBreakIfDebuggerPresent();
    }
    else if (std::isfinite(value)) {
        return;
    }
    else {
        DebugBreakIfDebuggerPresent();
    }
#endif
}

void NiBlendTransformInterpolator::ApplyAdditiveTransforms(
    float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue) const
{
#if _DEBUG
    NiQuatTransform originalTransform = kValue;
#endif
    NiPoint3 kFinalTranslate = NiPoint3::ZERO;
    NiQuaternion kFinalRotate = NiQuaternion(1.0f, 0.0f, 0.0f, 0.0f); // Identity quaternion
    float fFinalScale = 0.0f;

    bool bTransChanged = false;
    bool bRotChanged = false;
    bool bScaleChanged = false;

#if _DEBUG
    if (false)
    {
        kValue = originalTransform;
    }
    const char* targetNodeName = debugBlendInterpsToTargetsMap[const_cast<NiBlendTransformInterpolator*>(this)];
#endif
    std::shared_lock lock(g_additiveManagerMutex);
    unsigned int uiNumAdditiveInterps = 0;
    for (auto& kItem : GetItems())
    {
        if (!kItem.m_spInterpolator)
            continue;
        const auto* kContext = GetAdditiveInterpMetadata(kItem.m_spInterpolator.data);
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
        ++uiNumAdditiveInterps;

        const auto& [sequence, kRefTransform, ignorePriorities] = *kContext;

        if (kItem.m_cPriority < m_cNextHighPriority && !ignorePriorities)
            continue;

        const float fUpdateTime = GetManagerControlled() ? kItem.m_fUpdateTime : fTime;
        if (fUpdateTime == INVALID_TIME)
            continue;
        
        NiQuatTransform kInterpTransform;
        if (kItem.m_spInterpolator->Update(fUpdateTime, pkInterpTarget, kInterpTransform))
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

#if _DEBUG && 0
    if (uiNumAdditiveInterps == 0)
        DebugBreakIfDebuggerPresent();
#endif
    
    if (bTransChanged && kValue.IsTranslateValid())
    {
        kValue.m_kTranslate += kFinalTranslate;
    }
    if (bRotChanged && kValue.IsRotateValid())
    {
        kValue.m_kRotate = kValue.m_kRotate * kFinalRotate;
        kValue.m_kRotate.Normalize();
    }
    if (bScaleChanged && kValue.IsScaleValid())
    {
        kValue.m_fScale += fFinalScale;
    }

#if _DEBUG
    check_float(kValue.m_kRotate.m_fW);
    check_float(kValue.m_kRotate.m_fX);
    check_float(kValue.m_kRotate.m_fY);
    check_float(kValue.m_kRotate.m_fZ);
    check_float(kValue.m_kTranslate.x);
    check_float(kValue.m_kTranslate.y);
    check_float(kValue.m_kTranslate.z);
#endif
}

void NiBlendInterpolator::ComputeNormalizedWeightsFor2Additive(InterpArrayItem* pkItem1, InterpArrayItem* pkItem2) const
{
    // Calculate the real weight of each item.
    float fRealWeight1 = pkItem1->m_fWeight * pkItem1->m_fEaseSpinner;
    float fRealWeight2 = pkItem2->m_fWeight * pkItem2->m_fEaseSpinner;
    if (fRealWeight1 == 0.0f && fRealWeight2 == 0.0f)
    {
        pkItem1->m_fNormalizedWeight = 0.0f;
        pkItem2->m_fNormalizedWeight = 0.0f;
        return;
    }

    // Compute normalized weights.
    if (pkItem1->m_cPriority > pkItem2->m_cPriority)
    {
        if (pkItem1->m_fEaseSpinner == 1.0f)
        {
            pkItem1->m_fNormalizedWeight = 1.0f;
            pkItem2->m_fNormalizedWeight = 0.0f;
            return;
        }

        float fOneMinusEaseSpinner = 1.0f - pkItem1->m_fEaseSpinner;
        float fOneOverSumOfWeights = 1.0f / (pkItem1->m_fEaseSpinner *
            fRealWeight1 + fOneMinusEaseSpinner * fRealWeight2);
        pkItem1->m_fNormalizedWeight = pkItem1->m_fEaseSpinner * fRealWeight1
            * fOneOverSumOfWeights;
        pkItem2->m_fNormalizedWeight = fOneMinusEaseSpinner * fRealWeight2 *
            fOneOverSumOfWeights;
    }
    else if (pkItem1->m_cPriority < pkItem2->m_cPriority)
    {
        if (pkItem2->m_fEaseSpinner == 1.0f)
        {
            pkItem1->m_fNormalizedWeight = 0.0f;
            pkItem2->m_fNormalizedWeight = 1.0f;
            return;
        }

        float fOneMinusEaseSpinner = 1.0f - pkItem2->m_fEaseSpinner;
        float fOneOverSumOfWeights = 1.0f / (pkItem2->m_fEaseSpinner *
            fRealWeight2 + fOneMinusEaseSpinner * fRealWeight1);
        pkItem1->m_fNormalizedWeight = fOneMinusEaseSpinner * fRealWeight1 *
            fOneOverSumOfWeights;
        pkItem2->m_fNormalizedWeight = pkItem2->m_fEaseSpinner * fRealWeight2
            * fOneOverSumOfWeights;
    }
    else
    {
        float fOneOverSumOfWeights = 1.0f / (fRealWeight1 + fRealWeight2);
        pkItem1->m_fNormalizedWeight = fRealWeight1 * fOneOverSumOfWeights;
        pkItem2->m_fNormalizedWeight = fRealWeight2 * fOneOverSumOfWeights;
    }

    // Only use the highest weight, if so desired.
    if (GetOnlyUseHighestWeight())
    {
        if (pkItem1->m_fNormalizedWeight >= pkItem2->m_fNormalizedWeight)
        {
            pkItem1->m_fNormalizedWeight = 1.0f;
            pkItem2->m_fNormalizedWeight = 0.0f;
        }
        else
        {
            pkItem1->m_fNormalizedWeight = 0.0f;
            pkItem2->m_fNormalizedWeight = 1.0f;
        }
        return;
    }

    // Exclude weights below threshold.
    if (m_fWeightThreshold > 0.0f)
    {
        bool bReduced1 = false;
        if (pkItem1->m_fNormalizedWeight < m_fWeightThreshold)
        {
            pkItem1->m_fNormalizedWeight = 0.0f;
            bReduced1 = true;
        }

        bool bReduced2 = false;
        if (pkItem2->m_fNormalizedWeight < m_fWeightThreshold)
        {
            pkItem2->m_fNormalizedWeight = 0.0f;
            bReduced1 = true;
        }

        if (bReduced1 && bReduced2)
        {
            return;
        }
        else if (bReduced1)
        {
            pkItem2->m_fNormalizedWeight = 1.0f;
            return;
        }
        else if (bReduced2)
        {
            pkItem1->m_fNormalizedWeight = 1.0f;
            return;
        }
    }
}

void NiBlendInterpolator::ComputeNormalizedWeightsAdditive()
{
    if (!GetComputeNormalizedWeights())
    {
        return;
    }

    SetComputeNormalizedWeights(false);

    thread_local std::vector<InterpArrayItem*> kItems;
    thread_local std::vector<unsigned int> kIndices;
    kItems.clear();
    kIndices.clear();
    
    kItems.reserve(m_ucArraySize);

    std::shared_lock lock(g_additiveManagerMutex);
    for (int i = 0; i < m_ucArraySize; i++)
    {
        auto& item = m_pkInterpArray[i];
        if (item.m_spInterpolator != nullptr)
        {
            if (!IsAdditiveInterpolator(item.m_spInterpolator))
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
    lock.unlock();

    if (kItems.empty())
    {
        if (m_ucInterpCount)
        {
            const float fValue = 1.0f / static_cast<float>(m_ucInterpCount);
            for (auto& item : GetItems())
            {
                if (item.m_spInterpolator)
                    item.m_fNormalizedWeight = fValue;
            }
        }
        return;
    }

    if (kItems.size() == 1)
    {
        kItems[0]->m_fNormalizedWeight = 1.0f;
        return;
    }

    if (kItems.size() == 2)
    {
        ComputeNormalizedWeightsFor2Additive(kItems[0], kItems[1]);
        return;
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
                kItem.m_pkBlendInterp = nullptr;
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
        if (kItem.m_pkBlendInterp && kItem.m_ucBlendIdx != INVALID_INDEX)
        {
            kItem.m_pkBlendInterp->RemoveInterpInfo(kItem.m_ucBlendIdx);
        }
    }
}

void NiBlendInterpolator::CalculatePrioritiesAdditive()
{
    m_cNextHighPriority = INVALID_INDEX;
    m_cHighPriority = INVALID_INDEX;

    if (m_ucInterpCount == 1 && m_ucSingleIdx != INVALID_INDEX)
    {
        m_cHighPriority = m_pkInterpArray[m_ucSingleIdx].m_cPriority;
        return;
    }
    for (auto& item : GetItems())
    {
        if (item.m_spInterpolator != nullptr && !IsAdditiveInterpolator(item.m_spInterpolator))
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
#if _DEBUG && 0
    const size_t numAdditiveInterps = ra::count_if(GetItems(), [](const auto& item) { return item.m_spInterpolator && IsAdditiveInterpolator(item.m_spInterpolator); });
    if (numAdditiveInterps == 0)
        DebugBreakIfDebuggerPresent();
#endif
}

void DebugSequence(NiControllerSequence* pkSequence);

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
            pBlendInterpolator->ApplyAdditiveTransforms(fTime, pkInterpTarget, *kValue);
        return true;
    }), &uiBlendValuesAddr);

    // NiControllerSequence::AttachInterpolators
    static UInt32 uiAttachInterpolatorsAddr;
    WriteRelCall(0xA34F71, INLINE_HOOK(void, __fastcall, NiControllerSequence* pkSequence, void*, char cPriority)
    {
        if (IsAdditiveSequence(pkSequence))
        {
            MarkInterpolatorsAsAdditive(pkSequence);
            pkSequence->AttachInterpolatorsAdditive(cPriority);
            return;
        }
#if _DEBUG && 0
        DebugSequence(pkSequence);
#endif
        ThisStdCall(uiAttachInterpolatorsAddr, pkSequence, cPriority);
        
    }), &uiAttachInterpolatorsAddr);
    
#if 0
    static UInt32 uiDetachInterpolatorsAddr;
    WriteRelCall(0xA350C5, INLINE_HOOK(void, __fastcall, NiControllerSequence* pkSequence)
    {
        if (IsAdditiveSequence(pkSequence))
            ClearLocalInterpsToTransformsMap(pkSequence);
        ThisStdCall(uiDetachInterpolatorsAddr, pkSequence);
    }), &uiDetachInterpolatorsAddr);
#endif

#if 0
    static UInt32 uiNiControllerSequenceUpdateAddr;
    WriteRelCall(0xA2E251, INLINE_HOOK(void, __fastcall, NiControllerSequence* pkSequence, void*, float fTime, bool bUpdateInterpolators)
    {
        if (pkSequence->m_fLastTime == -NI_INFINITY && pkSequence->m_eState != NiControllerSequence::INACTIVE && IsAdditiveSequence(pkSequence))
        {
            MarkInterpolatorsAsAdditive(pkSequence);
            PopulateAdditiveInterpsToTransformsMap(pkSequence);
        }
        ThisStdCall(uiNiControllerSequenceUpdateAddr, pkSequence, fTime, bUpdateInterpolators);
        //if (pkSequence->m_eState == NiControllerSequence::INACTIVE && IsAdditiveSequence(pkSequence))
        //    ClearLocalInterpsToTransformsMap(pkSequence);
    }), &uiNiControllerSequenceUpdateAddr);
#endif
}

void DebugSequence(NiControllerSequence* pkSequence)
{
#if _DEBUG
    auto idx = 0u;
    for (auto& block : pkSequence->GetControlledBlocks())
    {
        auto& idTag = pkSequence->GetIDTags()[idx];
        if (block.m_pkBlendInterp)
        {
            debugBlendInterpToSequenceMap[block.m_pkBlendInterp] = pkSequence;
            debugBlendInterpsToTargetsMap[block.m_pkBlendInterp] = idTag.m_kAVObjectName.CStr();
        }
        idx++;
    }
#endif
}

void DebugInterpolator(NiBlendInterpolator* pBlendInterpolator)
{
#if _DEBUG
    static std::unordered_set<UInt32> s_threadIds;
    static std::unordered_map<UInt32, std::unordered_set<const char*>> s_threadToNameMap;
    static std::unordered_map<UInt32, std::unordered_set<const char*>> s_threadToAnimMap;
    static std::unordered_map<UInt32, std::unordered_set<NiControllerManager*>> s_threadToActorMap;
    const auto currentThreadId = GetCurrentThreadId();
    const auto mainThreadId = OSGlobals::GetSingleton()->mainThreadID;
    const auto linearTaskThread = AILinearTaskManager::GetSingleton()->pThreads[0]->threadID;
    s_threadIds.insert(currentThreadId);
    auto* name = debugBlendInterpsToTargetsMap[pBlendInterpolator];
    s_threadToNameMap[currentThreadId].insert(name);
    auto* sequence = debugBlendInterpToSequenceMap[pBlendInterpolator];
    s_threadToAnimMap[currentThreadId].insert(sequence->m_kName.CStr());
    s_threadToActorMap[currentThreadId].insert(sequence->m_pkOwner);
#endif
}