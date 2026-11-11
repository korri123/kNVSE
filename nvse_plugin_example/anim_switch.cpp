#include "anim_switch.h"

#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "utility.h"

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
        if (fDuration > 0.0f && sequence->DeactivateNoReset(fDuration, false, m_fTime))
        {
            Add(sequence);
            return true;
        }
        break;
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
    // ids count per scene root: a sequence of another manager has its own
    if (!m_uiID || !sequence || sequence->m_pkOwner != m_pkManager)
        return;
    const auto eState = sequence->m_eState;
    if (eState != NiControllerSequence::EASEIN && eState != NiControllerSequence::EASEOUT)
        return;
    if (auto* extraData = SequenceExtraDatas::GetOrCreate(sequence))
        extraData->switchMembership = { m_uiID, eState };
}

UInt32 AnimSwitch::Of(const NiControllerSequence* sequence)
{
    const auto* extraData = SequenceExtraDatas::Get(sequence);
    if (!extraData || !extraData->switchMembership || extraData->switchMembership->eState != sequence->m_eState)
        return 0;
    return extraData->switchMembership->uiSwitch;
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
