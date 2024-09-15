#include "additive_anims.h"

#include "hooks.h"
#include "SafeWrite.h"
#include "utility.h"

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
    const auto& kAdditiveMgr = AdditiveSequences::Get();
    for (auto& kItem : interpolator.GetItems())
    {
        if (!kAdditiveMgr.IsAdditiveInterpolator(kItem.m_spInterpolator.data))
            continue;
        if (kItem.m_fNormalizedWeight != 0.0f)
        {
#ifdef _DEBUG
            // this interpolator should not be factored into the normalized weight since that is reserved for the weighted blend animations
            // if normalized weight is not 0, then this interpolator is part of the weighted blend
            if (IsDebuggerPresent())
                DebugBreak();
#endif
            continue;
        }

        const float fUpdateTime = interpolator.GetManagerControlled() ? kItem.m_fUpdateTime : fTime;
        if (fUpdateTime == INVALID_TIME)
            continue;
        NiQuatTransform kInterpTransform;
        NiQuatTransform kBaseTransform;
        const auto eType = kAdditiveMgr.GetAdditiveTransformType(kItem.m_spInterpolator.data);
        if (eType == AdditiveTransformType::BaseTransform)
        {
            const auto& kBaseTransformResult = kAdditiveMgr.GetBaseTransform(kItem.m_spInterpolator.data);
            if (!kBaseTransformResult)
                continue;
            kBaseTransform = *kBaseTransformResult;
        }
        else if (eType == AdditiveTransformType::EachFrame)
        {
            kBaseTransform = kValue;
        }
        if (kItem.m_spInterpolator.data->Update(fUpdateTime, pkInterpTarget, kInterpTransform))
        {
            NiQuatTransform kSubtractedTransform = kInterpTransform - kBaseTransform;
            const float fWeight = kItem.m_fWeight * kItem.m_fEaseSpinner;
            if (kSubtractedTransform.IsTranslateValid())
            {
                kFinalTranslate += kSubtractedTransform.GetTranslate() * fWeight;
                bTransChanged = true;
            }
            if (kSubtractedTransform.IsRotateValid())
            {
                NiQuaternion kSubtractedRotate = kSubtractedTransform.GetRotate();

                if (!bFirstRotation)
                {
                    const float fCos = NiQuaternion::Dot(kFinalRotate, kSubtractedRotate);

                    // If the angle is negative, we need to invert the
                    // quat to get the best path.
                    if (fCos < 0.0f)
                        kSubtractedRotate = -kSubtractedRotate;
                }
                else
                    bFirstRotation = false;

                kSubtractedRotate = kSubtractedRotate * fWeight;

                kFinalRotate.SetValues(
                    kSubtractedRotate.GetW() + kFinalRotate.GetW(),
                    kSubtractedRotate.GetX() + kFinalRotate.GetX(),
                    kSubtractedRotate.GetY() + kFinalRotate.GetY(),
                    kSubtractedRotate.GetZ() + kFinalRotate.GetZ()
                );
                bRotChanged = true;
            }
            if (kSubtractedTransform.IsScaleValid())
            {
                fFinalScale += kSubtractedTransform.GetScale() * fWeight;
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
        kFinalRotate.Normalize();
        kValue.m_kRotate = kValue.m_kRotate * kFinalRotate;
    }
    if (bScaleChanged)
    {
        kValue.m_fScale += fFinalScale;
    }
}

void AdditiveAnimations::WriteHooks()
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
                if (item.m_spInterpolator != nullptr && item.m_cPriority != SCHAR_MIN && AdditiveSequences::Get().IsAdditiveInterpolator(item.m_spInterpolator))
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