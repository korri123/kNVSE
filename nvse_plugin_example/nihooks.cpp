#include "nihooks.h"

#include "blend_fixes.h"
#include "commands_animation.h"
#include "hooks.h"
#include "NiNodes.h"
#include "SafeWrite.h"
#include "NiObjects.h"
#include "NiTypes.h"

#include <array>
#include <functional>
#include <ranges>

#define BETHESDA_GAMEBRYO_MODIFICATIONS 1

#if _DEBUG
extern std::unordered_map<NiBlendInterpolator*, std::unordered_set<NiControllerSequence*>> g_debugInterpMap;
extern std::unordered_map<NiInterpolator*, const char*> g_debugInterpNames;
extern std::unordered_map<NiInterpolator*, const char*> g_debugInterpSequences;
#endif

bool NiBlendAccumTransformInterpolator::BlendValues(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue)
{
    float fTotalTransWeight = 1.0f;
    float fTotalScaleWeight = 1.0f;

    NiPoint3 kFinalTranslate = NiPoint3::ZERO;
    NiQuaternion kFinalRotate = NiQuaternion(0.0f, 0.0f, 0.0f, 0.0f);
    float fFinalScale = 0.0f;

    bool bTransChanged = false;
    bool bRotChanged = false;
    bool bScaleChanged = false;

    bool bAccumTransformInvalid = m_kAccumulatedTransformValue
        .IsTransformInvalid();
    if (bAccumTransformInvalid)
    {
        m_kAccumulatedTransformValue.SetTranslate(NiPoint3::ZERO);
        m_kAccumulatedTransformValue.SetRotate(NiQuaternion::IDENTITY);
        m_kAccumulatedTransformValue.SetScale(1.0f);
    }

    bool bFirstRotation = true;
    for (unsigned char uc = 0; uc < m_ucArraySize; uc++)
    {
        NiBlendInterpolator::InterpArrayItem& kItem = m_pkInterpArray[uc];
        NiBlendAccumTransformInterpolator::AccumArrayItem& kAccumItem = m_pkAccumArray[uc];
        if (kItem.m_spInterpolator && kItem.m_fNormalizedWeight > 0.0f)
        {
            NiQuatTransform kTransform;
            if (bAccumTransformInvalid)
            {
                kTransform = kAccumItem.m_kLastValue;
            }
            else
            {
                kTransform = kAccumItem.m_kDeltaValue;
            }

            // Add in the current interpolator's weighted
            // translation to the accumulated translation thus far.
            if (kTransform.IsTranslateValid())
            {
                kFinalTranslate += kTransform.GetTranslate() *
                    kItem.m_fNormalizedWeight;
                bTransChanged = true;
            }
            else
            {
                // This translate is invalid, so we need to
                // remove it's overall weight from the result
                // at the end
                fTotalTransWeight -= kItem.m_fNormalizedWeight;
            }

            // Add in the current interpolator's weighted
            // rotation to the accumulated rotation thus far.
            // Since quaternion SLERP is not commutative, we can
            // get away with accumulating weighted sums of the quaternions
            // as long as we re-normalize at the end.
            if (kTransform.IsRotateValid())
            {
                NiQuaternion kRotValue = kTransform.GetRotate();

                // Dot only represents the angle between quats when they
                // are unitized. However, we don't care about the 
                // specific angle. We only care about the sign of the angle
                // between the two quats. This is preserved when
                // quaternions are non-unit.
                if (!bFirstRotation)
                {
                    float fCos = NiQuaternion::Dot(kFinalRotate, kRotValue);

                    // If the angle is negative, we need to invert the
                    // quat to get the best path.
                    if (fCos < 0.0f)
                    {
                        kRotValue = -kRotValue;
                    }
                }
                else
                {
                    bFirstRotation = false;
                }

                // Multiply in the weights to the quaternions.
                // Note that this makes them non-rotations.
                kRotValue = kRotValue * kItem.m_fNormalizedWeight;

                // Accumulate the total weighted values into the 
                // rotation
                kFinalRotate.SetValues(
                    kRotValue.GetW() + kFinalRotate.GetW(),
                    kRotValue.GetX() + kFinalRotate.GetX(),
                    kRotValue.GetY() + kFinalRotate.GetY(),
                    kRotValue.GetZ() + kFinalRotate.GetZ());

                // Need to re-normalize quaternion.
                bRotChanged = true;
            }
            // we don't need to remove the weight of invalid rotations
            // since we are re-normalizing at the end. It's just extra work.

            // Add in the current interpolator's weighted
            // scale to the accumulated scale thus far.
            if (kTransform.IsScaleValid())
            {
                fFinalScale += kTransform.GetScale() *
                    kItem.m_fNormalizedWeight;
                bScaleChanged = true;
            }
            else
            {
                // This scale is invalid, so we need to
                // remove it's overall weight from the result
                // at the end
                fTotalScaleWeight -= kItem.m_fNormalizedWeight;
            }
        }
    }

    // If any of the channels were animated, the final
    // transform needs to be updated
    if (bTransChanged || bRotChanged || bScaleChanged)
    {
        // Since channels may or may not actually have been
        // active during the blend, we can remove the weights for
        // channels that weren't active.
        float fTotalTransWeightInv = 1.0f / fTotalTransWeight;
        float fTotalScaleWeightInv = 1.0f / fTotalScaleWeight;

        NiQuatTransform kFinalTransform;
        if (bTransChanged)
        {
            // Remove the effect of invalid translations from the
            // weighted sum
            kFinalTranslate *= fTotalTransWeightInv;
            kFinalTransform.SetTranslate(kFinalTranslate);
        }
        if (bRotChanged)
        {
            // Since we summed quaternions earlier, we have
            // non-unit quaternions, which are not rotations.
            // To make the accumulated quaternion a rotation, we 
            // need to normalize.
            kFinalRotate.Normalize();
            kFinalTransform.SetRotate(kFinalRotate);
        }
        if (bScaleChanged)
        {
            // Remove the effect of invalid scales from the
            // weighted sum
            fFinalScale *= fTotalScaleWeightInv;
            kFinalTransform.SetScale(fFinalScale);
        }

        m_kAccumulatedTransformValue = m_kAccumulatedTransformValue *
            kFinalTransform;
        if (m_kAccumulatedTransformValue.IsTransformInvalid())
        {
            return false;
        }

        kValue = m_kAccumulatedTransformValue;
        return true;
    }

    return false;
}

