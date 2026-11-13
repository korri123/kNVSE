#include "ease.h"

#include <algorithm>

#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "utility.h"

namespace
{
    // NiControllerSequence::Deactivate (virtual; BSAnimGroupSequence's calls the base one): with an ease-out time it
    // starts the sequence's ease-out from the top, also for a sequence already easing out. One deactivated at once is
    // left alone: its record ends with its state, and it may be on its way out of a scene whose root, the manager's
    // target, is gone
    template <UInt32 uiOriginal>
    bool __fastcall DeactivateHook(NiControllerSequence* sequence, void*, float fEaseOutTime, bool bTransition)
    {
        const bool bResult = ThisStdCall<bool>(uiOriginal, sequence, fEaseOutTime, bTransition);
        if (bResult && fEaseOutTime > 0.0f)
        {
            if (auto* extraData = SequenceExtraDatas::Get(sequence))
                extraData->ease = Ease();
        }
        return bResult;
    }
}

Ease Ease::At(NiControllerSequence::AnimState eState, float fSpinner)
{
    Ease ease;
    ease.eState = eState;
    ease.fFromSpinner = std::clamp(fSpinner, 0.0f, 1.0f);
    // the default curve is point symmetric: as far along as the spinner
    ease.fFromProgress = Eases::IsRising(eState) ? ease.fFromSpinner : 1.0f - ease.fFromSpinner;
    ease.fLastSpinner = ease.fFromSpinner;
    return ease;
}

float Ease::GetWeight(float fSpinner) const
{
    const bool bRising = Eases::IsRising(eState);
    // the spinner is linear in time: how much of the time it had left at its start has passed
    const float fLeft = bRising ? 1.0f - fFromSpinner : fFromSpinner;
    const float fPassed = bRising ? fSpinner - fFromSpinner : fFromSpinner - fSpinner;
    const float fFraction = fLeft > 0.0f ? std::clamp(fPassed / fLeft, 0.0f, 1.0f) : 1.0f;
    const float fShare = curve.Evaluate(fFromProgress + (1.0f - fFromProgress) * fFraction);
    return bRising ? fShare : 1.0f - fShare;
}

void Ease::SetCurve(const BlendCurve& newCurve, float fSpinner)
{
    const float fWeight = GetWeight(fSpinner);
    fFromSpinner = std::clamp(fSpinner, 0.0f, 1.0f);
    curve = newCurve;
    fFromProgress = newCurve.Invert(Eases::IsRising(eState) ? fWeight : 1.0f - fWeight);
}

bool Eases::IsRising(NiControllerSequence::AnimState eState)
{
    return eState == NiControllerSequence::EASEIN || eState == NiControllerSequence::TRANSDEST;
}

bool Eases::IsEasing(NiControllerSequence::AnimState eState)
{
    return IsRising(eState) || eState == NiControllerSequence::EASEOUT || eState == NiControllerSequence::TRANSSOURCE;
}

float Eases::GetSpinner(const NiControllerSequence& sequence, float fTime)
{
    const bool bRising = IsRising(sequence.m_eState);
    // Update sets the window on the sequence's first evaluation
    if (sequence.m_fStartTime == -NI_INFINITY)
        return bRising ? 0.0f : 1.0f;
    const float fLength = sequence.m_fEndTime - sequence.m_fStartTime;
    const float fPassed = fLength > 0.0f ? std::clamp((fTime - sequence.m_fStartTime) / fLength, 0.0f, 1.0f) : 1.0f;
    return bRising ? fPassed : 1.0f - fPassed;
}

Ease* Eases::Find(const NiControllerSequence& sequence)
{
    if (!IsEasing(sequence.m_eState))
        return nullptr;
    auto* extraData = SequenceExtraDatas::Get(&sequence);
    return extraData && extraData->ease.eState == sequence.m_eState ? &extraData->ease : nullptr;
}

Ease& Eases::Get(SequenceExtraData& extraData, const NiControllerSequence& sequence, float fSpinner)
{
    if (extraData.ease.eState != sequence.m_eState)
        extraData.ease = Ease::At(sequence.m_eState, fSpinner);
    return extraData.ease;
}

void Eases::Reverse(const NiControllerSequence& sequence, NiControllerSequence::AnimState eNewState, float fSpinner)
{
    // without data the sequence is on the default curve before and after: nothing to carry over
    auto* extraData = SequenceExtraDatas::Get(&sequence);
    if (!extraData)
        return;
    const Ease previous = Get(*extraData, sequence, fSpinner);
    const float fWeight = previous.GetWeight(fSpinner);
    Ease& ease = extraData->ease;
    ease = Ease::At(eNewState, fSpinner);
    ease.curve = ease.switchCurve = previous.curve;
    ease.fFromProgress = ease.curve.Invert(IsRising(eNewState) ? fWeight : 1.0f - fWeight);
}

void Eases::WriteHooks()
{
    ReplaceVTableEntry(0x1096610, &DeactivateHook<0xA35030>); // NiControllerSequence
    ReplaceVTableEntry(0x101DEC8, &DeactivateHook<0x4EFDC0>); // BSAnimGroupSequence
}
