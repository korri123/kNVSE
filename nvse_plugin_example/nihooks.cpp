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

#include "additive_anims.h"
#include "blend_smoothing.h"
#include "sequence_extradata.h"

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
    double dTotalTransWeight = 0.0;
    double dTotalScaleWeight = 0.0;
    double dTotalRotWeight = 0.0;

    NiPoint3 kFinalTranslate = NiPoint3::ZERO;
    NiQuaternion kFinalRotate = NiQuaternion(0.0f, 0.0f, 0.0f, 0.0f);
    float fFinalScale = 0.0f;

    bool bTransChanged = false;
    bool bRotChanged = false;
    bool bScaleChanged = false;

    bool bFirstRotation = true;
    auto interpItems = GetItems();
    auto* kExtraData = kBlendInterpolatorExtraData::GetExtraData(pkInterpTarget);
    
    struct ValidTranslate
    {
        NiPoint3 translate;
        InterpArrayItem* item;
    };
    thread_local std::vector<ValidTranslate> validTranslates;
    validTranslates.clear();
    
    struct ValidRotation
    {
        NiQuaternion rotation;
        InterpArrayItem* item;
    };
    thread_local std::vector<ValidRotation> validRotations;
    validRotations.clear();

    struct ValidScale
    {
        float scale;
        InterpArrayItem* item;
    };
    thread_local std::vector<ValidScale> validScales;
    validScales.clear();

    for (auto& item : interpItems)
    {
        if (!item.m_spInterpolator || AdditiveManager::IsAdditiveInterpolator(kExtraData, item.m_spInterpolator) || !GetUpdateTimeForItem(fTime, item))
            continue;
        
        NiQuatTransform kTransform;
        if (!item.m_spInterpolator->Update(fTime, pkInterpTarget, kTransform))
            continue;
        if (kTransform.IsTranslateValid())
            validTranslates.emplace_back(kTransform.GetTranslate(), &item);
        if (kTransform.IsRotateValid())
            validRotations.emplace_back(kTransform.GetRotate(), &item);
        if (kTransform.IsScaleValid())
            validScales.emplace_back(kTransform.GetScale(), &item);
    }

    thread_local std::vector<InterpArrayItem*> items;
    items.clear();
    
    for (auto& item : validTranslates)
        items.emplace_back(item.item);
    ComputeNormalizedWeights(items);
    BlendSmoothing::ApplyForItems(kExtraData, items, kWeightType::Translate);

    float fFinalWeight = 0.0f;
    for (auto& translation : validTranslates)
    {
        kFinalTranslate += translation.translate *
                       translation.item->m_fNormalizedWeight;
        dTotalTransWeight += translation.item->m_fNormalizedWeight;
        bTransChanged = true;
        fFinalWeight += translation.item->m_fNormalizedWeight;
    }
    
    //if (!items.empty())
    //    DebugAssert(std::abs(1.0f - fFinalWeight) < 0.001f);
    items.clear();

    for (auto& item : validRotations)
        items.emplace_back(item.item);
    ComputeNormalizedWeights(items);
    BlendSmoothing::ApplyForItems(kExtraData, items, kWeightType::Rotate);
    
    fFinalWeight = 0.0f;
    for (auto& rotation : validRotations)
    {
        NiQuaternion kRotValue = rotation.rotation;

        if (!bFirstRotation)
        {
            float fCos = NiQuaternion::Dot(kFinalRotate, kRotValue);
            if (fCos < 0.0f)
            {
                kRotValue = -kRotValue;
            }
        }
        else
        {
            bFirstRotation = false;
        }

        float weight = rotation.item->m_fNormalizedWeight;
        
        kRotValue = kRotValue * weight;
        dTotalRotWeight += weight;

        kFinalRotate.SetValues(
            kRotValue.GetW() + kFinalRotate.GetW(),
            kRotValue.GetX() + kFinalRotate.GetX(),
            kRotValue.GetY() + kFinalRotate.GetY(),
            kRotValue.GetZ() + kFinalRotate.GetZ());
        bRotChanged = true;
        fFinalWeight += weight;
    }
    //if (!items.empty())
    //    DebugAssert(std::abs(1.0f - fFinalWeight) < 0.001f);
    items.clear();
    for (auto& scale : validScales)
        items.emplace_back(scale.item);

    ComputeNormalizedWeights(items);
    BlendSmoothing::ApplyForItems(kExtraData, items, kWeightType::Scale);

    for (auto& scale : validScales)
    {
        fFinalScale += scale.scale *
                       scale.item->m_fNormalizedWeight;
        dTotalScaleWeight += scale.item->m_fNormalizedWeight;
        bScaleChanged = true;
    }

    BlendSmoothing::DetachZeroWeightItems(kExtraData, this);

    kValue.MakeInvalid();
    if (bTransChanged || bRotChanged || bScaleChanged)
    {
        constexpr double EPSILON = 1e-10;
        if (bTransChanged && dTotalTransWeight > EPSILON)
        {
            float fInverseWeight = static_cast<float>(1.0 / dTotalTransWeight);
            kFinalTranslate *= fInverseWeight;
            kValue.SetTranslate(kFinalTranslate);
        }
        
        if (bRotChanged && dTotalRotWeight > EPSILON)
        {
            kFinalRotate.Normalize();
            kValue.SetRotate(kFinalRotate);
        }
        
        if (bScaleChanged && dTotalScaleWeight > EPSILON)
        {
            float fInverseWeight = static_cast<float>(1.0 / dTotalScaleWeight);
            fFinalScale *= fInverseWeight;
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

bool NiBlendTransformInterpolator::UpdateHooked(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue)
{
    bool bReturnValue = false;
    auto* kExtraData = kBlendInterpolatorExtraData::GetExtraData(pkInterpTarget);
    if (m_ucInterpCount == 1)
    {
        bReturnValue = StoreSingleValue(fTime, pkInterpTarget, kValue);
        // remove last interp
        if (g_pluginSettings.blendSmoothing && !g_pluginSettings.poseInterpolators && kExtraData)
        {
            auto* kExtraItem = kExtraData->GetItem(this->m_pkSingleInterpolator);
            if (kExtraItem && kExtraItem->detached)
            {
                this->RemoveInterpInfo(this->m_ucSingleIdx);
                kExtraItem->ClearValues();
                kExtraItem->debugState = kInterpDebugState::RemovedInStoreSingle;
            }
        }
    }
    else if (m_ucInterpCount > 0)
    {
        bReturnValue = BlendValuesFixFloatingPointError(fTime, pkInterpTarget, kValue);
        if (GetHasAdditiveTransforms())
            ApplyAdditiveTransforms(fTime, pkInterpTarget, kValue);
    }
    m_fLastTime = fTime;
    return !kValue.IsTransformInvalid() && bReturnValue;
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

void NiBlendInterpolator::ComputeNormalizedWeights(const std::vector<InterpArrayItem*>& items)
{
    if (items.size() == 1)
    {
        items.front()->m_fNormalizedWeight = 1.0f;
        return;
    }

    char cHighPriority = INVALID_INDEX;
    char cNextHighPriority = INVALID_INDEX;
    for (auto& item : items)
    {
        if (item->m_spInterpolator != nullptr)
        {
            if (item->m_cPriority > cHighPriority)
            {
                cNextHighPriority = cHighPriority;
                cHighPriority = item->m_cPriority;
            }
            else if (item->m_cPriority > cNextHighPriority && item->m_cPriority < cHighPriority)
            {
                cNextHighPriority = item->m_cPriority;
            }
        }
    }

    float fHighSumOfWeights = 0.0f;
    float fNextHighSumOfWeights = 0.0f;
    float fHighEaseSpinner = 0.0f;
    for (auto& kItemPtr : items)
    {
        auto& kItem = *kItemPtr;
        if (kItem.m_spInterpolator != nullptr)
        {
            float fRealWeight = kItem.m_fWeight * kItem.m_fEaseSpinner;
            if (kItem.m_cPriority == cHighPriority)
            {
                fHighSumOfWeights += fRealWeight;
                if (kItem.m_fEaseSpinner > fHighEaseSpinner)
                {
                    fHighEaseSpinner = kItem.m_fEaseSpinner;
                }
            }
            else if (kItem.m_cPriority == cNextHighPriority)
            {
                fNextHighSumOfWeights += fRealWeight;
            }
        }
    }

    float fOneMinusHighEaseSpinner = 1.0f - fHighEaseSpinner;
    float fTotalSumOfWeights = fHighEaseSpinner * fHighSumOfWeights +
        fOneMinusHighEaseSpinner * fNextHighSumOfWeights;
    float fOneOverTotalSumOfWeights =
        (fTotalSumOfWeights > 0.0f) ? (1.0f / fTotalSumOfWeights) : 0.0f;

    // Compute normalized weights.
    for (auto& kItemPtr : items)
    {
        auto& kItem = *kItemPtr;
        if (kItem.m_spInterpolator != nullptr)
        {
            if (kItem.m_cPriority == cHighPriority)
            {
                kItem.m_fNormalizedWeight = fHighEaseSpinner *
                    kItem.m_fWeight * kItem.m_fEaseSpinner *
                    fOneOverTotalSumOfWeights;
            }
            else if (kItem.m_cPriority == cNextHighPriority)
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
        for (unsigned char uc = 0; uc < m_ucArraySize; uc++)
        {
            InterpArrayItem& kItem = m_pkInterpArray[uc];
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
            (fSumOfNormalizedWeights > 0.0f) ?
            (1.0f / fSumOfNormalizedWeights) : 0.0f;

        for (unsigned char uc = 0; uc < m_ucArraySize; uc++)
        {
            InterpArrayItem& kItem = m_pkInterpArray[uc];
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
        for (unsigned char uc = 0; uc < m_ucArraySize; uc++)
        {
            if (m_pkInterpArray[uc].m_fNormalizedWeight > fHighest)
            {
                ucHighIndex = uc;
                fHighest = m_pkInterpArray[uc].m_fNormalizedWeight;
            }
            m_pkInterpArray[uc].m_fNormalizedWeight = 0.0f;
        }

        NIASSERT(ucHighIndex != INVALID_INDEX);
        // Set the highest index to 1.0
        m_pkInterpArray[ucHighIndex].m_fNormalizedWeight = 1.0f;
    }
}

void NiBlendInterpolator::ComputeNormalizedWeightsHighPriorityDominant()
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
        InterpArrayItem& kItem = m_pkInterpArray[uc];
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

    assert(m_fHighEaseSpinner >= 0.0f && m_fHighEaseSpinner <= 1.0f);

    // Check if high priority sum is approximately 1.0
    const float EPSILON = 0.001f;  // Tolerance value for "approximately"
    bool bHighPriorityOnly = (fabs(m_fHighSumOfWeights - 1.0f) < EPSILON);

    float fTotalSumOfWeights;
    float fOneOverTotalSumOfWeights;

    if (bHighPriorityOnly)
    {
        // If high priority weights sum to ~1.0, only use high priority
        fTotalSumOfWeights = m_fHighSumOfWeights;
    }
    else
    {
        // Original blending behavior
        float fOneMinusHighEaseSpinner = 1.0f - m_fHighEaseSpinner;
        fTotalSumOfWeights = m_fHighEaseSpinner * m_fHighSumOfWeights +
            fOneMinusHighEaseSpinner * m_fNextHighSumOfWeights;
    }

    fOneOverTotalSumOfWeights = 
        (fTotalSumOfWeights > 0.0f) ? (1.0f / fTotalSumOfWeights) : 0.0f;

    // Compute normalized weights.
    for (uc = 0; uc < m_ucArraySize; uc++)
    {
        InterpArrayItem& kItem = m_pkInterpArray[uc];
        if (kItem.m_spInterpolator != NULL)
        {
            if (kItem.m_cPriority == m_cHighPriority)
            {
                if (bHighPriorityOnly)
                {
                    // Simple normalization when only using high priority
                    kItem.m_fNormalizedWeight = kItem.m_fWeight * kItem.m_fEaseSpinner *
                        fOneOverTotalSumOfWeights;
                }
                else
                {
                    // Original calculation
                    kItem.m_fNormalizedWeight = m_fHighEaseSpinner *
                        kItem.m_fWeight * kItem.m_fEaseSpinner *
                        fOneOverTotalSumOfWeights;
                }
            }
            else if (!bHighPriorityOnly && kItem.m_cPriority == m_cNextHighPriority)
            {
                // Only calculate for next high priority if not in high-priority-only mode
                float fOneMinusHighEaseSpinner = 1.0f - m_fHighEaseSpinner;
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

bool NiControllerSequence::DisabledBlendSmoothing() const
{
    const static NiFixedString key = "noBlendSmoothing";
    return m_spTextKeys && m_spTextKeys->FindFirstByName(key);
}

void NiControllerSequence::AttachInterpolators(char cPriority)
{
#if (!_DEBUG || !NI_OVERRIDE) && 0
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

void NiControllerSequence::AttachInterpolatorsHooked(char cPriority)
{
    auto* sequenceExtraData = SequenceExtraDatas::Get(this);

    if (sequenceExtraData && sequenceExtraData->needsStoreTargets)
    {
        auto* target = NI_DYNAMIC_CAST(NiAVObject, this->m_pkOwner->m_pkTarget);
        DebugAssert(target);
        if (target)
            StoreTargets(target);
        sequenceExtraData->needsStoreTargets = false;
    }
    
    if (sequenceExtraData && sequenceExtraData->additiveMetadata)
    {
        AdditiveManager::MarkInterpolatorsAsAdditive(this);
        AttachInterpolatorsAdditive(cPriority, sequenceExtraData);
        return;
    }
    
    if (!g_pluginSettings.blendSmoothing)
    {
        AttachInterpolators(cPriority);
        return;
    }
    const auto disabledBlendSmoothing = DisabledBlendSmoothing();
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem &kItem = m_pkInterpArray[ui];
        auto* pkBlendInterp = kItem.m_pkBlendInterp;
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
        DebugAssert(target && target->m_pcName == idTag.m_kAVObjectName);

        auto* extraData = kBlendInterpolatorExtraData::Obtain(target);
        DebugAssert(extraData);
        extraData->owner = m_pkOwner;
        if (disabledBlendSmoothing)
            ++extraData->noBlendSmoothRequesterCount;
        auto& extraInterpItem = extraData->ObtainItem(kItem.m_spInterpolator);
        if (pkBlendInterp->m_ucArraySize == 0 && extraInterpItem.blendIndex != INVALID_INDEX)
        {
            extraInterpItem.ClearValues();
            extraInterpItem.interpolator = kItem.m_spInterpolator;
        }
        
        extraInterpItem.sequence = this;
        extraInterpItem.state = kInterpState::Activating;
        extraInterpItem.blendInterp = pkBlendInterp;

        if (extraInterpItem.detached)
        {
            extraInterpItem.detached = false;
            DebugAssert(extraInterpItem.blendIndex < pkBlendInterp->m_ucArraySize
                && kItem.m_ucBlendIdx == INVALID_INDEX);

            kItem.m_ucBlendIdx = extraInterpItem.blendIndex;
            extraInterpItem.blendIndex = INVALID_INDEX;

            DebugAssert(kItem.m_ucBlendIdx < pkBlendInterp->m_ucArraySize);
            pkBlendInterp->SetPriority(cInterpPriority, kItem.m_ucBlendIdx);
            extraInterpItem.debugState = kInterpDebugState::ReattachedWhileSmoothing;
        }
        else
        {
            DebugAssert(extraInterpItem.blendIndex == INVALID_INDEX);
            kItem.m_ucBlendIdx = pkBlendInterp->AddInterpInfo(kItem.m_spInterpolator, 0.0f, cInterpPriority);
            extraInterpItem.debugState = kInterpDebugState::AttachedNormally;
        }
        DebugAssert(pkBlendInterp->m_pkInterpArray[kItem.m_ucBlendIdx].m_spInterpolator == kItem.m_spInterpolator &&
            extraInterpItem.interpolator == kItem.m_spInterpolator &&
            kItem.m_spInterpolator != nullptr &&
            kItem.m_ucBlendIdx != INVALID_INDEX);

        if (g_pluginSettings.poseInterpolators)
        {
            auto* poseInterp = extraData->ObtainPoseInterp(target);
            poseInterp->m_kTransformValue = target->m_kLocal;
        }
    }
}

bool NiControllerSequence::Activate(char cPriority, bool bStartOver, float fWeight, float fEaseInTime,
                                    NiControllerSequence* pkTimeSyncSeq, bool bTransition)
{
#if !_DEBUG || !NI_OVERRIDE
    return ThisStdCall<bool>(0xA34F20, this, cPriority, bStartOver, fWeight, fEaseInTime, pkTimeSyncSeq, bTransition);
#else
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
    AttachInterpolatorsHooked(cPriority);

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

bool NiControllerSequence::Activate(float fEaseInTime, bool bTransition)
{
    return Activate(0, false, this->m_fSeqWeight, fEaseInTime, nullptr, bTransition);
}

bool NiControllerSequence::ActivateNoReset(float fEaseInTime, bool bTransition)
{
    if ((m_eState != EASEOUT && m_eState != TRANSSOURCE) || fEaseInTime == 0.0f)
    {
#if _DEBUG
        DebugBreak();
#endif
        return false;
    }
    
    const auto fTime = m_fLastTime - m_fOffset;
    const auto fCurrentEaseTime = m_fEndTime - m_fStartTime;

    auto fAnimTime = fTime - m_fStartTime;
    fAnimTime = max(fAnimTime, 0.0f); // floating point inaccuracy fix
    const auto fEaseOutProgress = fAnimTime / fCurrentEaseTime;
            
    // Calculate current animation level (1.0 at start of EASEOUT, 0.0 at end)
    const float fCurrentAnimLevel = 1.0f - fEaseOutProgress;
            
    // Only apply special handling if we're partially through the ease-out
    if (fCurrentAnimLevel >= 0.0f && fCurrentAnimLevel <= 1.0f)
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
        
    m_eState = bTransition ? TRANSDEST : EASEIN;
    m_fLastTime = -NI_INFINITY;
    return true;
}

bool NiControllerSequence::StartBlend(NiControllerSequence* pkDestSequence, float fDuration, float fDestFrame,
                                      int iPriority, float fSourceWeight, float fDestWeight, NiControllerSequence* pkTimeSyncSeq)
{
#if 0
    // thanks stewie
    return ThisStdCall<bool>(0xA350D0, this, pkDestSequence, fDuration, fDestFrame, iPriority, fSourceWeight, fDestWeight, pkTimeSyncSeq);
#else
    // Deactivate source sequence first.
    Deactivate(0.0f, true);

    if (fDuration <= 0.0f)
    {
        fDuration = 0.0001f;
    }

    // The following "Frame" variables must be divided by frequency
    // because they are eventually passed into ComputeScaledTime.
    // Must set them here because they are the only way to figure out if
    // we are in a blend transition, and the activation callbacks might
    // need that info.
    m_fDestFrame = m_fLastScaledTime;
    m_fDestFrame /= m_fFrequency;

    fDestFrame /= m_fFrequency;
    pkDestSequence->m_fDestFrame = fDestFrame;

    // Activate source and destination sequences.
    if (!Activate(iPriority, false, fSourceWeight, 0.0f, NULL, true) ||
        !pkDestSequence->Activate(iPriority, false, fDestWeight, fDuration,
        pkTimeSyncSeq, true))
    {
        return false;
    }

    // Ease out source sequence.
    Deactivate(fDuration, true);

    return true;
#endif
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
#if !_DEBUG
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
        
        float fTimeBefore = m_fLastScaledTime;

        SetInterpsWeightAndTime(m_fSeqWeight * fTransSpinner, fEaseSpinner,
                                ComputeScaledTime(fUpdateTime, true));
        
        if (spAnimNotes)
        {
            if (m_fLastScaledTime < fTimeBefore)
                usCurAnimNIdx = -1;

            for (UInt16 i = usCurAnimNIdx + 1; i < usNumNotes; ++i)
            {
                auto* note = &spAnimNotes[i];
                if (fUpdateTime <= note->fTime)
                    break;

                usCurAnimNIdx = i;
                m_pkOwner->pListener->Update(note);
            }
        }
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

NiAVObject* NiControllerManager::GetTarget(NiInterpController* controller,
    const NiControllerSequence::IDTag& idTag)
{
    if (!GetObjectPalette())
        return nullptr;
    if (auto* target = m_spObjectPalette->m_kHash.Lookup(idTag.m_kAVObjectName))
        return target;
    if (auto* target = m_spObjectPalette->m_pkScene->GetObjectByName(idTag.m_kAVObjectName))
        return target;
    if (auto* multiTarget = NI_DYNAMIC_CAST(NiMultiTargetTransformController, controller))
    {
        auto targets = multiTarget->GetTargets();
        auto iter = std::ranges::find_if(targets, [&](const auto& target)
        {
            return target->m_pcName == idTag.m_kAVObjectName;
        });
        if (iter != targets.end())
        {
            return *iter;
        }
    }
    return nullptr;
}

NiDefaultAVObjectPalette* NiControllerManager::GetObjectPalette() const
{
    if (!m_spObjectPalette || !m_spObjectPalette->m_pkScene)
        return nullptr;
    auto* scene = NI_DYNAMIC_CAST(NiNode, m_spObjectPalette->m_pkScene);
    if (!scene || scene->m_children.m_usESize == 0)
        return nullptr;
    return m_spObjectPalette;
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

void NiControllerSequence::DetachInterpolatorsHooked()
{
    if (!g_pluginSettings.blendSmoothing)
    {
        DetachInterpolators();
        return;
    }
    const auto disabledBlendSmoothing = DisabledBlendSmoothing();
    // BlendFixes::DetachSecondaryTempInterpolators(this);
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem &kItem = m_pkInterpArray[ui];
        if (auto* blendInterp = kItem.m_pkBlendInterp; blendInterp && kItem.m_spInterpolator)
        {
            const auto& idTag = kItem.GetIDTag(this);
            DebugAssert(m_pkOwner && m_pkOwner->m_spObjectPalette);
            auto* target = m_pkOwner ? m_pkOwner->GetTarget(kItem.m_spInterpCtlr, idTag) : nullptr;
            const auto blendIndex = kItem.m_ucBlendIdx;
            if (!target)
            {
                // this case happens in the destructor of NiControllerSequence or when you have blocks that don't exist in nif ("Ripper" in 1hmidle.kf for example)
                if (blendIndex != INVALID_INDEX)
                    blendInterp->RemoveInterpInfo(blendIndex);
                kItem.m_ucBlendIdx = INVALID_INDEX;
                continue;
            }
            DebugAssert(target->m_pcName == idTag.m_kAVObjectName);
            
            auto* extraData = kBlendInterpolatorExtraData::GetExtraData(target);
            if (!extraData)
            {
                if (blendIndex != INVALID_INDEX)
                    blendInterp->RemoveInterpInfo(blendIndex);
                kItem.m_ucBlendIdx = INVALID_INDEX;
                continue;
            }
            if (extraData->noBlendSmoothRequesterCount && disabledBlendSmoothing)
                --extraData->noBlendSmoothRequesterCount;

            auto& extraInterpItem = extraData->ObtainItem(kItem.m_spInterpolator);
            extraInterpItem.state = kInterpState::Deactivating;

            DebugAssert(blendInterp->m_pkInterpArray[blendIndex].m_spInterpolator == kItem.m_spInterpolator
                && extraInterpItem.interpolator == kItem.m_spInterpolator && kItem.m_spInterpolator != nullptr);

            if (extraInterpItem.IsSmoothedWeightZero() || extraInterpItem.isAdditive)
            {
                DebugAssert(!extraInterpItem.detached && blendIndex != INVALID_INDEX);
                if (blendIndex != INVALID_INDEX)
                    blendInterp->RemoveInterpInfo(blendIndex);
                extraInterpItem.ClearValues();
                extraInterpItem.debugState = kInterpDebugState::RemovedInDetachInterpolators;

                if (blendInterp->m_ucInterpCount == 1 && g_pluginSettings.poseInterpolators)
                {
                    if (auto* poseItem = extraData->GetPoseInterpItem())
                    {
                        DebugAssert(poseItem->poseInterpIndex < blendInterp->m_ucArraySize);
                        blendInterp->RemoveInterpInfo(poseItem->poseInterpIndex);
                        poseItem->ClearValues();
                    }
                }
            }
            else
            {
                DebugAssert(!extraInterpItem.detached && extraInterpItem.blendIndex == INVALID_INDEX
                    && blendIndex != INVALID_INDEX);
                extraInterpItem.detached = true;
                extraInterpItem.blendIndex = blendIndex;
                blendInterp->SetPriority(0, blendIndex);
                DebugAssert(kItem.m_spInterpolator == extraInterpItem.interpolator &&
                    blendInterp->m_pkInterpArray[blendIndex].m_spInterpolator == kItem.m_spInterpolator &&
                    kItem.m_spInterpolator != nullptr);
                extraInterpItem.debugState = kInterpDebugState::DetachedButSmoothing;
            }
            kItem.m_ucBlendIdx = INVALID_INDEX;

            if (g_pluginSettings.poseInterpolators)
            {
                auto* poseInterp = extraData->ObtainPoseInterp(target);
                poseInterp->m_kTransformValue = target->m_kLocal;
            }
        }
    }
}

NiControllerSequence::InterpArrayItem* NiControllerSequence::GetControlledBlock(
    const NiInterpolator* interpolator) const
{
    for (unsigned int ui = 0; ui < m_uiArraySize; ui++)
    {
        InterpArrayItem &kItem = m_pkInterpArray[ui];
        if (kItem.m_spInterpolator == interpolator)
        {
            return &kItem;
        }
    }
    return nullptr;
}

NiControllerSequence::InterpArrayItem* NiControllerSequence::GetControlledBlock(
    const NiBlendInterpolator* interpolator) const
{
    for (auto& item : GetControlledBlocks())
    {
        if (item.m_pkBlendInterp == interpolator)
            return &item;
    }
    return nullptr;
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

bool NiControllerSequence::DeactivateNoReset(float fEaseOutTime, bool bTransition)
{
    if ((m_eState != EASEIN && m_eState != TRANSDEST) || fEaseOutTime == 0.0f)
    {
#if _DEBUG
        DebugBreak();
#endif
        return false;
    }
    
    // Store the current animation state for a smooth transition
    const float fCurrentTime = m_fLastTime - m_fOffset;

    float fAnimTime = fCurrentTime - m_fStartTime;
    fAnimTime = max(fAnimTime, 0.0f);
    // Calculate current ease level if we were easing in
    const float fCurrentEaseLevel = fAnimTime / (m_fEndTime - m_fStartTime);
    // Adjust timing to create a smooth transition from EASEIN to EASEOUT
    if (fCurrentEaseLevel >= 0.0f && fCurrentEaseLevel <= 1.0f)
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
    
    m_eState = bTransition ? TRANSSOURCE : EASEOUT;
    return true;
}

unsigned int __fastcall AddInterpHook(NiControllerSequence* poseSequence, UInt32 offset, NiControllerSequence* pkSequence,
    NiInterpolator* interpolator, NiControllerSequence::IDTag*, unsigned char priority)
{
    auto idx = offset / 0x10;
    const auto& newIdTag = pkSequence->m_pkIDTagArray[idx];
    const auto result = poseSequence->AddInterpolator(interpolator, newIdTag, priority);
    return result;
}

__declspec(naked) void PoseSequenceIDTagHook()
{
    __asm
    {
        mov edx, [esp + ((0x38-0x2c) + 0x3c)] // offset ((ida stack offset of hook spot - ida initial var decl stack offset) + ida var hover info)
        push ebp // pkSequence
        call AddInterpHook
        mov ecx, 0xA330B0
        jmp ecx
    }
}

namespace NiHooks
{
    void WriteHooks()
    {
        // Spider hands fix
        // WriteRelCall(0xA41160, &NiBlendTransformInterpolator::BlendValuesFixFloatingPointError); // hook conflict
        WriteRelJump(0xA330AB, &PoseSequenceIDTagHook);
        WriteRelJump(0xA30900, &NiControllerSequence::AttachInterpolatorsHooked);
        WriteRelJump(0xA30540, &NiControllerSequence::DetachInterpolatorsHooked);
        ReplaceVTableEntry(0x1097598, &NiBlendTransformInterpolator::UpdateHooked);
        //WriteRelJump(0xA41110, &NiBlendTransformInterpolator::_Update);
        //WriteRelJump(0xA3FDB0, &NiTransformInterpolator::_Update);
        //WriteRelJump(0xA37260, &NiBlendInterpolator::ComputeNormalizedWeightsHighPriorityDominant);
        //WriteRelJump(0xA39960, &NiBlendAccumTransformInterpolator::BlendValues); // modified and enhanced movement bugs out when sprinting
        //WriteRelJump(0x4F0380, &NiMultiTargetTransformController::_Update);
        //WriteRelJump(0xA2F800, &NiControllerManager::BlendFromPose);
        //WriteRelJump(0xA2E280, &NiControllerManager::CrossFade);
        //WriteRelJump(0xA2E1B0, &NiControllerManager::Morph);
        //WriteRelJump(0xA30C80, &NiControllerSequence::CanSyncTo);
        //WriteRelJump(0xA34F20, &NiControllerSequence::Activate);
        //WriteRelJump(0xA35030, &NiControllerSequence::Deactivate_);
#if _DEBUG
        // WriteRelJump(0xA34BA0, &NiControllerSequence::Update);
#endif
    }

    // override stewie's tweaks
    void WriteDelayedHooks()
    {
#if _DEBUG
        WriteRelCall(0x4F0087, &NiMultiTargetTransformController::_Update);
        WriteRelJump(0x4F0380, &NiMultiTargetTransformController::_Update);
#endif
    }
}

void ApplyNiHooks()
{
    NiHooks::WriteHooks();
}