void NiMultiTargetTransformController::_Update(float fTime, bool bSelective)
{
    if (!GetActive() || !m_usNumInterps)
        return;
    NiQuatTransform kTransform;
    for (unsigned short us = 0; us < m_usNumInterps; us++)
    {
        auto* pkTarget = m_ppkTargets[us];
        // We need to check the UpdateSelected flag before updating the
        // interpolator. For instance, BoneLOD might have turned off that
        // bone.
        
        if (!pkTarget)
        {
            continue;
        }
        if (bSelective == pkTarget->GetSelectiveUpdate() && us != m_usNumInterps - 1)
        {
            continue;
        }
        if (us == m_usNumInterps - 1 && (!bSelective && !pkTarget->GetSelectiveUpdate()))
        {
            // beth bs
            break;
        }

        auto& kBlendInterp = m_pkBlendInterps[us];
        if (kBlendInterp.Update(fTime, pkTarget, kTransform))
        {
            if (kTransform.IsTranslateValid())
            {
                pkTarget->SetTranslate(kTransform.GetTranslate());
            }
            if (kTransform.IsRotateValid())
            {
                pkTarget->SetRotate(kTransform.GetRotate());
            }
            if (kTransform.IsScaleValid())
            {
                pkTarget->SetScale(kTransform.GetScale());
            }
        }
    }
}


