#include "additive_anims.h"

#include "blend_fixes.h"
#include "blend_smoothing.h"
#include "hooks.h"
#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "utility.h"

namespace
{
    void AddReferencePoseTransforms(AnimData* animData, NiControllerSequence* additiveSequence,
                                       NiControllerSequence* referencePoseSequence, float timePoint, bool ignorePriorities)
    {
        const auto refBlocks = referencePoseSequence->GetControlledBlocks();

        NiNode* node = animData->nBip01;
        if (!node)
            return;
        for (auto& refBlock : refBlocks)
        {
            const auto& idTag = refBlock.GetIDTag(referencePoseSequence);
            NiInterpolator* refInterp = refBlock.m_spInterpolator;
            const auto* additiveBlock = additiveSequence->GetControlledBlock(idTag.m_kAVObjectName);
            if (!refInterp || !refBlock.m_pkBlendInterp || !NI_DYNAMIC_CAST(NiBlendTransformInterpolator, refBlock.m_pkBlendInterp) || !additiveBlock)
                continue;
            NiInterpolator* additiveInterp = additiveBlock->m_spInterpolator;
            if (!additiveInterp)
                continue;

            auto* target = animData->controllerManager->GetTarget(refBlock.m_spInterpCtlr, idTag);
            if (!target)
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
                auto* extraData = kBlendInterpolatorExtraData::Obtain(target);
                DebugAssert(extraData);
                auto& extraInterpItem = extraData->ObtainItem(additiveInterp);
                extraInterpItem.isAdditive = true;
                extraInterpItem.additiveMetadata.refTransform = refTransform;
                extraInterpItem.additiveMetadata.ignorePriorities = ignorePriorities;
            }
        }
    }

    bool IsAdditiveInterpolator(kBlendInterpolatorExtraData* extraData, NiInterpolator* interpolator)
    {
        auto* kExtraItem = extraData->GetItem(interpolator);
        if (!kExtraItem)
            return false;
        return kExtraItem->isAdditive;
    }

    kAdditiveInterpMetadata* GetAdditiveInterpMetadata(kBlendInterpolatorExtraData* extraData, NiInterpolator* interpolator)
    {
        auto* kExtraItem = extraData->GetItem(interpolator);
        if (!kExtraItem || !kExtraItem->isAdditive)
            return nullptr;
        return &kExtraItem->additiveMetadata;
    }
}

void AdditiveManager::MarkInterpolatorsAsAdditive(const NiControllerSequence* additiveSequence)
{
    for (const auto& block : additiveSequence->GetControlledBlocks())
    {
        if (block.m_pkBlendInterp && !block.m_pkBlendInterp->GetHasAdditiveTransforms() && NI_DYNAMIC_CAST(NiBlendTransformInterpolator, block.m_pkBlendInterp))
            block.m_pkBlendInterp->SetHasAdditiveTransforms(true);
    }
}

void AdditiveManager::InitAdditiveSequence(AnimData* animData, NiControllerSequence* additiveSequence, NiControllerSequence* referencePoseSequence, float timePoint, bool ignorePriorities)
{
    auto metadata = AdditiveSequenceMetadata {
        .type = sAdditiveSequenceMetadata,
        .referencePoseSequenceName = referencePoseSequence->m_kName,
        .referenceTimePoint = timePoint,
        .ignorePriorities = ignorePriorities,
        .controllerManager = animData->controllerManager,
        .firstPerson = animData == g_thePlayer->firstPersonAnimData
    };

    auto* sequenceExtraData = SequenceExtraDatas::Get(additiveSequence);

    if (sequenceExtraData->additiveMetadata)
    {
        if (metadata == *sequenceExtraData->additiveMetadata)
            return;
        *sequenceExtraData->additiveMetadata = metadata;
    }
    else
    {
        sequenceExtraData->additiveMetadata = std::make_unique<AdditiveSequenceMetadata>(metadata);
    }
    AddReferencePoseTransforms(animData, additiveSequence, referencePoseSequence, timePoint, ignorePriorities);
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
    InitAdditiveSequence(animData, additiveAnim, referenceAnim, 0.0f, false);

    BSAnimGroupSequence* currentAnim = nullptr;
    if (referenceAnim->animGroup)
        currentAnim = animData->animSequence[referenceAnim->animGroup->GetSequenceType()];
    float easeInTime = 0.0f;
    const static NiFixedString sNoBlend = "noBlend";
    if (!additiveAnim->m_spTextKeys->FindFirstByName(sNoBlend))
        easeInTime = GetDefaultBlendTime(additiveAnim, currentAnim);
    const auto result = additiveAnim->Activate(0, true, additiveAnim->m_fSeqWeight, easeInTime, nullptr, false);
    if (result)
    {
        if (auto* animTime = HandleExtraOperations(animData, additiveAnim, true))
        {
            animTime->endIfSequenceTypeChanges = false;
            animTime->isOverlayAdditiveAnim = true;
            animTime->referenceAnim = currentAnim;
        }
    }
}

