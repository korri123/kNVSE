#include "anim_switch.h"

#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "utility.h"

namespace
{
    // When the ease the sequence is in ends, in its owner's time: until its first update the window is the duration alone
    float GetEaseEnd(const NiControllerSequence& sequence, float fTime)
    {
        return sequence.m_fStartTime == -NI_INFINITY ? fTime + sequence.m_fEndTime : sequence.m_fEndTime;
    }
}

AnimSwitch AnimSwitch::Current(AnimData* animData)
{
    NiControllerManager* manager = animData ? animData->controllerManager : nullptr;
    if (!manager || !manager->m_pkTarget)
        return { nullptr, 0, 0.0f };
    const float fTime = animData->timePassed;
    auto& open = kAnimationExtraData::GetOrCreate(manager->m_pkTarget)->openSwitch;
    if (open.manager != manager || open.fTime != fTime)
    {
        open.manager = manager;
        open.fTime = fTime;
        if (++open.uiID == 0)
            ++open.uiID;
    }
    return { manager, open.uiID, fTime };
}

bool AnimSwitch::EaseOut(NiControllerSequence* sequence, float fDuration) const
{
    switch (sequence->m_eState)
    {
    case NiControllerSequence::INACTIVE:
        return false;
    case NiControllerSequence::EASEOUT:
        return true;
    case NiControllerSequence::EASEIN:
    {
        // the switch that was easing it in, asked before its ease turns around
        const UInt32 uiEasedInBy = Of(sequence);
        if (fDuration > 0.0f && sequence->DeactivateNoReset(fDuration, false, m_fTime))
        {
            if (Join(sequence))
            {
                TakeOver(sequence, uiEasedInBy);
                CoverEaseIns();
            }
            return true;
        }
        break;
    }
    default:
        break;
    }
    const bool bResult = sequence->Deactivate(fDuration, false);
    Add(sequence);
    return bResult;
}

bool AnimSwitch::EaseIn(NiControllerSequence* sequence, float fDuration) const
{
    bool bResult;
    if (sequence->m_eState == NiControllerSequence::EASEOUT && fDuration > 0.0f)
    {
        bResult = sequence->ActivateNoReset(fDuration, false, m_fTime);
    }
    else
    {
        if (sequence->m_eState == NiControllerSequence::EASEOUT)
            sequence->Deactivate(0.0f, false);
        bResult = sequence->Activate(0, true, sequence->m_fSeqWeight, fDuration, nullptr, false);
    }
    if (bResult)
        Add(sequence);
    return bResult;
}

void AnimSwitch::Add(NiControllerSequence* sequence) const
{
    if (Join(sequence))
        CoverEaseIns();
}

bool AnimSwitch::Join(NiControllerSequence* sequence) const
{
    // ids count per scene root: a sequence of another manager has its own
    if (!m_uiID || !sequence || sequence->m_pkOwner != m_pkManager)
        return false;
    const auto eState = sequence->m_eState;
    if (eState != NiControllerSequence::EASEIN && eState != NiControllerSequence::EASEOUT)
        return false;
    auto* extraData = SequenceExtraDatas::GetOrCreate(sequence);
    if (!extraData)
        return false;
    Ease& ease = Eases::Get(*extraData, *sequence, Eases::GetSpinner(*sequence, m_fTime));
    if (ease.uiSwitch != m_uiID)
    {
        ease.uiSwitch = m_uiID;
        ease.bCurveResolved = false;
    }
    return true;
}

void AnimSwitch::TakeOver(NiControllerSequence* sequence, UInt32 uiEasedInBy) const
{
    if (Ease* ease = Eases::Find(*sequence))
        ease->bMakesWay = true;
    if (!uiEasedInBy)
        return;
    for (auto* other : m_pkManager->m_kActiveSequences.ToSpan())
    {
        Ease* ease = other ? Eases::Find(*other) : nullptr;
        if (!ease || ease->uiSwitch != uiEasedInBy)
            continue;
        // all of them: a cross fade split over two switches is blended as two eases that have nothing to do with each
        // other (BlendTransitions)
        if (ease->uiSwitch != m_uiID)
        {
            ease->uiSwitch = m_uiID;
            ease->bCurveResolved = false;
        }
        if (other->m_eState == NiControllerSequence::EASEOUT)
            ease->bMakesWay = true;
    }
}