bool NiBlendTransformInterpolator::BlendValues(float fTime, NiObjectNET* pkInterpTarget,
                                               NiQuatTransform& kValue)
{
    float fTotalTransWeight = 1.0f;
    float fTotalScaleWeight = 1.0f;

    NiPoint3 kFinalTranslate = NiPoint3::ZERO;
    NiQuaternion kFinalRotate = NiQuaternion(0.0f, 0.0f, 0.0f, 0.0f);
    float fFinalScale = 0.0f;

    bool bTransChanged = false;
    bool bRotChanged = false;
    bool bScaleChanged = false;

    bool bFirstRotation = true;
    for (unsigned char uc = 0; uc < this->m_ucArraySize; uc++)
    {
        InterpArrayItem& kItem = this->m_pkInterpArray[uc];
        if (kItem.m_spInterpolator && kItem.m_fNormalizedWeight > 0.0f)
        {
            float fUpdateTime = fTime;
            if (!this->GetUpdateTimeForItem(fUpdateTime, kItem))
            {
                fTotalTransWeight -= kItem.m_fNormalizedWeight;
                fTotalScaleWeight -= kItem.m_fNormalizedWeight;
                continue;
            }

            NiQuatTransform kTransform;
            bool bSuccess = kItem.m_spInterpolator.data->Update(fUpdateTime, pkInterpTarget, kTransform);

            if (bSuccess)
            {
                // Add in the current interpolator's weighted
                // translation to the accumulated translation thus far.
                if (kTransform.IsTranslateValid())
                {
                    kFinalTranslate += kTransform.GetTranslate() *
                        kItem.m_fNormalizedWeight;
                    bTransChanged = true;
                }
                else
                {
                    // This translate is invalid, so we need to
                    // remove it's overall weight from the result
                    // at the end
                    fTotalTransWeight -= kItem.m_fNormalizedWeight;
                }

                // Add in the current interpolator's weighted
                // rotation to the accumulated rotation thus far.
                // Since quaternion SLERP is not commutative, we can
                // get away with accumulating weighted sums of the quaternions
                // as long as we re-normalize at the end.
                if (kTransform.IsRotateValid())
                {
                    NiQuaternion kRotValue = kTransform.GetRotate();

                    // Dot only represents the angle between quats when they
                    // are unitized. However, we don't care about the 
                    // specific angle. We only care about the sign of the
                    // angle between the two quats. This is preserved when
                    // quaternions are non-unit.
                    if (!bFirstRotation)
                    {
                        float fCos = NiQuaternion::Dot(kFinalRotate,
                                                       kRotValue);

                        // If the angle is negative, we need to invert the
                        // quat to get the best path.
                        if (fCos < 0.0f)
                        {
                            kRotValue = -kRotValue;
                        }
                    }
                    else
                    {
                        bFirstRotation = false;
                    }

                    // Multiply in the weights to the quaternions.
                    // Note that this makes them non-rotations.
                    kRotValue = kRotValue * kItem.m_fNormalizedWeight;

                    // Accumulate the total weighted values into the 
                    // rotation
                    kFinalRotate.SetValues(
                        kRotValue.GetW() + kFinalRotate.GetW(),
                        kRotValue.GetX() + kFinalRotate.GetX(),
                        kRotValue.GetY() + kFinalRotate.GetY(),
                        kRotValue.GetZ() + kFinalRotate.GetZ());

                    // Need to re-normalize quaternion.
                    bRotChanged = true;
                }
                // we don't need to remove the weight of invalid rotations
                // since we are re-normalizing at the end. It's just extra
                // work.

                // Add in the current interpolator's weighted
                // scale to the accumulated scale thus far.
                if (kTransform.IsScaleValid())
                {
                    fFinalScale += kTransform.GetScale() *
                        kItem.m_fNormalizedWeight;
                    bScaleChanged = true;
                }
                else
                {
                    // This scale is invalid, so we need to
                    // remove it's overall weight from the result
                    // at the end
                    fTotalScaleWeight -= kItem.m_fNormalizedWeight;
                }
            }
            else
            {
                // If the update failed, we should 
                // remove the weights of the interpolator
                fTotalTransWeight -= kItem.m_fNormalizedWeight;
                fTotalScaleWeight -= kItem.m_fNormalizedWeight;
            }
        }
    }

    // If any of the channels were animated, the final
    // transform needs to be updated
    kValue.MakeInvalid();
    if (bTransChanged || bRotChanged || bScaleChanged)
    {
        // Since channels may or may not actually have been
        // active during the blend, we can remove the weights for
        // channels that weren't active.

        if (bTransChanged && fTotalTransWeight != 0.0f)
        {
            // Remove the effect of invalid translations from the
            // weighted sum
            NIASSERT(fTotalTransWeight != 0.0f);
            kFinalTranslate /= fTotalTransWeight;
            kValue.SetTranslate(kFinalTranslate);
        }
        if (bRotChanged)
        {
            // Since we summed quaternions earlier, we have
            // non-unit quaternions, which are not rotations.
            // To make the accumulated quaternion a rotation, we 
            // need to normalize.
            kFinalRotate.Normalize();
            kValue.SetRotate(kFinalRotate);
        }
        if (bScaleChanged && fTotalScaleWeight >= 0.0001)
        {
            // Remove the effect of invalid scales from the
            // weighted sum
            NIASSERT(fTotalScaleWeight != 0.0f);
            fFinalScale /= fTotalScaleWeight;
            kValue.SetScale(fFinalScale);
        }
    }

    if (kValue.IsTransformInvalid())
    {
        return false;
    }

    return true;
}

