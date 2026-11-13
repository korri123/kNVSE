#include "pose_blend_fixes.h"

#include <algorithm>
#include <vector>

#include "anim_switch.h"
#include "game_types.h"
#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "utility.h"

namespace
{
    using HardCut = kAnimationExtraData::HardCut;

    // The sequences of the Animation's manager that Animation::ClearGroup cut off for this switch. Its callers start the
    // next sequence in the same update (StartGroup itself, Actor::ForceIdleAnimation, Actor::StopAttack,
    // InterfaceManager::UpdatePipboy, ...). A sequence's cut is dropped when it leaves the manager, so every sequence
    // returned is still owned by it. Taken off the node: the manager's other cuts are of switches that are over
    std::vector<HardCut> TakeHardCuts(const AnimSwitch& animSwitch, const NiControllerManager* manager)
    {
        std::vector<HardCut> cuts;
        auto* nodeData = manager->m_pkTarget ? kAnimationExtraData::Get(manager->m_pkTarget) : nullptr;
        if (!nodeData)
            return cuts;
        for (const auto& cut : nodeData->hardCuts)
        {
            if (cut.manager == manager && cut.uiSwitch == animSwitch.GetID())
                cuts.push_back(cut);
        }
        std::erase_if(nodeData->hardCuts, [&](const HardCut& cut) { return cut.manager == manager; });
        return cuts;
    }

    // Knocked down, ragdolled, unconscious, getting up or dead: the ragdoll poses the bones, not the animations, so the
    // snapshot BlendFromPose takes is the only source of that pose
    bool IsPosedByRagdoll(const AnimData* animData)
    {
        auto* actor = animData->actor;
        if (!actor || !actor->IsActor())
            return false;
        if (actor->IsDead(true))
            return true;
        auto* process = actor->baseProcess;
        return process && process->processLevel <= 1 &&
            static_cast<Decoding::MiddleHighProcess*>(process)->knockedState != Decoding::MiddleHighProcess::kState_None;
    }

    // Brings a hard cut sequence back and fades it out from where the cut left it
    void FadeOut(const AnimSwitch& animSwitch, NiControllerManager* manager, const HardCut& cut, float fDuration)
    {
        auto* sequence = cut.sequence;
        if (sequence->m_eState != NiControllerSequence::INACTIVE)
            return;
        switch (cut.eState)
        {
        case NiControllerSequence::ANIMATING:
        case NiControllerSequence::EASEIN:
        case NiControllerSequence::EASEOUT:
            break;
        default:
            // transitions and morphs: the partner sequence they were timed against has moved on
            return;
        }
        // Deactivate(0) moved the time it was cut at into m_fOffset: without bStartOver it continues from there
        if (!manager->ActivateSequence(sequence, 0, false, sequence->m_fSeqWeight, 0.0f, nullptr))
            return;
        if (cut.eState == NiControllerSequence::ANIMATING)
        {
            animSwitch.EaseOut(sequence, fDuration);
            return;
        }
        // the ease the cut interrupted: an ease-out goes on, an ease-in turns around at the weight it had reached
        sequence->m_eState = cut.eState;
        sequence->m_fStartTime = cut.fStartTime;
        sequence->m_fEndTime = cut.fEndTime;
        if (auto* extraData = SequenceExtraDatas::Get(sequence))
            extraData->ease = cut.ease;
        if (cut.eState == NiControllerSequence::EASEIN)
            animSwitch.EaseOut(sequence, fDuration);
        else
            animSwitch.Add(sequence);
    }

    bool __fastcall EaseInInsteadOfPose(NiControllerManager* manager, AnimData* animData, NiControllerSequence* sequence,
        float fDestFrame, float fDuration, int iPriority, NiControllerSequence* pkSequenceToSynchronize)
    {
        const auto animSwitch = AnimSwitch::Current(animData);
        const auto cuts = TakeHardCuts(animSwitch, manager);
        const bool bRestart = std::ranges::any_of(cuts, [&](const HardCut& cut) { return cut.sequence == sequence; });
        // a sequence cut to start over from its first frame has nothing of its own to fade out
        if (bRestart || fDestFrame != 0.0f || pkSequenceToSynchronize || IsPosedByRagdoll(animData))
            return manager->BlendFromPose(sequence, fDestFrame, fDuration, iPriority, pkSequenceToSynchronize);
        // with the weight StartGroup gave it (BlendFromPose replaced it with 1), at StartGroup's priority 0
        if (!animSwitch.EaseIn(sequence, fDuration))
            return false;
        for (const auto& cut : cuts)
            FadeOut(animSwitch, manager, cut, fDuration);
        return true;
    }

    // NiControllerManager::BlendFromPose in Animation::StartGroup (thiscall, 5 stack arguments): pass the Animation at
    // [ebp-0x5C] in edx to the fastcall replacement, which pops the same 5 arguments
    __declspec(naked) void BlendFromPoseHook()
    {
        __asm
        {
            mov edx, [ebp - 0x5C]
            jmp EaseInInsteadOfPose
        }
    }
}

void PoseBlendFixes::RecordHardCut(AnimData* animData, NiControllerSequence* sequence, float fEaseOut)
{
    if (fEaseOut > 0.0f || !sequence || sequence->m_eState == NiControllerSequence::INACTIVE)
        return;
    const NiControllerManager* manager = sequence->m_pkOwner;
    if (!manager || !manager->m_pkTarget)
        return;
    const UInt32 uiSwitch = AnimSwitch::Current(animData).GetID();
    auto& cuts = kAnimationExtraData::GetOrCreate(manager->m_pkTarget)->hardCuts;
    // a cut is for the sequence started in the same switch: the manager's cuts of earlier switches are over
    std::erase_if(cuts, [&](const HardCut& cut)
    {
        return cut.manager == manager && (cut.uiSwitch != uiSwitch || cut.sequence == sequence);
    });
    const Ease* ease = Eases::Find(*sequence);
    cuts.push_back({ manager, sequence, sequence->m_eState, sequence->m_fStartTime, sequence->m_fEndTime, uiSwitch,
        ease ? *ease : Ease() });
}

void PoseBlendFixes::ApplyHooks()
{
    WriteRelCall(0x4953DF, &BlendFromPoseHook);
}
