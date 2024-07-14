#include "nihooks.h"

#include "blend_fixes.h"
#include "commands_animation.h"
#include "hooks.h"
#include "NiNodes.h"
#include "SafeWrite.h"

#include <array>
#include <functional>
#include <ranges>

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

        if (bTransChanged)
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
        if (bScaleChanged)
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


namespace NiHooks
{
    void WriteHooks()
    {
        WriteRelJump(0xA40C10, &NiBlendTransformInterpolator::BlendValues);
        WriteRelJump(0xA3FDB0, &NiTransformInterpolator::_Update);
        WriteRelJump(0xA37260, &NiBlendInterpolator::ComputeNormalizedWeights);
        WriteRelJump(0xA39960, &NiBlendAccumTransformInterpolator::BlendValues);
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

void FixConflictingPriorities(NiControllerSequence* pkSource, NiControllerSequence* pkDest)
{
    const auto sourceControlledBlocks = pkSource->GetControlledBlocks();
    std::unordered_map<NiBlendInterpolator*, NiControllerSequence::ControlledBlock*> sourceInterpMap;
    for (auto& interp : sourceControlledBlocks)
    {
        if (!interp.interpolator || !interp.blendInterpolator || interp.priority == 0xFF)
            continue;
        sourceInterpMap.emplace(interp.blendInterpolator, &interp);
    }
    const auto destControlledBlocks = pkDest->GetControlledBlocks();
    const auto tags = pkDest->GetIDTags();
    auto index = 0;

    const static std::unordered_set<std::string_view> s_ignoredInterps = {
        "Bip01 NonAccum", "Bip01 Translate", "Bip01 Rotate", "Bip01"
    };
    for (auto& destBlock : destControlledBlocks)
    {
        const auto& tag = tags[index++];
        auto blendInterpolator = destBlock.blendInterpolator;
        if (!destBlock.interpolator || !blendInterpolator || destBlock.priority == 0xFF)
            continue;
        if (s_ignoredInterps.contains(tag.m_kAVObjectName.CStr()))
            continue;
        if (auto it = sourceInterpMap.find(blendInterpolator); it != sourceInterpMap.end())
        {
            const auto sourceInterp = *it->second;
            if (destBlock.priority != sourceInterp.priority)
                continue;
            std::span blendInterpItems(blendInterpolator->m_pkInterpArray, blendInterpolator->m_ucArraySize);
            auto destInterpItem = std::ranges::find_if(blendInterpItems, [&](const NiBlendInterpolator::InterpArrayItem& item)
            {
                return item.m_spInterpolator == destBlock.interpolator;
            });
            if (destInterpItem == blendInterpItems.end())
                continue;
            const auto newPriority = ++destInterpItem->m_cPriority;
            if (newPriority > blendInterpolator->m_cHighPriority)
            {
                blendInterpolator->m_cHighPriority = newPriority;
                blendInterpolator->m_cNextHighPriority = destBlock.priority;
            }
        }
    }
}

bool __fastcall CrossFadeHook(NiControllerManager*, void*, NiControllerSequence* pkSourceSequence, NiControllerSequence* pkDestSequence,
    float fEaseInTime, char iPriority, bool bStartOver, float fWeight, NiControllerSequence* pkTimeSyncSeq)
{
    if (pkSourceSequence->state == NiControllerSequence::kAnimState_Inactive
        || pkDestSequence->state != NiControllerSequence::kAnimState_Inactive)
        return false;
    pkSourceSequence->Deactivate(fEaseInTime, false);
    const auto result = ThisStdCall<bool>(0xA34F20, pkDestSequence, iPriority, bStartOver, fWeight, fEaseInTime, pkTimeSyncSeq, false);
    if (result && fEaseInTime > 0.01f)
        FixConflictingPriorities(pkSourceSequence, pkDestSequence);
    return result;
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
    NiHooks::WriteHooks();
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