void AnimSwitch::CoverEaseIns() const
{
    const auto activeSequences = m_pkManager->m_kActiveSequences.ToSpan();
    float fLastEaseInEnd = -NI_INFINITY;
    for (const auto* sequence : activeSequences)
    {
        if (sequence && sequence->m_eState == NiControllerSequence::EASEIN && Of(sequence) == m_uiID)
            fLastEaseInEnd = max(fLastEaseInEnd, GetEaseEnd(*sequence, m_fTime));
    }
    if (fLastEaseInEnd == -NI_INFINITY)
        return;
    for (auto* sequence : activeSequences)
    {
        if (!sequence || sequence->m_eState != NiControllerSequence::EASEOUT)
            continue;
        const Ease* ease = Eases::Find(*sequence);
        if (!ease || ease->uiSwitch != m_uiID || !ease->bMakesWay || GetEaseEnd(*sequence, m_fTime) >= fLastEaseInEnd)
            continue;
        if (sequence->m_fStartTime == -NI_INFINITY)
        {
            // not updated since it was eased out: the window is still its length
            sequence->m_fEndTime = fLastEaseInEnd - m_fTime;
            continue;
        }
        const float fSpinner = Eases::GetSpinner(*sequence, m_fTime);
        if (fSpinner <= 0.0f)
            continue;
        // the spinner stays where it is, and reaches 0 at the new end
        sequence->m_fEndTime = fLastEaseInEnd;
        sequence->m_fStartTime = fLastEaseInEnd - (fLastEaseInEnd - m_fTime) / fSpinner;
    }
}

UInt32 AnimSwitch::Of(const NiControllerSequence* sequence)
{
    const Ease* ease = Eases::Find(*sequence);
    return ease ? ease->uiSwitch : 0;
}

// The engine's own eases. Animation::StartGroup (0x4949A0) keeps its Animation at [ebp-0x5C]; the sequences
// Animation::ClearGroup eases out are declared by its hooks in hooks.cpp, the sequence StartGroup starts from a pose
// by PoseBlendFixes
void AnimSwitch::WriteHooks()
{
    // NiControllerManager::CrossFade in Animation::StartGroup, its only caller: the section's sequence for the new one
    WriteRelCall(0x4952F8, INLINE_HOOK(bool, __fastcall, NiControllerManager* manager, void*, NiControllerSequence* pkSource,
        NiControllerSequence* pkDest, float fDuration, int iPriority, bool bStartOver, float fWeight,
        NiControllerSequence* pkTimeSyncSeq)
    {
        auto* animData = GET_CALLER_VAR_LAMBDA(AnimData*, -0x5C);
        const bool bResult = ThisStdCall<bool>(0xA2E280, manager, pkSource, pkDest, fDuration, iPriority, bStartOver, fWeight,
            pkTimeSyncSeq);
        if (bResult)
        {
            const auto animSwitch = Current(animData);
            animSwitch.Add(pkSource);
            animSwitch.Add(pkDest);
        }
        return bResult;
    }));

    // NiControllerManager::ActivateSequence in Animation::StartGroup for an aim up or down section with nothing playing:
    // the one of its three that eases in
    WriteRelCall(0x495216, INLINE_HOOK(bool, __fastcall, NiControllerManager* manager, void*, NiControllerSequence* pkSequence,
        int iPriority, bool bStartOver, float fWeight, float fEaseInTime, NiControllerSequence* pkTimeSyncSeq)
    {
        auto* animData = GET_CALLER_VAR_LAMBDA(AnimData*, -0x5C);
        const bool bResult = ThisStdCall<bool>(0x47AAB0, manager, pkSequence, iPriority, bStartOver, fWeight, fEaseInTime,
            pkTimeSyncSeq);
        Current(animData).Add(pkSequence);
        return bResult;
    }));
}
