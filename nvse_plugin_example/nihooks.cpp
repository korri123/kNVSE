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

#define BETHESDA_MODIFICATIONS 1
#define NI_OVERRIDE 0

void NiOutputDebugString(const char* text)
{
    ERROR_LOG(text);
}

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
                NIASSERT(fFinalScale < 1000.0f && fFinalScale > -1000.0f);
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
            NIASSERT(fFinalScale < 1000.0f && fFinalScale > -1000.0f);
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
    
    for (unsigned short us = 0; us < m_usNumInterps; us++)
    {
        NiQuatTransform kTransform;
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

#define ADDITIVE_ANIMS 0

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
        float fNormalizedWeight = kItem.m_fNormalizedWeight;
        if (kItem.m_spInterpolator && (fNormalizedWeight > 0.0f))
        {
            float fUpdateTime = fTime;
            if (!this->GetUpdateTimeForItem(fUpdateTime, kItem))
            {
                fTotalTransWeight -= fNormalizedWeight;
                fTotalScaleWeight -= fNormalizedWeight;
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
                        fNormalizedWeight;
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
                    kRotValue = kRotValue * fNormalizedWeight;

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
                        fNormalizedWeight;
                    bScaleChanged = true;
                    NIASSERT(fFinalScale < 1000.0f && fFinalScale > -1000.0f);
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
                fTotalTransWeight -= fNormalizedWeight;
                fTotalScaleWeight -= fNormalizedWeight;
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

bool NiBlendTransformInterpolator::BlendValuesFixFloatingPointError(float fTime, NiObjectNET* pkInterpTarget,
    NiQuatTransform& kValue)
{
    // Accumulate weights positively instead of subtracting
    double dTotalTransWeight = 0.0;
    double dTotalScaleWeight = 0.0;

    NiPoint3 kFinalTranslate = NiPoint3::ZERO;
    NiQuaternion kFinalRotate = NiQuaternion(0.0f, 0.0f, 0.0f, 0.0f);
    float fFinalScale = 0.0f;

    bool bTransChanged = false;
    bool bRotChanged = false;
    bool bScaleChanged = false;

    bool bFirstRotation = true;
    for (unsigned char uc = 0; uc < m_ucArraySize; uc++)
    {
        InterpArrayItem& kItem = m_pkInterpArray[uc];
        if (kItem.m_spInterpolator && kItem.m_fNormalizedWeight > 0.0f)
        {
            float fUpdateTime = fTime;
            if (!GetUpdateTimeForItem(fUpdateTime, kItem))
            {
                continue;
            }

            NiQuatTransform kTransform;
            bool bSuccess = kItem.m_spInterpolator->Update(fUpdateTime, 
                pkInterpTarget, kTransform);

            if (bSuccess)
            {
                // Use double for weight calculations
                double dWeight = static_cast<double>(kItem.m_fNormalizedWeight);
                
                if (kTransform.IsTranslateValid())
                {
                    kFinalTranslate += kTransform.GetTranslate() *
                        kItem.m_fNormalizedWeight;
                    dTotalTransWeight += dWeight;
                    bTransChanged = true;
                }

                if (kTransform.IsRotateValid())
                {
                    NiQuaternion kRotValue = kTransform.GetRotate();

                    if (!bFirstRotation)
                    {
                        float fCos = NiQuaternion::Dot(kFinalRotate,
                            kRotValue);

                        if (fCos < 0.0f)
                        {
                            kRotValue = -kRotValue;
                        }
                    }
                    else
                    {
                        bFirstRotation = false;
                    }

                    kRotValue = kRotValue * kItem.m_fNormalizedWeight;

                    kFinalRotate.SetValues(
                        kRotValue.GetW() + kFinalRotate.GetW(), 
                        kRotValue.GetX() + kFinalRotate.GetX(), 
                        kRotValue.GetY() + kFinalRotate.GetY(),
                        kRotValue.GetZ() + kFinalRotate.GetZ());

                    bRotChanged = true;
                }

                if (kTransform.IsScaleValid())
                {
                    fFinalScale += kTransform.GetScale() *
                        kItem.m_fNormalizedWeight;
                    dTotalScaleWeight += dWeight;
                    bScaleChanged = true;
                }
            }
        }
    }

    kValue.MakeInvalid();
    if (bTransChanged || bRotChanged || bScaleChanged)
    {
        constexpr double EPSILON = 1e-10;
        // Use epsilon checks for safer divisions
        if (bTransChanged && dTotalTransWeight > EPSILON)
        {
            // Convert back to float for the division
            float fInverseWeight = static_cast<float>(1.0 / dTotalTransWeight);
            kFinalTranslate *= fInverseWeight;  // Use multiplication instead of division
            kValue.SetTranslate(kFinalTranslate);
        }
        
        if (bRotChanged)
        {
            // Quaternion normalization handles the weighting implicitly
            kFinalRotate.Normalize();
            kValue.SetRotate(kFinalRotate);
        }
        
        if (bScaleChanged && dTotalScaleWeight > EPSILON)
        {
            // Convert back to float for the division
            float fInverseWeight = static_cast<float>(1.0 / dTotalScaleWeight);
            fFinalScale *= fInverseWeight;  // Use multiplication instead of division
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
#if ADDITIVE_ANIMS
    const auto& kAdditiveManager = AdditiveSequences::Get();
#endif
    if (m_ucInterpCount == 2)
    {
#if ADDITIVE_ANIMS
        const bool bIsFirstAdditive = kAdditiveManager.IsAdditiveInterpolator(m_pkInterpArray[0].m_spInterpolator);
        const bool bIsSecondAdditive = kAdditiveManager.IsAdditiveInterpolator(m_pkInterpArray[1].m_spInterpolator);
        if (bIsFirstAdditive && !bIsSecondAdditive)
            m_pkInterpArray[1].m_fNormalizedWeight = 1.0f;
        if (bIsSecondAdditive && !bIsFirstAdditive)
            m_pkInterpArray[0].m_fNormalizedWeight = 1.0f;
        if (bIsFirstAdditive || bIsSecondAdditive)
            return;
#endif
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
#if ADDITIVE_ANIMS
                if (kAdditiveManager.IsAdditiveInterpolator(kItem.m_spInterpolator))
                    continue;
#endif
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
#if ADDITIVE_ANIMS
            if (kAdditiveManager.IsAdditiveInterpolator(kItem.m_spInterpolator))
                continue;
#endif
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
#if ADDITIVE_ANIMS
                if (kAdditiveManager.IsAdditiveInterpolator(kItem.m_spInterpolator))
                    continue;
#endif
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
#if ADDITIVE_ANIMS
            if (kAdditiveManager.IsAdditiveInterpolator(kItem.m_spInterpolator))
                continue;
#endif
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
#if ADDITIVE_ANIMS
            if (kAdditiveManager.IsAdditiveInterpolator(m_pkInterpArray[uc].m_spInterpolator))
                continue;
#endif
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

void NiControllerSequence::RemoveInterpolator(const NiFixedString& name) const
{
    const auto idTags = GetIDTags();
    const auto it = ra::find_if(idTags, [&](const auto& idTag)
    {
        return idTag.m_kAVObjectName == name;
    });
    if (it != idTags.end())
    {
        RemoveInterpolator(it - idTags.begin());
    }
}

void NiControllerSequence::RemoveInterpolator(unsigned int index) const
{
    m_pkInterpArray[index].ClearValues();
    m_pkIDTagArray[index].ClearValues();
}

float NiControllerSequence::GetEaseSpinner() const
{
    if (m_uiArraySize == 0)
        return 0.0f;
    auto interpArrayItems = GetControlledBlocks();
    const auto itemIter = std::ranges::find_if(interpArrayItems, [](const InterpArrayItem& item)
    {
        return item.m_spInterpolator && item.m_pkBlendInterp;
    });
    if (itemIter == interpArrayItems.end())
        return 0.0f;
    auto blendInterpItems = itemIter->m_pkBlendInterp->GetItems();
    const auto blendItemIter = std::ranges::find_if(blendInterpItems, [&](const NiBlendInterpolator::InterpArrayItem& blendItem)
    {
        return blendItem.m_spInterpolator == itemIter->m_spInterpolator;
    });
    if (blendItemIter == blendInterpItems.end())
        return 0.0f;
    return blendItemIter->m_fEaseSpinner;
}

bool NiControllerSequence::PopulateIDTags(NiControllerSequence* source)
{
    if (m_uiArraySize != source->m_uiArraySize)
        return false;

    for (unsigned int i = 0; i < m_uiArraySize; i++)
    {
        m_pkIDTagArray[i] = source->m_pkIDTagArray[i];
    }
    return true;
}

void NiControllerSequence::AttachInterpolators(char cPriority)
{
#if !_DEBUG || !NI_OVERRIDE
    ThisStdCall(0xA30900, this, cPriority);
#else
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem &kItem = m_pkInterpArray[ui];
        if (kItem.m_spInterpolator != NULL)
        {
            // kItem.m_pkBlendInterp may be NULL if the interpolator was
            // not successfully attached.
            if (kItem.m_pkBlendInterp != NULL)
            {
                const unsigned int cInterpPriority = kItem.m_ucPriority != 0xFF ? kItem.m_ucPriority : cPriority;
                kItem.m_ucBlendIdx = kItem.m_pkBlendInterp->AddInterpInfo(
                    kItem.m_spInterpolator, 0.0f, cInterpPriority);
                assert(kItem.m_ucBlendIdx != INVALID_INDEX);
            }
        }
    }
#endif
}

UInt32 g_lastActivateCall = 0;

bool NiControllerSequence::Activate(char cPriority, bool bStartOver, float fWeight, float fEaseInTime,
                                    NiControllerSequence* pkTimeSyncSeq, bool bTransition)
{
#if !_DEBUG || !NI_OVERRIDE
    return ThisStdCall<bool>(0xA34F20, this, cPriority, bStartOver, fWeight, fEaseInTime, pkTimeSyncSeq, bTransition);
#else
    if (g_thePlayer && g_thePlayer->baseProcess && g_thePlayer->baseProcess->animData && g_thePlayer->baseProcess->animData->controllerManager)
        if (m_pkOwner == g_thePlayer->baseProcess->animData->controllerManager)
        {
            auto name = sv::get_file_name(m_kName.CStr());
            name = name.empty() ? m_kName.CStr() : name;
            ++g_lastActivateCall;
            Console_Print("Activate %.1f %s %d", fEaseInTime, name.data(), g_lastActivateCall);
        }
    // return ThisStdCall(0xA34F20, this, cPriority, bStartOver, fWeight, fEaseInTime, pkTimeSyncSeq, bTransition);
    assert(m_pkOwner);

    if (m_eState != INACTIVE)
    {
        NiOutputDebugString("Attempting to activate a sequence that is "
                            "already animating!\n");
        return false;
    }

#if BETHESDA_MODIFICATIONS
    auto* pkActiveSequences = &m_pkOwner->m_kActiveSequences;
    auto* thisPtr = this;
    ThisStdCall(0xE7EC00, pkActiveSequences, &thisPtr); // NiTPool::ReleaseObject
#endif

    m_pkPartnerSequence = NULL;
    if (pkTimeSyncSeq)
    {
#if BETHESDA_MODIFICATIONS
        auto* pkMutualPartner = pkTimeSyncSeq->m_pkPartnerSequence;
        if (pkMutualPartner && (pkMutualPartner == this || !VerifyDependencies(pkMutualPartner)))
        {
            return false;
        }
        m_pkPartnerSequence = pkTimeSyncSeq;
#else
        if (!VerifyDependencies(pkTimeSyncSeq) ||
            !VerifyMatchingMorphKeys(pkTimeSyncSeq))
        {
            return false;
        }
        m_pkPartnerSequence = pkTimeSyncSeq;
#endif
    }

    // Set parameters.
    m_fSeqWeight = fWeight;
    
    // Attach the interpolators to their blend interpolators.
    AttachInterpolators(cPriority);

    if (fEaseInTime > 0.0f)
    {
        if (bTransition)
        {
            m_eState = TRANSDEST;
        }
        else
        {
            m_eState = EASEIN;
        }
        m_fStartTime = -NI_INFINITY;
        m_fEndTime = fEaseInTime;
    }
    else
    {
#if BETHESDA_MODIFICATIONS
        m_eState = ANIMATING;
        m_fStartTime = 0.0f;
#else
        m_eState = ANIMATING;
#endif
    }

    if (bStartOver)
    {
        ResetSequence();
    }

    m_fLastTime = -NI_INFINITY;
    return true;
#endif
}

bool NiControllerSequence::ActivateNoReset(float fEaseInTime)
{
    if (m_eState != EASEOUT || fEaseInTime == 0.0f)
    {
#if _DEBUG
        DebugBreak();
#endif
        return false;
    }
    
    const auto fTime = m_fLastTime - m_fOffset;
    const auto fCurrentEaseTime = m_fEndTime - m_fStartTime;
    const auto fEaseOutProgress = (fTime - m_fStartTime) / fCurrentEaseTime;
            
    // Calculate current animation level (1.0 at start of EASEOUT, 0.0 at end)
    const float fCurrentAnimLevel = 1.0f - fEaseOutProgress;
            
    // Only apply special handling if we're partially through the ease-out
    if (fCurrentAnimLevel > 0.0f && fCurrentAnimLevel < 1.0f)
    {
        // Set timing for EASEIN to start from the current animation level
        // Using fEaseInTime instead of fCurrentEaseTime
        m_fEndTime = fTime + fEaseInTime * (1.0f - fCurrentAnimLevel);
        m_fStartTime = fTime - fEaseInTime * fCurrentAnimLevel;
    }
    else
    {
#if _DEBUG
        DebugBreak();
#endif
        return false;
    }
        
    m_eState = EASEIN;
    m_fLastTime = -NI_INFINITY;
    return true;
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

#if BETHESDA_MODIFICATIONS
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

void NiControllerSequence::SetTimePassed(float fTime, bool bUpdateInterpolators)
{
    ThisStdCall(0xA328B0, this, fTime, bUpdateInterpolators);
}

void NiControllerSequence::Update(float fTime, bool bUpdateInterpolators)
{
#if 1
    ThisStdCall(0xA34BA0, this, fTime, bUpdateInterpolators);
#else
    if (m_eState == INACTIVE)
    {
        return;
    }

    if (m_fOffset == -NI_INFINITY)
    {
        m_fOffset = -fTime;
    }

    if (m_fStartTime == -NI_INFINITY)
    {
        m_fStartTime = fTime;
        m_fEndTime = fTime + m_fEndTime;
    }

    float fEaseSpinner = 1.0f;
    float fTransSpinner = 1.0f;
    switch (m_eState)
    {
    case EASEIN:
        if (fTime < m_fEndTime)
        {
            assert(fTime >= m_fStartTime && m_fEndTime != m_fStartTime);
            fEaseSpinner = (fTime - m_fStartTime) / (m_fEndTime -
                                                     m_fStartTime);
        }
        else
        {
            m_eState = ANIMATING;
        }
        break;
    case TRANSDEST:
        if (fTime < m_fEndTime)
        {
            assert(fTime >= m_fStartTime && m_fEndTime != m_fStartTime);
            fTransSpinner = (fTime - m_fStartTime) / (m_fEndTime -
                                                      m_fStartTime);
        }
        else
        {
            if (m_fDestFrame != -NI_INFINITY)
            {
                // This case is hit when we were blending in this
                // sequence. In this case, we need to reset the sequence
                // offset and clear the destination frame.
                m_fOffset = -fTime + m_fDestFrame;
                m_fDestFrame = -NI_INFINITY;
            }
            m_eState = ANIMATING;
        }
        break;
    case EASEOUT:
        if (fTime < m_fEndTime)
        {
            assert(fTime >= m_fStartTime && m_fEndTime != m_fStartTime);
            fEaseSpinner = (m_fEndTime - fTime) / (m_fEndTime -
                                                   m_fStartTime);
        }
        else
        {
            Deactivate(0.0f, false);
            return;
        }
        break;
    case MORPHSOURCE:
    {
        assert(m_pkPartnerSequence);

        // Compute initial offset for partner sequence, undoing phase
        // and frequency adjustments. This assumes the phase and
        // frequency will not change between now and the end time of
        // the morph.
        float fStartFrame = m_pkPartnerSequence->FindCorrespondingMorphFrame(this, m_fOffset + fTime);
        fStartFrame /= m_pkPartnerSequence->m_fFrequency;
        m_pkPartnerSequence->m_fOffset = fStartFrame - fTime;

        // Change sequence state appropriately.
        m_eState = TRANSSOURCE;

        // This case statement intentionally does not break. The code
        // for the TRANSSOURCE case should be subsequently run.
    }
    case TRANSSOURCE:
        if (fTime < m_fEndTime)
        {
            assert(fTime >= m_fStartTime && m_fEndTime != m_fStartTime);
            fTransSpinner = (m_fEndTime - fTime) / (m_fEndTime -
                                                    m_fStartTime);
        }
        else
        {
            Deactivate(0.0f, true);
            return;
        }
        break;
    default:
        // Is there something better we can do here?
        // possible cases are INACTIVE and ANIMATING
        break;
    }

    if (bUpdateInterpolators)
    {
        float fUpdateTime;
        if (m_fDestFrame != -NI_INFINITY)
        {
            fUpdateTime = m_fDestFrame;
        }
        else if (m_pkPartnerSequence)
        {
            if (m_pkPartnerSequence->GetLastTime() !=
                m_pkPartnerSequence->m_fOffset + fTime)
            {
                m_pkPartnerSequence->Update(fTime, false);
            }

            fUpdateTime = FindCorrespondingMorphFrame(m_pkPartnerSequence,
                                                      m_pkPartnerSequence->m_fOffset + fTime);
            fUpdateTime /= m_fFrequency;
        }
        else
        {
            fUpdateTime = m_fOffset + fTime;
        }

        SetInterpsWeightAndTime(m_fSeqWeight * fTransSpinner, fEaseSpinner,
                                ComputeScaledTime(fUpdateTime, true));
    }
#endif
}

void NiControllerSequence::RemoveSingleInterps() const
{
    int i = 0;
    for (auto& interp : GetControlledBlocks())
    {
        if (interp.m_spInterpolator && interp.m_pkBlendInterp && interp.m_pkBlendInterp->m_ucInterpCount != 0)
        {
            RemoveInterpolator(i);
        }
        ++i;
    }
}

bool NiControllerSequence::StoreTargets(NiAVObject* pkRoot)
{
    return ThisStdCall<bool>(0xA32C70, this, pkRoot);
}

float NiControllerSequence::FindCorrespondingMorphFrame(NiControllerSequence* pkTargetSequence, float fTime) const
{
    return ThisStdCall<float>(0xA32010, this, pkTargetSequence, fTime);
}

void NiControllerSequence::SetInterpsWeightAndTime(float fWeight, float fEaseSpinner, float fTime)
{
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem &kItem = m_pkInterpArray[ui];
        if (kItem.m_pkBlendInterp)
        {
            kItem.m_pkBlendInterp->SetWeight(fWeight, kItem.m_ucBlendIdx);
            kItem.m_pkBlendInterp->SetEaseSpinner(fEaseSpinner,
                                                  kItem.m_ucBlendIdx);
            kItem.m_pkBlendInterp->SetTime(fTime, kItem.m_ucBlendIdx);
        }
    }
}

float BSAnimGroupSequence::GetEaseInTime() const
{
    return GetDefaultBlendTime(this, nullptr);
}

float BSAnimGroupSequence::GetEaseOutTime() const
{
    return GetDefaultBlendOutTime(this);
}

NiAVObject* NiInterpController::GetTargetNode(const NiControllerSequence::IDTag& idTag) const
{
    const auto index = GetInterpolatorIndexByIDTag(&idTag);
    if (IS_TYPE(this, NiMultiTargetTransformController))
    {
        auto* multiTargetTransformController = static_cast<const NiMultiTargetTransformController*>(this);
        if (index == 0xFFFF)
            return nullptr;
        return multiTargetTransformController->GetTargets()[index];
    }
    if (index != 0 && index != 0xFFFF)
    {
#ifdef _DEBUG
        if (IsDebuggerPresent())
            DebugBreak();
        ERROR_LOG("GetTargetNode: index is not 0");
#endif
        return nullptr;
    }
    return NI_DYNAMIC_CAST(NiAVObject, this->m_pkTarget);
}

bool NiControllerManager::BlendFromPose(NiControllerSequence* pkSequence, float fDestFrame, float fDuration,
                                        int iPriority, NiControllerSequence* pkSequenceToSynchronize)
{
#if !_DEBUG || !NI_OVERRIDE
    return GameFuncs::BlendFromPose(this, pkSequence, fDestFrame, fDuration, iPriority, pkSequenceToSynchronize);
#else
    NIASSERT(pkSequence && pkSequence->GetOwner() == this &&
        (!pkSequenceToSynchronize || pkSequenceToSynchronize->GetOwner() ==
            this));

    NiControllerSequence* pkTempSequence = CreateTempBlendSequence(pkSequence,
        pkSequenceToSynchronize);
    return pkTempSequence->StartBlend(pkSequence, fDuration, fDestFrame,
        iPriority, 1.0f, 1.0f, nullptr);
#endif
}

bool NiControllerManager::CrossFade(NiControllerSequence* pkSourceSequence, NiControllerSequence* pkDestSequence,
    float fDuration, int iPriority, bool bStartOver, float fWeight, NiControllerSequence* pkTimeSyncSeq)
{
#if !_DEBUG || !NI_OVERRIDE
    return GameFuncs::CrossFade(this, pkSourceSequence, pkDestSequence, fDuration, iPriority, bStartOver, fWeight, pkTimeSyncSeq);
#else
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
#endif
}

NiControllerSequence* NiControllerManager::CreateTempBlendSequence(NiControllerSequence* pkSequence,
    NiControllerSequence* pkSequenceToSynchronize)
{
    return ThisStdCall<NiControllerSequence*>(0xA2F170, this, pkSequence, pkSequenceToSynchronize);
}

bool NiControllerManager::Morph(NiControllerSequence* pkSourceSequence, NiControllerSequence* pkDestSequence,
    float fDuration, int iPriority, float fSourceWeight, float fDestWeight)
{
#if !_DEBUG || !NI_OVERRIDE
    return ThisStdCall<bool>(0xA2E1B0, this, pkSourceSequence, pkDestSequence, fDuration, iPriority, fSourceWeight, fDestWeight);
#else
    NIASSERT(pkSourceSequence && pkSourceSequence->GetOwner() == this &&
        pkDestSequence && pkDestSequence->GetOwner() == this);

    return pkSourceSequence->StartMorph(pkDestSequence, fDuration, iPriority,
        fSourceWeight, fDestWeight);
#endif
}

bool NiControllerManager::ActivateSequence(NiControllerSequence* pkSequence, int iPriority, bool bStartOver,
    float fWeight, float fEaseInTime, NiControllerSequence* pkTimeSyncSeq)
{
#if !_DEBUG || !NI_OVERRIDE
    return GameFuncs::ActivateSequence(this, pkSequence, iPriority, bStartOver, fWeight, fEaseInTime, pkTimeSyncSeq);
#else
    return pkSequence->Activate(iPriority, bStartOver, fWeight, fEaseInTime, pkTimeSyncSeq, false);
#endif
}

bool NiControllerManager::DeactivateSequence(NiControllerSequence* pkSequence, float fEaseOutTime)
{
#if !_DEBUG || !NI_OVERRIDE
    return GameFuncs::DeactivateSequence(this, pkSequence, fEaseOutTime);
#else
    return pkSequence->Deactivate(fEaseOutTime, false);
#endif
}

void NiControllerSequence::DetachInterpolators() const
{
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem &kItem = m_pkInterpArray[ui];
        if (kItem.m_pkBlendInterp)
        {
            kItem.m_pkBlendInterp->RemoveInterpInfo(kItem.m_ucBlendIdx);
        }
    }
}

bool NiControllerSequence::Deactivate_(float fEaseOutTime, bool bTransition)
{
    if (m_eState == INACTIVE)
    {
        return false;
    }

    if (fEaseOutTime > 0.0f)
    {
        if (bTransition)
        {
            m_eState = TRANSSOURCE;
        }
        else
        {
            m_eState = EASEOUT;
        }
        m_fStartTime = -NI_INFINITY;
        m_fEndTime = fEaseOutTime;
    }
    else
    {
        // Store the new offset.
        if (m_fLastTime != -NI_INFINITY)
        {
            m_fOffset += (m_fWeightedLastTime / m_fFrequency) - m_fLastTime;
        }

        m_eState = INACTIVE;
        m_pkPartnerSequence = NULL;
        m_fDestFrame = -NI_INFINITY;

        DetachInterpolators();
    }
    return true;
}

bool NiControllerSequence::DeactivateNoReset(float fEaseOutTime)
{
    if (m_eState != EASEIN || fEaseOutTime == 0.0f)
    {
#if _DEBUG
        DebugBreak();
#endif
        return false;
    }
    
    // Store the current animation state for a smooth transition
    const float fCurrentTime = m_fLastTime - m_fOffset;
        
    // Calculate current ease level if we were easing in
    const float fCurrentEaseLevel = (fCurrentTime - m_fStartTime) / (m_fEndTime - m_fStartTime);
    // Adjust timing to create a smooth transition from EASEIN to EASEOUT
    if (fCurrentEaseLevel > 0.0f && fCurrentEaseLevel < 1.0f)
    {
        // Set timing so that EASEOUT starts from the correct partially-eased level
        // This creates a virtual start time that produces the correct ease spinner value
        m_fEndTime = fCurrentTime + (fEaseOutTime * fCurrentEaseLevel);
        m_fStartTime = fCurrentTime - (fEaseOutTime * (1.0f - fCurrentEaseLevel));
        m_fLastTime = -NI_INFINITY;
    }
    else
    {
#if _DEBUG
        DebugBreak();
#endif
        return false;
    }
    
    m_eState = EASEOUT;
    return true;
}

namespace NiHooks
{
    void WriteHooks()
    {
        // Spider hands fix
        WriteRelJump(0xA40C10, &NiBlendTransformInterpolator::BlendValuesFixFloatingPointError);
        //WriteRelJump(0xA41110, &NiBlendTransformInterpolator::_Update);
        //WriteRelJump(0xA3FDB0, &NiTransformInterpolator::_Update);
        //WriteRelJump(0xA37260, &NiBlendInterpolator::ComputeNormalizedWeights);
        // WriteRelJump(0xA39960, &NiBlendAccumTransformInterpolator::BlendValues); // modified and enhanced movement bugs out when sprinting
        //WriteRelJump(0x4F0380, &NiMultiTargetTransformController::_Update);
        //WriteRelJump(0xA2F800, &NiControllerManager::BlendFromPose);
        //WriteRelJump(0xA2E280, &NiControllerManager::CrossFade);
        //WriteRelJump(0xA2E1B0, &NiControllerManager::Morph);
        //WriteRelJump(0xA30C80, &NiControllerSequence::CanSyncTo);
        //WriteRelJump(0xA34F20, &NiControllerSequence::Activate);
        //WriteRelJump(0xA35030, &NiControllerSequence::Deactivate_);
        //WriteRelJump(0xA34BA0, &NiControllerSequence::Update);
    }

    // override stewie's tweaks
    void WriteDelayedHooks()
    {
#if _DEBUG && 0
        WriteRelCall(0x4F0087, &NiMultiTargetTransformController::_Update);
#endif
    }
}

std::unordered_map<NiInterpolator*, NiControllerSequence*> g_interpsToSequenceMap;

void __fastcall NiControllerSequence_AttachInterpolatorsHook(NiControllerSequence* _this, void*,  char cPriority)
{
    std::span interpItems(_this->m_pkInterpArray, _this->m_uiArraySize);
    for (auto& interp : interpItems)
    {
        g_interpsToSequenceMap[interp.m_spInterpolator] = _this;
    }
    ThisStdCall(0xA30900, _this, cPriority);
}

void __fastcall NiControllerSequence_DetachInterpolatorsHook(NiControllerSequence* _this, void*)
{
    ThisStdCall(0xA30540, _this);
    std::span interpItems(_this->m_pkInterpArray, _this->m_uiArraySize);
    for (auto& interp : interpItems)
    {
        g_interpsToSequenceMap.erase(interp.m_spInterpolator);
    }
}

std::unordered_set<NiControllerSequence*> g_appliedDestFrameAnims;

void ApplyDestFrame(NiControllerSequence* sequence, float destFrame)
{
    sequence->m_fDestFrame = destFrame;
    g_appliedDestFrameAnims.insert(sequence);
}

bool IsTempBlendSequence(const NiControllerSequence* sequence)
{
    return strncmp("__", sequence->m_kName.CStr(), 2) == 0;
}

std::string GetLastSubstringAfterSlash(const std::string& str)
{
    const auto pos = str.find_last_of('\\');
    if (pos == std::string::npos)
        return str;
    return str.substr(pos + 1);
}


#define EXPERIMENTAL_HOOKS 0

void __fastcall NiControllerSequenceUpdateHook(NiControllerSequence* sequence, void*, float fTime, bool bUpdateInterpolators)
{
    if (sequence->m_eState == NiControllerSequence::INACTIVE)
    {
        return;
    }
    if (sequence->m_fStartTime == -NI_INFINITY)
    {
        const auto fEaseSpinner = sequence->GetEaseSpinner();

        if (sequence->m_eState == NiControllerSequence::EASEOUT)
        {
            if (fEaseSpinner == 0.0f)
            {
                sequence->Deactivate(0.0f, false);
                return;
            }

            float fEaseOutTime = sequence->m_fEndTime;

            // Choose a start and end time so that the ease spinner starts
            // at the same value that it ended at.  Also, scale the total
            // ease out time by the ease spinner.  If the sequence is
            // X% eased out already, then reduce the ease out time by X%.
            sequence->m_fEndTime = fTime + fEaseSpinner * fEaseOutTime;

            // Note: Use the minimum here as the floating point math
            // may work out such that fStartTime > fTime, which should
            // never happen (and there are asserts about, below).
            sequence->m_fStartTime = min(sequence->m_fEndTime - fEaseOutTime, fTime);
        }
        else if (sequence->m_eState == NiControllerSequence::EASEIN)
        {
            float fEaseInTime = sequence->m_fEndTime;
            
            // See comments above in EASEOUT case.
            sequence->m_fEndTime = fTime + (1.0f - fEaseSpinner) * fEaseInTime;
            sequence->m_fStartTime = min(sequence->m_fEndTime - fEaseInTime, fTime);
        }
    }
    sequence->Update(fTime, bUpdateInterpolators);
}

void ApplyNiHooks()
{
#if _DEBUG && 1
    NiHooks::WriteHooks();
    // WriteRelCall(0xA2E251, &NiControllerSequenceUpdateHook);
#endif
#if EXPERIMENTAL_HOOKS

    if (g_fixBlendSamePriority)
    {
        //WriteRelJump(0xA37260, NiBlendInterpolator_ComputeNormalizedWeights);
        WriteRelCall(0xA34F71, NiControllerSequence_AttachInterpolatorsHook);
        WriteRelCall(0xA350C5, NiControllerSequence_DetachInterpolatorsHook);
        //WriteRelJump(0xA2E280, CrossFadeHook);
    }

#if _DEBUG
    SafeWriteBuf(0xA35093, "\xEB\x15\x90", 3);

    WriteRelCall(0xA2F817, TempBlendDebugHook);
   
#endif
#endif
}