bool AdditiveManager::IsAdditiveSequence(NiControllerSequence* sequence)
{
    if (!sequence->m_spTextKeys)
        return false;
    auto* sequenceExtraData = SequenceExtraDatas::Get(sequence);
    return sequenceExtraData->additiveMetadata != nullptr;
}

void AdditiveManager::EraseAdditiveSequence(NiControllerSequence* sequence)
{
    for (auto& item : sequence->GetControlledBlocks())
    {
        if (item.m_pkBlendInterp)
            item.m_pkBlendInterp->SetHasAdditiveTransforms(false);
    }
}

bool AdditiveManager::IsAdditiveInterpolator(NiObjectNET* target, NiInterpolator* interpolator)
{
    auto* extraData = kBlendInterpolatorExtraData::GetExtraData(target);
    if (!extraData)
        return false;
    return ::IsAdditiveInterpolator(extraData, interpolator);
}

static kAdditiveInterpMetadata* GetAdditiveInterpMetadata(NiObjectNET* target, NiInterpolator* interpolator)
{
    auto* extraData = kBlendInterpolatorExtraData::GetExtraData(target);
    if (!extraData)
        return nullptr;
    return GetAdditiveInterpMetadata(extraData, interpolator);
}

void AdditiveManager::SetAdditiveInterpWeightMult(NiObjectNET* target, NiInterpolator* interpolator, float weightMult)
{
    if (auto* metadata = GetAdditiveInterpMetadata(target, interpolator); metadata)
    {
        metadata->weightMultiplier = weightMult;
    }
}