bool NiBlendTransformInterpolator::_Update(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue)
{
    // Do not use the TimeHasChanged check here, because blend interpolators
    // should always update their interpolators.

    bool bReturnValue = false;
    if (m_ucInterpCount == 1)
    {
        bReturnValue = StoreSingleValue(fTime, pkInterpTarget, kValue);
    }
    else if (m_ucInterpCount > 0)
    {
        ComputeNormalizedWeights();
        bReturnValue = BlendValues(fTime, pkInterpTarget, kValue);
    }
    
    m_fLastTime = fTime;
    return bReturnValue;
}

// underscore needed since vtable
bool NiTransformInterpolator::_Update(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue)
{
    if (bPose)
    {
        m_fLastTime = fTime;
        kValue = m_kTransformValue;
        return true;
    }

    if (!TimeHasChanged(fTime))
    {
        kValue = m_kTransformValue;

        if (m_kTransformValue.IsTransformInvalid())
            return false;
        return true;
    }

    // Compute translation value.
    unsigned int uiNumKeys;
    NiAnimationKey::KeyType eTransType;
    unsigned char ucSize;
    NiPosKey* pkTransKeys = GetPosData(uiNumKeys, eTransType, ucSize);
    if (uiNumKeys > 0)
    {
        m_kTransformValue.SetTranslate(NiPosKey::GenInterp(fTime, pkTransKeys,
                                                           eTransType, uiNumKeys, m_usLastTransIdx,
                                                           ucSize));
    }

    // Compute rotation value.
    NiAnimationKey::KeyType eRotType;
    NiRotKey* pkRotKeys = GetRotData(uiNumKeys, eRotType, ucSize);
    if (uiNumKeys > 0)
    {
        //NiQuaternion quat;
        //UInt32 uiLastRotIdx = m_usLastRotIdx;
        //CdeclCall(0xA28740, &quat, fTime, pkRotKeys, eRotType, uiNumKeys, &uiLastRotIdx, ucSize);
        //m_kTransformValue.SetRotate(quat);
        //m_usLastRotIdx = uiLastRotIdx;
        // Thanks Bethesda
        m_kTransformValue.SetRotate(NiRotKey::GenInterp(fTime, pkRotKeys,
                                                        eRotType, uiNumKeys, m_usLastRotIdx, ucSize));
    }

    // Compute scale value.
    NiFloatKey::KeyType eScaleType;
    NiFloatKey* pkScaleKeys = GetScaleData(uiNumKeys, eScaleType, ucSize);
    if (uiNumKeys > 0)
    {
        m_kTransformValue.SetScale(NiFloatKey::GenInterp(fTime, pkScaleKeys,
                                                         eScaleType, uiNumKeys, m_usLastScaleIdx,
                                                         ucSize));
    }

    kValue = m_kTransformValue;
    if (m_kTransformValue.IsTransformInvalid())
    {
        return false;
    }

    m_fLastTime = fTime;
    return true;
}

