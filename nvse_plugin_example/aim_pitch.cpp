#include "aim_pitch.h"

#include <charconv>
#include <cmath>

#include "aim_transitions.h"
#include "blend_smoothing.h"
#include "game_types.h"
#include "hooks.h"
#include "SafeWrite.h"
#include "sequence_extradata.h"
#include "string_view_util.h"
#include "text_key_arguments.h"
#include "utility.h"

namespace
{
    constexpr std::string_view kKeyName = "aimPitch";

    // What the vanilla rifle aimup/aimdown KFs turn (30 degrees each at the full 90)
    constexpr std::string_view kDefaultBones[] = { "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2" };

    // MiddleHighProcess::UpdateAimSequences (0x9295C0), virtual: Actor::Update calls it every frame for third person
    // skeletons, Actor::StartWeaponGroup after starting a weapon group and its up/down variants
    void __fastcall UpdateAimSequencesHook(Decoding::MiddleHighProcess* process, void*, Actor* actor)
    {
        ThisStdCall(0x9295C0, process, actor);
        auto* sequence = process->weaponSequence[0];
        if (!sequence || !AimPitch::Applies(*sequence))
            return;
        // the pose looking straight ahead, as the game weights them at a pitch of 0
        sequence->m_fSeqWeight = 1.0f;
        for (auto* variant : { process->weaponSequence[1], process->weaponSequence[2] })
            if (variant)
                variant->m_fSeqWeight = 0.0f;
    }

    // Actor::UpdateAnimationMovement's call of Animation::UpdateMovementNoWorldUpdate (0x4939D0), which blends the bones:
    // the player's skeletons, and every actor's with iNumHWThreads 1. The other actors' go through
    // Actor::UpdateAnimMovement (the hook at 0x8885C9 in hooks.cpp)
    UInt32 g_uiUpdateMovementNoWorldUpdateAddr;
    void __fastcall UpdateMovementNoWorldUpdateHook(AnimData* animData, void*, Actor* actor)
    {
        AimPitch::Update(animData, actor);
        ThisStdCall(g_uiUpdateMovementNoWorldUpdateAddr, animData, actor);
    }
}

bool AimPitch::HasKey(const NiControllerSequence& sequence)
{
    return TextKeys::FindArguments(sequence, kKeyName).has_value();
}

bool AimPitch::Applies(const NiControllerSequence& sequence)
{
    return HasKey(sequence) || AimTransitions::Of(&sequence) != TransitionKind::None;
}

AimPitch::Shares::Shares(const NiControllerSequence& sequence)
{
    const auto arguments = TextKeys::FindArguments(sequence, kKeyName);
    if (!arguments)
    {
        // an aim transition turns as with the key without arguments
        m_default = AimTransitions::Of(&sequence) != TransitionKind::None;
        return;
    }
    if (arguments->empty())
    {
        m_default = true;
        return;
    }
    TextKeys::ArgumentReader reader(*arguments);
    while (const auto next = reader.Next())
    {
        const auto entry = *next;
        if (entry.empty())
            continue;
        const auto space = entry.find_last_of(" \t");
        float share = 0.0f;
        const bool parsed = space != std::string_view::npos &&
            std::from_chars(entry.data() + space + 1, entry.data() + entry.size(), share).ec == std::errc();
        if (!parsed || m_count == kMaxBones)
        {
            ERROR_LOG(FormatString("aimPitch: can't use \"%.*s\" in %s (expected aimPitch: <bone> <share>, <bone> <share>, ..., at most %u bones)",
                static_cast<int>(entry.size()), entry.data(), sequence.m_kName.CStr(), kMaxBones));
            continue;
        }
        m_entries[m_count++] = { sv::trim(entry.substr(0, space)), share };
    }
}

