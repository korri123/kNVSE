#include "additive_anims.h"

#include "blend_fixes.h"
#include "blend_smoothing.h"
#include "hooks.h"
#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "utility.h"

namespace
{
    
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

void AdditiveManager::AddReferencePoseTransforms(NiControllerSequence* additiveSequence, NiControllerSequence* referencePoseSequence, float timePoint, bool ignorePriorities)
{
    const auto refBlocks = referencePoseSequence->GetControlledBlocks();
    if (!additiveSequence->m_pkOwner || !additiveSequence->m_pkOwner->GetObjectPalette())
    {
        DebugAssert(false);
        return;
    }
    auto* owner = additiveSequence->m_pkOwner;
    if (!owner)
    {
        DebugAssert(false);
        return;
    }
    for (auto& refBlock : refBlocks)
    {
        const auto& idTag = refBlock.GetIDTag(referencePoseSequence);
        NiInterpolator* refInterp = refBlock.m_spInterpolator;
        const auto* additiveBlock = 
            additiveSequence == referencePoseSequence ? &refBlock :
            additiveSequence->GetControlledBlock(idTag.m_kAVObjectName);
        if (!refInterp || !refBlock.m_pkBlendInterp || !NI_DYNAMIC_CAST(NiBlendTransformInterpolator, refBlock.m_pkBlendInterp) || !additiveBlock)
            continue;
        NiInterpolator* additiveInterp = additiveBlock->m_spInterpolator;
        if (!additiveInterp)
            continue;
        
        NiAVObject* targetNode = owner->GetTarget(refBlock.m_spInterpCtlr, idTag);
        if (!targetNode)
            continue;

        const float fOldTime = refInterp->m_fLastTime;
        NiQuatTransform refTransform;
        const bool bSuccess = refInterp->Update(timePoint, targetNode, refTransform);
        refInterp->m_fLastTime = fOldTime;
        if (bSuccess)
        {
            auto* extraData = kBlendInterpolatorExtraData::GetOrCreate(targetNode);
            DebugAssert(extraData);
            extraData->owner = owner;

            auto& extraInterpItem = extraData->GetOrCreateItem(additiveInterp);
            extraInterpItem.isAdditive = true;
            extraInterpItem.sequence = additiveSequence;
            extraInterpItem.additiveMetadata.refTransform = refTransform;
            extraInterpItem.additiveMetadata.ignorePriorities = ignorePriorities;
        }
    }
}

void AdditiveManager::InitAdditiveSequence(AnimData* animData, NiControllerSequence* additiveSequence, NiControllerSequence* referencePoseSequence, float timePoint, bool ignorePriorities)
{
    auto metadata = AdditiveSequenceMetadata {
        .type = sAdditiveSequenceMetadata,
        .referencePoseSequence = referencePoseSequence,
        .referenceTimePoint = timePoint,
        .ignorePriorities = ignorePriorities,
        .controllerManager = animData->controllerManager,
        .firstPerson = animData == g_thePlayer->firstPersonAnimData
    };

    auto* sequenceExtraData = SequenceExtraDatas::GetOrCreate(additiveSequence);

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

bool AdditiveManager::IsAdditiveSequence(const NiControllerSequence* sequence)
{
    auto* sequenceExtraData = SequenceExtraDatas::Get(sequence);
    if (!sequenceExtraData)
        return false;
    return sequenceExtraData->additiveMetadata != nullptr;
}

void AdditiveManager::RemoveAdditiveTransformsFlags(NiControllerSequence* sequence)
{
    for (auto& item : sequence->GetControlledBlocks())
    {
        if (item.m_pkBlendInterp)
            item.m_pkBlendInterp->SetHasAdditiveTransforms(false);
    }
}

bool AdditiveManager::IsAdditiveInterpolator(kBlendInterpolatorExtraData* extraData, NiInterpolator* interpolator)
{
    if (!extraData)
        return false;
    return ::IsAdditiveInterpolator(extraData, interpolator);
}

void AdditiveManager::SetAdditiveInterpWeightMult(NiObjectNET* target, NiInterpolator* interpolator, float weightMult)
{
    auto* extraData = kBlendInterpolatorExtraData::GetOrCreate(target);
    if (auto* metadata = GetAdditiveInterpMetadata(extraData, interpolator); metadata)
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
    
    auto* extraData = kBlendInterpolatorExtraData::GetExtraData(pkInterpTarget);
    DebugAssert(extraData);
    if (!extraData)
        return;

    unsigned int uiNumAdditiveInterps = 0;
    for (auto& kItem : GetItems())
    {
        if (!kItem.m_spInterpolator)
            continue;
        const auto* kContext = GetAdditiveInterpMetadata(extraData, kItem.m_spInterpolator.data);
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

void NiControllerSequence::AttachInterpolatorsAdditive(char cPriority, SequenceExtraData* sequenceExtraData)
{
    DebugAssert(sequenceExtraData && sequenceExtraData->additiveMetadata);
    if (!sequenceExtraData || !sequenceExtraData->additiveMetadata)
        return;

    auto& metadata = *sequenceExtraData->additiveMetadata;
    AdditiveManager::AddReferencePoseTransforms(this, metadata.referencePoseSequence.data, metadata.referenceTimePoint, metadata.ignorePriorities);
    
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem& kItem = m_pkInterpArray[ui];
        auto* pkBlendInterp = kItem.m_pkBlendInterp;
        if (kItem.m_spInterpolator != nullptr && pkBlendInterp != nullptr)
        {
            if (!NI_DYNAMIC_CAST(NiBlendTransformInterpolator, kItem.m_pkBlendInterp))
            {
                // make sure that only blend transform interpolators are attached
                kItem.m_ucBlendIdx = INVALID_INDEX;
                kItem.m_pkBlendInterp = nullptr;
                continue;
            }
            if (!kItem.m_spInterpolator || !pkBlendInterp)
                continue;
            const auto cInterpPriority = static_cast<unsigned char>(kItem.m_ucPriority) != 0xFFui8 ? kItem.m_ucPriority : cPriority;

            auto& idTag = kItem.GetIDTag(this);
            auto* target = m_pkOwner->GetTarget(kItem.m_spInterpCtlr, idTag);
            if (!target) 
            {
                // hello "Ripper" from 1hmidle.kf
                kItem.m_ucBlendIdx = pkBlendInterp->AddInterpInfo(kItem.m_spInterpolator, 0.0f, cInterpPriority);
                continue;
            }

            auto* extraData = kBlendInterpolatorExtraData::GetOrCreate(target);
            DebugAssert(extraData);
            if (extraData->owner)
                DebugAssert(extraData->owner == m_pkOwner);
            extraData->owner = m_pkOwner;
            auto& extraInterpItem = extraData->GetOrCreateItem(kItem.m_spInterpolator);
            if (!extraInterpItem.isAdditive)
            {
                kItem.m_ucBlendIdx = INVALID_INDEX;
                kItem.m_pkBlendInterp = nullptr;
                DebugAssert(false);
                continue;
            }
            if (extraInterpItem.detached)
            {
                // happens if this sequence was non additive but is now additive
                DebugAssert(extraInterpItem.blendIndex != 0xFF);
                if (extraInterpItem.blendIndex != 0xFF)
                    kItem.m_pkBlendInterp->RemoveInterpInfo(extraInterpItem.blendIndex);
                extraInterpItem.blendIndex = 0xFF;
                extraInterpItem.detached = false;
                extraInterpItem.translateWeight.ClearValues();
                extraInterpItem.rotateWeight.ClearValues();
                extraInterpItem.scaleWeight.ClearValues();
                extraInterpItem.debugState = kInterpDebugState::WasDetachedButNowAdditive;
            }
            kItem.m_ucBlendIdx = kItem.m_pkBlendInterp->AddInterpInfo(
                kItem.m_spInterpolator,
                m_fSeqWeight,
                kItem.m_ucPriority != INVALID_INDEX ? kItem.m_ucPriority : cPriority,
                1.0f);
        }
    }
}

void AdditiveManager::WriteHooks()
{
}