void NiBlendInterpolator::ComputeNormalizedWeights()
{
    if (!GetComputeNormalizedWeights())
    {
        return;
    }

    SetComputeNormalizedWeights(false);

    if (m_ucInterpCount == 1)
    {
        m_pkInterpArray[m_ucSingleIdx].m_fNormalizedWeight = 1.0f;
        return;
    }
    if (m_ucInterpCount == 2)
    {
        ThisStdCall(0xA36BD0, this); // NiBlendInterpolator::ComputeNormalizedWeightsFor2
        return;
    }

    unsigned char uc;

    if (m_fHighSumOfWeights == -NI_INFINITY)
    {
        // Compute sum of weights for highest and next highest priorities,
        // along with highest ease spinner for the highest priority.
        m_fHighSumOfWeights = 0.0f;
        m_fNextHighSumOfWeights = 0.0f;
        m_fHighEaseSpinner = 0.0f;
        for (uc = 0; uc < m_ucArraySize; uc++)
        {
            auto& kItem = m_pkInterpArray[uc];
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


    float fOneMinusHighEaseSpinner = 1.0f - m_fHighEaseSpinner;
    float fTotalSumOfWeights = m_fHighEaseSpinner * m_fHighSumOfWeights +
        fOneMinusHighEaseSpinner * m_fNextHighSumOfWeights;
    float fOneOverTotalSumOfWeights =
        (fTotalSumOfWeights > 0.0f) ? (1.0f / fTotalSumOfWeights) : 0.0f;


    // Compute normalized weights.
    for (uc = 0; uc < m_ucArraySize; uc++)
    {
        auto& kItem = m_pkInterpArray[uc];
        if (kItem.m_spInterpolator != NULL)
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
        for (uc = 0; uc < m_ucArraySize; uc++)
        {
            auto& kItem = m_pkInterpArray[uc];
            if (kItem.m_spInterpolator != NULL &&
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

        for (uc = 0; uc < m_ucArraySize; uc++)
        {
            auto& kItem = m_pkInterpArray[uc];
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
        for (uc = 0; uc < m_ucArraySize; uc++)
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

bool NiControllerSequence::Activate(char cPriority, bool bStartOver, float fWeight, float fEaseInTime,
    NiControllerSequence* pkTimeSyncSeq, bool bTransition)
{
    return ThisStdCall<bool>(0xA34F20, this, cPriority, bStartOver, fWeight, fEaseInTime, pkTimeSyncSeq, bTransition);
}

bool NiControllerSequence::StartBlend(NiControllerSequence* pkDestSequence, float fDuration, float fDestFrame,
    int iPriority, float fSourceWeight, float fDestWeight, NiControllerSequence* pkTimeSyncSeq)
{
    return ThisStdCall<bool>(0xA350D0, this, pkDestSequence, fDuration, fDestFrame, iPriority, fSourceWeight, fDestWeight, pkTimeSyncSeq);
}

bool NiControllerSequence::StartMorph(NiControllerSequence* pkDestSequence, float fDuration, int iPriority,
    float fSourceWeight, float fDestWeight)
{
    return ThisStdCall<bool>(0xA351D0, this, pkDestSequence, fDuration, iPriority, fSourceWeight, fDestWeight);
}

bool NiControllerSequence::VerifyDependencies(NiControllerSequence* pkSequence) const
{
    return ThisStdCall<bool>(0xA30580, this, pkSequence);
}

bool NiControllerSequence::VerifyMatchingMorphKeys(NiControllerSequence* pkTimeSyncSeq) const
{
    return ThisStdCall<bool>(0xA30AB0, this, pkTimeSyncSeq);
}

bool NiControllerSequence::CanSyncTo(NiControllerSequence* pkTargetSequence) const
{
    if (!pkTargetSequence)
        return false;

#if BETHESDA_GAMEBRYO_MODIFICATIONS
    // Bethesda
    auto* pkPartnerSequence = pkTargetSequence->m_pkPartnerSequence;
    if ((!pkPartnerSequence || pkPartnerSequence != this && VerifyDependencies(pkTargetSequence))
        && VerifyMatchingMorphKeys(pkTargetSequence))
    {
        return true;
    }
    return false;
#else
    // Gamebryo
    if (!VerifyDependencies(pkTargetSequence) ||
        !VerifyMatchingMorphKeys(pkTargetSequence))
    {
        return false;
    }
    return true;
#endif
}

bool NiControllerManager::BlendFromPose(NiControllerSequence* pkSequence, float fDestFrame, float fDuration,
                                        int iPriority, NiControllerSequence* pkSequenceToSynchronize)
{
    NIASSERT(pkSequence && pkSequence->GetOwner() == this &&
        (!pkSequenceToSynchronize || pkSequenceToSynchronize->GetOwner() ==
            this));

    NiControllerSequence* pkTempSequence = CreateTempBlendSequence(pkSequence,
        pkSequenceToSynchronize);
    return pkTempSequence->StartBlend(pkSequence, fDuration, fDestFrame,
        iPriority, 1.0f, 1.0f, nullptr);
}

bool NiControllerManager::CrossFade(NiControllerSequence* pkSourceSequence, NiControllerSequence* pkDestSequence,
    float fDuration, int iPriority, bool bStartOver, float fWeight, NiControllerSequence* pkTimeSyncSeq)
{
    NIASSERT(pkSourceSequence && pkSourceSequence->GetOwner() == this);
    NIASSERT(pkDestSequence && pkDestSequence->GetOwner() == this);

    if (pkSourceSequence->GetState() == NiControllerSequence::INACTIVE ||
        pkDestSequence->GetState() != NiControllerSequence::INACTIVE)
    {
        return false;
    }

    pkSourceSequence->Deactivate(fDuration, false);
    return pkDestSequence->Activate(iPriority, bStartOver, fWeight, fDuration,
        pkTimeSyncSeq, false);
}

NiControllerSequence* NiControllerManager::CreateTempBlendSequence(NiControllerSequence* pkSequence,
    NiControllerSequence* pkSequenceToSynchronize)
{
    return ThisStdCall<NiControllerSequence*>(0xA2F170, this, pkSequence, pkSequenceToSynchronize);
}

bool NiControllerManager::Morph(NiControllerSequence* pkSourceSequence, NiControllerSequence* pkDestSequence,
    float fDuration, int iPriority, float fSourceWeight, float fDestWeight)
{
    NIASSERT(pkSourceSequence && pkSourceSequence->GetOwner() == this &&
        pkDestSequence && pkDestSequence->GetOwner() == this);

    return pkSourceSequence->StartMorph(pkDestSequence, fDuration, iPriority,
        fSourceWeight, fDestWeight);
}

namespace NiHooks
{
    void WriteHooks()
    {
        WriteRelJump(0xA40C10, &NiBlendTransformInterpolator::BlendValues);
        WriteRelJump(0xA41110, &NiBlendTransformInterpolator::_Update);
        WriteRelJump(0xA3FDB0, &NiTransformInterpolator::_Update);
        WriteRelJump(0xA37260, &NiBlendInterpolator::ComputeNormalizedWeights);
        WriteRelJump(0xA39960, &NiBlendAccumTransformInterpolator::BlendValues);
        WriteRelJump(0x4F0380, &NiMultiTargetTransformController::_Update);
        WriteRelJump(0xA2F800, &NiControllerManager::BlendFromPose);
        WriteRelJump(0xA2E280, &NiControllerManager::CrossFade);
        WriteRelJump(0xA2E1B0, &NiControllerManager::Morph);
        WriteRelJump(0xA30C80, &NiControllerSequence::CanSyncTo);
    }

    // override stewie's tweaks
    void WriteDelayedHooks()
    {
#if _DEBUG
        WriteRelCall(0x4F0087, &NiMultiTargetTransformController::_Update);
#endif
    }
}

std::unordered_map<NiInterpolator*, NiControllerSequence*> g_interpsToSequenceMap;

void __fastcall NiControllerSequence_AttachInterpolatorsHook(NiControllerSequence* _this, void*,  char cPriority)
{
    std::span interpItems(_this->controlledBlocks, _this->numControlledBlocks);
    for (auto& interp : interpItems)
    {
        g_interpsToSequenceMap[interp.interpolator] = _this;
    }
    ThisStdCall(0xA30900, _this, cPriority);
}

void __fastcall NiControllerSequence_DetachInterpolatorsHook(NiControllerSequence* _this, void*)
{
    ThisStdCall(0xA30540, _this);
    std::span interpItems(_this->controlledBlocks, _this->numControlledBlocks);
    for (auto& interp : interpItems)
    {
        g_interpsToSequenceMap.erase(interp.interpolator);
    }
}

std::unordered_set<NiControllerSequence*> g_appliedDestFrameAnims;

void ApplyDestFrame(NiControllerSequence* sequence, float destFrame)
{
    sequence->destFrame = destFrame;
    g_appliedDestFrameAnims.insert(sequence);
}

bool IsTempBlendSequence(const NiControllerSequence* sequence)
{
    return strncmp("__", sequence->sequenceName, 2) == 0;
}

void ClearTempBlendSequence(NiControllerSequence* sequence)
{
    if (auto it = g_tempBlendSequences.find(sequence->owner); it != g_tempBlendSequences.end())
    {
        int index = 0;
        auto& tempBlendSeqs = it->second;
        for (auto* tempBlendSeq : tempBlendSeqs)
        {
            if (tempBlendSeq == sequence)
            {
                tempBlendSeqs[index] = nullptr;
            }
            ++index;
        }
    }
}

void __fastcall NiControllerSequence_ApplyDestFrameHook(NiControllerSequence* sequence, void*, float fTime, bool bUpdateInterpolators)
{
    if (sequence->state != kAnimState_Inactive && sequence->destFrame != -FLT_MAX)
    {
        if (auto iter = g_appliedDestFrameAnims.find(sequence); iter != g_appliedDestFrameAnims.end())
        {
            g_appliedDestFrameAnims.erase(iter);
            if (sequence->offset == -FLT_MAX || sequence->state == kAnimState_TransDest)
            {
                sequence->offset = -fTime + sequence->destFrame;
            }

            if (sequence->startTime == -FLT_MAX)
            {
                const float easeTime = sequence->endTime;
                sequence->endTime = fTime + easeTime - sequence->destFrame;
                sequence->startTime = fTime - sequence->destFrame;
            }
            sequence->destFrame = -FLT_MAX; // end my suffering
#if _DEBUG
            float fEaseSpinnerIn = (fTime - sequence->startTime) / (sequence->endTime - sequence->startTime);
            float fEaseSpinnerOut = (sequence->endTime - fTime) / (sequence->endTime - sequence->startTime);
            int i = 0;
#endif
        }
		
    }
    ThisStdCall(0xA34BA0, sequence, fTime, bUpdateInterpolators);
    if (IsTempBlendSequence(sequence) && sequence->state == kAnimState_Inactive)
    {
        ClearTempBlendSequence(sequence);
    }
}

std::string GetLastSubstringAfterSlash(const std::string& str)
{
    const auto pos = str.find_last_of('\\');
    if (pos == std::string::npos)
        return str;
    return str.substr(pos + 1);
}

std::unordered_map<NiControllerManager*, NiControllerSequence*> g_lastTempBlendSequence;

#define EXPERIMENTAL_HOOKS 0

NiControllerSequence* __fastcall TempBlendDebugHook(NiControllerManager* manager, void*, NiControllerSequence* source, NiControllerSequence* timeSync)
{
    auto* tempBlendSeq = ThisStdCall<NiControllerSequence*>(0xA2F170, manager, source, timeSync);
    const auto& str = "__TMP_BLEND_" + GetLastSubstringAfterSlash(source->sequenceName);
    tempBlendSeq->sequenceName = GameFuncs::BSFixedString_CreateFromPool(str.c_str());
    g_lastTempBlendSequence[manager] = tempBlendSeq;
    return tempBlendSeq;
}

void ApplyNiHooks()
{
#if _DEBUG
    NiHooks::WriteHooks();
#endif
#if EXPERIMENTAL_HOOKS

    if (g_fixBlendSamePriority)
    {
        //WriteRelJump(0xA37260, NiBlendInterpolator_ComputeNormalizedWeights);
        WriteRelCall(0xA34F71, NiControllerSequence_AttachInterpolatorsHook);
        WriteRelCall(0xA350C5, NiControllerSequence_DetachInterpolatorsHook);
        //WriteRelJump(0xA2E280, CrossFadeHook);
    }

    WriteRelCall(0xA2E251, NiControllerSequence_ApplyDestFrameHook);


#if _DEBUG
    WriteRelCall(0xA2F817, TempBlendDebugHook);
    SafeWriteBuf(0xA35093, "\xEB\x15\x90", 3);
#endif
#endif
}