float AimPitch::Shares::Get(std::string_view bone) const
{
    if (m_default)
    {
        for (const auto defaultBone : kDefaultBones)
            if (sv::equals_ci(defaultBone, bone))
                return 1.0f / static_cast<float>(std::size(kDefaultBones));
        return 0.0f;
    }
    for (unsigned int i = 0; i < m_count; i++)
        if (sv::equals_ci(m_entries[i].bone, bone))
            return m_entries[i].share;
    return 0.0f;
}

void AimPitch::Update(AnimData* animData, Actor* actor)
{
    if (!g_pluginSettings.blendSmoothing || !animData || !actor || animData == g_thePlayer->firstPersonAnimData)
        return;
    auto* manager = animData->controllerManager;
    auto* root = manager ? manager->m_pkTarget : nullptr;
    if (!root)
        return;
    // the pitch the game weights the up/down variants by (UpdateAimSequences): Actor::GetLooking plus the aim chase angle
    float fPitch = actor->rotX;
    if (auto* process = actor->baseProcess; process && process->processLevel <= 1)
        fPitch += static_cast<Decoding::MiddleHighProcess*>(process)->aimChaseLookAngle;
    if (auto* nodeData = kAnimationExtraData::GetOrCreate(root))
        nodeData->aimPitch = fPitch;
}

void AimPitch::Apply(const kBlendInterpolatorExtraData* extraData, const kBlendInterpItem* extraItem, NiObjectNET* target,
    NiQuatTransform& kTransform)
{
    if (!extraItem || extraItem->aimPitchShare == 0.0f || !kTransform.IsRotateValid())
        return;
    auto* manager = extraData->owner;
    auto* root = manager ? NI_DYNAMIC_CAST(NiAVObject, manager->m_pkTarget) : nullptr;
    const auto* parent = static_cast<NiAVObject*>(target)->m_pkParent;
    if (!root || !parent)
        return;
    const auto* nodeData = kAnimationExtraData::Get(root);
    if (!nodeData || nodeData->aimPitch == 0.0f)
        return;
    // the actor's right (the root's x axis) in the bone's parent space. Both world rotations are from the last world
    // update, so their relation is consistent while the actor turns. The upper body turns about the same axis on every
    // bone, which their turns leave where it is: the shares add up
    const auto& rootRotate = root->m_kWorld.m_Rotate.m_pEntry;
    const auto& parentRotate = parent->m_kWorld.m_Rotate.m_pEntry;
    NiPoint3 kAxis;
    kAxis.x = parentRotate[0][0] * rootRotate[0][0] + parentRotate[1][0] * rootRotate[1][0] + parentRotate[2][0] * rootRotate[2][0];
    kAxis.y = parentRotate[0][1] * rootRotate[0][0] + parentRotate[1][1] * rootRotate[1][0] + parentRotate[2][1] * rootRotate[2][0];
    kAxis.z = parentRotate[0][2] * rootRotate[0][0] + parentRotate[1][2] * rootRotate[1][0] + parentRotate[2][2] * rootRotate[2][0];
    const float fLength = kAxis.Length();
    if (fLength < 1e-6f)
        return;
    // a positive pitch looks down: forward (y) turns towards -z, a negative angle about the right axis
    const float fHalfAngle = -0.5f * nodeData->aimPitch * extraItem->aimPitchShare;
    const float fSin = std::sin(fHalfAngle) / fLength;
    const NiQuaternion kPitch(std::cos(fHalfAngle), kAxis.x * fSin, kAxis.y * fSin, kAxis.z * fSin);
    // turned in the parent's space: local' = pitch * local
    kTransform.m_kRotate = kPitch * kTransform.m_kRotate;
}

void AimPitch::WriteHooks()
{
    ReplaceVTableEntry(0x1087C8C, &UpdateAimSequencesHook); // HighProcess
    ReplaceVTableEntry(0x1089474, &UpdateAimSequencesHook); // MiddleHighProcess
    WriteRelCall(0x88866B, &UpdateMovementNoWorldUpdateHook, &g_uiUpdateMovementNoWorldUpdateAddr);
}