void NiBlendTransformInterpolator::ApplyAdditiveTransforms(
    float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue) const
{
#if _DEBUG
    NiQuatTransform originalTransform = kValue;
    NiMatrix3 originalRotation;
    kValue.GetRotate().ToRotation(originalRotation);
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
#endif
    unsigned int uiNumAdditiveInterps = 0;
    for (auto& kItem : GetItems())
    {
        if (!kItem.m_spInterpolator)
            continue;
        const auto* kContext = GetAdditiveInterpMetadata(pkInterpTarget, kItem.m_spInterpolator.data);
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

        const auto& [kRefTransform, ignorePriorities, weightMultiplier] = *kContext;

        if (kItem.m_cPriority < m_cNextHighPriority && !ignorePriorities)
            continue;

        const float fUpdateTime = GetManagerControlled() ? kItem.m_fUpdateTime : fTime;
        if (fUpdateTime == INVALID_TIME)
            continue;
        
        NiQuatTransform kInterpTransform;
        if (kItem.m_spInterpolator->Update(fUpdateTime, pkInterpTarget, kInterpTransform))
        {
            const float fWeight = kItem.m_fWeight * kItem.m_fEaseSpinner * weightMultiplier;
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

#define ADD_INVALID_TRANSFORM 0
    
    if (bTransChanged)
    {
        if (kValue.IsTranslateValid())
            kValue.m_kTranslate += kFinalTranslate;
#if ADD_INVALID_TRANSFORM
        else if (const auto* node = pkInterpTarget->GetAsNiNode())
            kValue.m_kTranslate = node->m_kLocal.m_Translate + kFinalTranslate;
#endif
    }
    if (bRotChanged)
    {
        if (kValue.IsRotateValid())
        {
            kValue.m_kRotate = kValue.m_kRotate * kFinalRotate;
            kValue.m_kRotate.Normalize();
        }
#if ADD_INVALID_TRANSFORM
        else if (const auto* node = pkInterpTarget->GetAsNiNode())
        {
            NiQuaternion kValueRotate;
            kValueRotate.FromRotation(node->m_kLocal.m_Rotate);
            kValueRotate = kValueRotate * kFinalRotate;
            kValueRotate.Normalize();
            kValue.m_kRotate = kFinalRotate;
        }
#endif
    }
    if (bScaleChanged)
    {
        if (kValue.IsScaleValid())
            kValue.m_fScale += fFinalScale;
#if ADD_INVALID_TRANSFORM
        else if (const auto* node = pkInterpTarget->GetAsNiNode())
        {
            kValue.m_fScale = node->m_kLocal.m_fScale + fFinalScale;
        }
#endif
    }
#if _DEBUG
    NiMatrix3 finalRotation;
    kValue.GetRotate().ToRotation(finalRotation);

#endif

#if _DEBUG && 0
    check_float(kValue.m_kRotate.m_fW);
    check_float(kValue.m_kRotate.m_fX);
    check_float(kValue.m_kRotate.m_fY);
    check_float(kValue.m_kRotate.m_fZ);
    check_float(kValue.m_kTranslate.x);
    check_float(kValue.m_kTranslate.y);
    check_float(kValue.m_kTranslate.z);
#endif
}

void NiBlendInterpolator::ComputeNormalizedWeightsFor2(InterpArrayItem* pkItem1, InterpArrayItem* pkItem2)
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
    
}

void NiBlendInterpolator::ComputeNormalizedWeightsAdditive(kBlendInterpolatorExtraData* kExtraData)
{
    if (!GetComputeNormalizedWeights())
    {
        return;
    }

    DebugAssert(kExtraData);

    if (!kExtraData) 
    {
        ComputeNormalizedWeights();
        return;
    }

    SetComputeNormalizedWeights(false);

    thread_local std::vector<InterpArrayItem*> kItems;
    thread_local std::vector<unsigned int> kIndices;
    kItems.clear();
    kIndices.clear();
    
    kItems.reserve(m_ucArraySize);
    kIndices.reserve(m_ucArraySize);

    for (int i = 0; i < m_ucArraySize; i++)
    {
        auto& item = m_pkInterpArray[i];
        if (item.m_spInterpolator != nullptr)
        {
            if (!IsAdditiveInterpolator(kExtraData, item.m_spInterpolator))
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

    if (kItems.empty())
    {
        return;
    }

    if (kItems.size() == 1)
    {
        kItems[0]->m_fNormalizedWeight = 1.0f;
        return;
    }

    if (kItems.size() == 2)
    {
        ComputeNormalizedWeightsFor2(kItems[0], kItems[1]);
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
            if (!NI_DYNAMIC_CAST(NiBlendTransformInterpolator, kItem.m_pkBlendInterp))
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

void NiBlendInterpolator::CalculatePrioritiesAdditive(kBlendInterpolatorExtraData* kExtraData)
{
    DebugAssert(kExtraData);
    if (!kExtraData)
    {
        return;
    }
    m_cNextHighPriority = INVALID_INDEX;
    m_cHighPriority = INVALID_INDEX;

    if (m_ucInterpCount == 1 && m_ucSingleIdx != INVALID_INDEX)
    {
        m_cHighPriority = m_pkInterpArray[m_ucSingleIdx].m_cPriority;
        return;
    }
    for (auto& item : GetItems())
    {
        if (item.m_spInterpolator != nullptr && !IsAdditiveInterpolator(kExtraData, item.m_spInterpolator))
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
    // AnimData::Refresh
    // this function deactivates all active sequences but only reactivates the ones in AnimData::animSequence
    // this will also reactivate unmanaged sequences (which additive anims are)
    static UInt32 uiAnimDataRefreshAddr = 0x499240;
    WriteRelCall(0x8B0B8C, INLINE_HOOK(UInt32, __fastcall, AnimData* animData, void*, char cUnk)
    {
        const auto doRefresh = [&]
        {
            return ThisStdCall<UInt32>(uiAnimDataRefreshAddr, animData, cUnk);
        };
        std::vector<NiControllerSequence*> sequences;
        auto* controllerManager = animData->controllerManager;
        if (!controllerManager || controllerManager->m_kActiveSequences.length == 0)
            return doRefresh();
        
        sequences.reserve(controllerManager->m_kActiveSequences.length);
        for (auto* item : controllerManager->m_kActiveSequences)
        {
            if (IsAdditiveSequence(item))
                sequences.push_back(item);
        }
        const auto result = doRefresh();
        for (auto* sequence : sequences)
        {
            if (sequence->m_eState == NiControllerSequence::INACTIVE)
                controllerManager->ActivateSequence(sequence, 0, false, sequence->m_fSeqWeight, 0.0f, nullptr);
        }
        return result;
    }), &uiAnimDataRefreshAddr);
}