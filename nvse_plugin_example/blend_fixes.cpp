#include "blend_fixes.h"

#include "commands_animation.h"
#include "GameObjects.h"
#include "hooks.h"
#include "main.h"
#include "nihooks.h"
#include <array>

#include "SafeWrite.h"
#include "anim_switch.h"
#include "anim_fixes.h"
#include "sequence_extradata.h"
#include "aim_transitions.h"
#include "transition_anims.h"
#include "jip_fixes.h"


bool IsAnimGroupAim(AnimGroupID groupId)
{
	return groupId >= kAnimGroup_Aim && groupId <= kAnimGroup_AimISDown;
}

bool IsAnimGroupAttack(AnimGroupID groupId)
{
	return groupId >= kAnimGroup_AttackLeft && groupId <= kAnimGroup_AttackThrow8ISDown;
}

bool HasAnimGroupUpDownVariants(AnimGroupID groupId)
{
	return (groupId >= kAnimGroup_Aim && groupId <= kAnimGroup_AttackSpin2ISDown)
	 || (groupId >= kAnimGroup_PlaceMine && groupId <= kAnimGroup_AttackThrow8ISDown);
}

bool IsAnimGroupIS(AnimGroupID groupId)
{
	const auto* groupInfo = GetGroupInfo(groupId);
	if (!groupInfo)
		return false;
	const auto* groupName = groupInfo->name;
	return strstr(groupName, "IS") != nullptr;
}

bool IsAnimGroupWeaponUp(AnimGroupID groupId)
{
	const auto* groupInfo = GetGroupInfo(groupId);
	if (!groupInfo)
		return false;
	return groupInfo->sequenceType == kSequence_WeaponUp;
}

bool IsAnimGroupWeaponDown(AnimGroupID groupId)
{
	const auto* groupInfo = GetGroupInfo(groupId);
	if (!groupInfo)
		return false;
	return groupInfo->sequenceType == kSequence_WeaponDown;
}

void AnimData::SetCurrentSequence(BSAnimGroupSequence* destAnim, bool resetSequenceState)
{
	const auto sequenceId = destAnim->animGroup->GetGroupInfo()->sequenceType;
	const auto animGroupId = destAnim->animGroup->groupID;
	animSequence[sequenceId] = destAnim;
	groupIDs[sequenceId] = animGroupId;
	if (resetSequenceState)
		sequenceState1[sequenceId] = 0;
}

template <typename F>
std::vector<BSAnimGroupSequence*> GetActiveSequencesWhere(AnimData* animData, F&& predicate)
{
	std::vector<BSAnimGroupSequence*> sequences;
	for (auto* sequence : animData->controllerManager->m_kActiveSequences)
	{
		auto* bsSequence = static_cast<BSAnimGroupSequence*>(sequence);
		if (IS_TYPE(bsSequence, BSAnimGroupSequence) && bsSequence && predicate(bsSequence))
			sequences.push_back(bsSequence);
	}
	return sequences;
}

BSAnimGroupSequence* GetOtherActiveWeaponSequence(AnimData* animData)
{
	return GetActiveSequenceWhere(animData, [&](BSAnimGroupSequence* sequence)
	{
		return sequence->animGroup->GetGroupInfo()->sequenceType == kSequence_Weapon
		   && sequence != animData->animSequence[kSequence_Weapon];
	});
}

BSAnimGroupSequence* GetActiveAttackSequence(AnimData* animData)
{
	return GetActiveSequenceWhere(animData, [&](BSAnimGroupSequence* sequence)
	{
		return sequence->animGroup->GetGroupInfo()->sequenceType == kSequence_Weapon && IsAnimGroupAttack(static_cast<AnimGroupID>(sequence->animGroup->GetBaseGroupID()));
	});
}

std::vector<BSAnimGroupSequence*> GetActiveSequencesBySequenceId(AnimData* animData, eAnimSequence sequenceId)
{
	return GetActiveSequencesWhere(animData, [&](BSAnimGroupSequence* sequence)
	{
		return sequence->animGroup->GetGroupInfo()->sequenceType == sequenceId;
	});
}

float GetIniBlend()
{
	return GetIniFloat(0x11C56FC);
}

BlendFixes::Result BlendFixes::ApplyAimBlendFix(AnimData* animData, BSAnimGroupSequence* destAnim)
{
	if (!destAnim || !destAnim->animGroup)
		return RESUME;

	const auto* destInfo = destAnim->animGroup->GetGroupInfo();
	const auto sequenceId = static_cast<eAnimSequence>(destInfo->sequenceType);
	if (sequenceId != kSequence_Weapon && sequenceId != kSequence_WeaponUp && sequenceId != kSequence_WeaponDown)
		return RESUME;

	const auto destGroupId = destAnim->animGroup->groupID;
	const auto destBaseGroupId = static_cast<AnimGroupID>(destGroupId & 0xFF);
	const auto isDestAim = IsAnimGroupAim(destBaseGroupId);
	if (!isDestAim)
		return RESUME;

	BSAnimGroupSequence* srcAnim = animData->animSequence[sequenceId];
	if (!srcAnim || !srcAnim->animGroup || srcAnim == destAnim)
		return RESUME;

	if (animData != g_thePlayer->firstPersonAnimData && sequenceId != kSequence_Weapon)
	{
		// fix variant bs
		const auto* baseProcess = animData->actor->baseProcess;
		auto* currentAnim = baseProcess->weaponSequence[sequenceId - kSequence_Weapon];
		if (currentAnim)
			destAnim->m_fSeqWeight = currentAnim->m_fSeqWeight;
	}

	animData->SetCurrentSequence(destAnim, true);

	// an aim transition at its end is in the pose of this aim: on with its own blend out
	const auto blendTime = AimTransitions::Of(srcAnim) != TransitionKind::None ? srcAnim->GetEaseOutTime() : destAnim->GetEaseInTime();

	// aim, aim up and aim down are switched one by one: they share the switch of this update
	const auto animSwitch = AnimSwitch::Current(animData);
	animSwitch.EaseOut(srcAnim, blendTime);
	animSwitch.EaseIn(destAnim, blendTime);
	return SKIP;
}

namespace
{
	void HandleSoundPathMismatch(AnimData* animData, BSAnimGroupSequence* sourceSeq, BSAnimGroupSequence* targetSeq)
	{
		if (!sourceSeq->m_spTextKeys || !targetSeq->m_spTextKeys)
			return;
		auto sourceKeys = sourceSeq->m_spTextKeys->GetKeys();
		auto targetKeys = targetSeq->m_spTextKeys->GetKeys();
		auto sourceCount = ra::count_if(sourceKeys, _L(auto& key, sv::starts_with_ci(key.m_kText.CStr(), "SoundPath:")));
		auto targetCount = ra::count_if(targetKeys, _L(auto& key, sv::starts_with_ci(key.m_kText.CStr(), "SoundPath:")));
		if (sourceCount != targetCount || sourceCount == 0)
			return;
		for (auto& key : sourceKeys)
		{
			if (!sv::starts_with_ci(key.m_kText.CStr(), "SoundPath:"))
				continue;
			const auto targetTime = GetAnimTime(targetSeq);
			const auto sourceTime = sourceSeq->m_fLastScaledTime;
			if (key.m_fTime < targetTime && key.m_fTime > sourceTime)
			{
				if (auto sound = Sounds::FromTextKey(animData, key.m_kText.CStr()))
				{
					const auto is3D = animData != g_thePlayer->firstPersonAnimData;
					sound->Play(animData->actor, is3D);
				}
			}
		}
	}
	
	// The blend the game gives the same switch outside an attack (ApplyAimBlendFix: the ease in of the aim group switched
	// to). The attack's own blend in is made for starting a shot (1 frame in the vanilla KFs), not for moving the weapon
	// between the hip and the sights.
	float GetAimBlendTime(AnimData* animData, bool toIronSights)
	{
		if (const auto* aimAnim = GetAnimByGroupID(animData, toIronSights ? kAnimGroup_AimIS : kAnimGroup_Aim))
			return aimAnim->GetEaseInTime();
		return GetDefaultBlendTime() / GetDefaultAnimMult();
	}

	// Cross fades a sequence to a variant of its group (hip fire / iron sights, standing / sneaking) at the same point of
	// the sequence. Both keep playing through the blend: a transition (StartBlend) holds both sequences at their frame
	// until it ends, which delays the sequence, its text keys and its end by the blend time. The variant continues the
	// source for Animation::Update while it eases in (ContinuedVariantState).
	bool TransitionToVariant(AnimData* animData, BSAnimGroupSequence* targetSeq, eAnimSequence sourceSequenceId, float blend)
	{
		auto* sourceSeq = animData->animSequence[sourceSequenceId];
		if (!targetSeq || !targetSeq->animGroup || !sourceSeq || !sourceSeq->animGroup || targetSeq == sourceSeq)
			return false;
		// the source may still be easing in (the first person attack: the engine cross fades it in from the aim over
		// 0.16 s with the 9mm Pistol, the third person one over a frame), the target still easing out from a switch the
		// other way
		if (sourceSeq->m_eState != NiControllerSequence::ANIMATING && sourceSeq->m_eState != NiControllerSequence::EASEIN)
			return false;
		if (targetSeq->m_eState != NiControllerSequence::INACTIVE && targetSeq->m_eState != NiControllerSequence::EASEOUT)
			return false;

		const auto sequenceType = targetSeq->animGroup->GetGroupInfo()->sequenceType;
		const bool isThirdPersonWeapon = animData != g_thePlayer->firstPersonAnimData && sequenceType >= kSequence_Weapon
			&& sequenceType <= kSequence_WeaponDown;
		// the up/down variants are weighted by the aim pitch
		if (isThirdPersonWeapon)
			targetSeq->m_fSeqWeight = sourceSeq->m_fSeqWeight;

		const auto fTime = animData->timePassed;
		const auto destFrame = sourceSeq->m_fLastScaledTime / sourceSeq->m_fEndKeyTime * targetSeq->m_fEndKeyTime;
		// From when the source counts as ANIMATING for Animation::Update, the time the variant takes over from: now, or
		// once the source's own ease in would have ended (an equip's Attach, a reload's round must not wait for the
		// variant's longer blend: the engine acts on the keys of an easing in sequence only once it is in)
		float continuesFrom = -NI_INFINITY;
		if (sourceSeq->m_eState == NiControllerSequence::EASEIN)
		{
			if (const Ease* sourceEase = Eases::Find(*sourceSeq); sourceEase && sourceEase->bContinuesVariant)
				continuesFrom = sourceEase->fContinuesFrom;
			else // m_fEndTime holds the ease in time until the sequence's first update makes it the end time
				continuesFrom = sourceSeq->m_fStartTime == INVALID_TIME ? fTime + sourceSeq->m_fEndTime : sourceSeq->m_fEndTime;
		}

		const auto animSwitch = AnimSwitch::Current(animData);
		animSwitch.EaseOut(sourceSeq, blend);
		if (!animSwitch.EaseIn(targetSeq, blend))
			return false;
		if (auto* targetExtraData = SequenceExtraDatas::GetOrCreate(targetSeq))
		{
			Ease& easeIn = Eases::Get(*targetExtraData, *targetSeq, Eases::GetSpinner(*targetSeq, fTime));
			easeIn.bContinuesVariant = true;
			easeIn.fContinuesFrom = continuesFrom;
		}
		// the ease-out the switch just gave the source
		if (Ease* easeOut = Eases::Find(*sourceSeq))
			easeOut->bContinuedByVariant = true;
		targetSeq->m_fOffset = destFrame / targetSeq->m_fFrequency - fTime;
		targetSeq->Update(fTime, true);

		Overrides::Played(animData, targetSeq);
		HandleExtraOperations(animData, targetSeq);
		SkipPassedBurstFireKeys(targetSeq);
		HandleSoundPathMismatch(animData, sourceSeq, targetSeq);

		animData->SetCurrentSequence(targetSeq, false);
		// another group in the section (attack <-> attackIS): JIP's play group handlers, which the engine's StartGroup
		// would have called. A stance variant (reloading standing <-> sneaking) goes on with the same group, as the
		// engine's next round does
		if (const auto targetGroupId = targetSeq->animGroup->GetBaseGroupID(); targetGroupId != sourceSeq->animGroup->GetBaseGroupID())
			JIPFixes::CallSetAnimGroupEvent(animData, targetGroupId);

		if (isThirdPersonWeapon)
			if (auto* highProcess = animData->actor->baseProcess; IS_TYPE(highProcess, HighProcess))
				highProcess->weaponSequence[sequenceType - kSequence_Weapon] = targetSeq;
		return true;
	}

	// Returns the third person sequence switched to
	BSAnimGroupSequence* TransitionAttackWithVariants(AnimData* animData3rd, AnimData* animData1st, AnimGroupID targetGroupId,
		bool toIronSights)
	{
		const auto blend3rd = GetAimBlendTime(animData3rd, toIronSights);
		// looked up once: started, an override's variant counts as played and the next lookup picks another
		auto* target3rd = GetAnimByGroupID(animData3rd, targetGroupId);
		TransitionToVariant(animData3rd, target3rd, kSequence_Weapon, blend3rd);
		TransitionToVariant(animData3rd, GetAnimByGroupID(animData3rd, static_cast<AnimGroupID>(targetGroupId + 1)), kSequence_WeaponUp, blend3rd); // up
		TransitionToVariant(animData3rd, GetAnimByGroupID(animData3rd, static_cast<AnimGroupID>(targetGroupId + 2)), kSequence_WeaponDown, blend3rd); // down

		TransitionToVariant(animData1st, GetAnimByGroupID(animData1st, targetGroupId), kSequence_Weapon, GetAimBlendTime(animData1st, toIronSights));
		return target3rd;
	}

	void ApplyAttackTransitionFix(bool toIronSights)
	{
		auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
		auto* animData1st = g_thePlayer->firstPersonAnimData;
		auto* animData = g_thePlayer->IsThirdPerson() ? animData3rd : animData1st;

		auto* curWeaponAnim = animData->animSequence[kSequence_Weapon];

		if (!curWeaponAnim || !curWeaponAnim->animGroup)
			return;

		const bool isValidSource = toIronSights ? curWeaponAnim->animGroup->IsAttack() && curWeaponAnim->animGroup->IsAttackNonIS() : curWeaponAnim->animGroup->IsAttackIS();
		if (!isValidSource)
			return;

		auto* highProcess = g_thePlayer->GetHighProcess();
		if (highProcess->isAiming != toIronSights)
			return;

		if (animData3rd->sequenceState1[kSequence_Weapon] < kSeqState_HitOrDetach)
			return;

		const auto curGroupId = curWeaponAnim->animGroup->groupID;
		const auto targetGroupId = static_cast<AnimGroupID>(toIronSights ? curGroupId + 3 : curGroupId - 3);

		auto* finalSequence = TransitionAttackWithVariants(animData3rd, animData1st, targetGroupId, toIronSights);
		const auto animAction = static_cast<Decoding::AnimAction>(highProcess->GetCurrentAnimAction());
		GameFuncs::Actor_SetAnimActionAndSequence(g_thePlayer, animAction, finalSequence);
	}
} // end namespace

void BlendFixes::ApplyAttackISToAttackFix()
{
	ApplyAttackTransitionFix(false);
}

void BlendFixes::ApplyAttackToAttackISFix()
{
	ApplyAttackTransitionFix(true);
}

namespace
{
	// What is left of the ease in of the idle or movement group the stance switched to: the body the weapon animation
	// poses and the pelvis that group poses then arrive together
	float GetStanceSwitchBlendTime(const AnimData* animData, const BSAnimGroupSequence* targetSeq)
	{
		float blend = 0.0f;
		for (const auto sequenceId : { kSequence_Idle, kSequence_Movement })
		{
			const auto* sequence = animData->animSequence[sequenceId];
			if (!sequence || sequence->m_eState != NiControllerSequence::EASEIN)
				continue;
			// m_fEndTime holds the ease in time until the sequence's first update makes it the end time
			const auto left = sequence->m_fStartTime == INVALID_TIME ? sequence->m_fEndTime : sequence->m_fEndTime - animData->timePassed;
			if (left > blend)
				blend = left;
		}
		return blend > 0.0f ? blend : targetSeq->GetEaseInTime();
	}

	// The variant of the sequence's group in the actor's current stance (the move type of the group ID: standing,
	// sneaking, swimming), if it is another one. Only the move type changes: before an equip's Attach and after an
	// unequip's Detach the weapon is not out, so the groups the actor would pick now have the hand type of no weapon,
	// and in a weapon swap the old weapon's unequip plays while the new one is equipped. Compared by group ID: a group
	// with several KFs (random variants, override sets) returns another sequence on each lookup
	BSAnimGroupSequence* GetStanceVariant(AnimData* animData, const BSAnimGroupSequence* sequence)
	{
		constexpr UInt16 kMoveTypeMask = 0x7000;
		const UInt16 groupId = sequence->animGroup->groupID;
		const UInt16 moveType = GetActorRealAnimGroup(animData->actor, sequence->animGroup->GetBaseGroupID()) & kMoveTypeMask;
		if (moveType == (groupId & kMoveTypeMask))
			return nullptr;
		// the nearest group the engine would play: the same one where the stance has no variant of it
		const UInt16 variantId = GameFuncs::GetNearestGroupID(animData, (groupId & ~kMoveTypeMask) | moveType, false);
		if (variantId == groupId || (variantId & ~kMoveTypeMask) != (groupId & ~kMoveTypeMask))
			return nullptr;
		return GetAnimByFullGroupID(animData, variantId);
	}
}

// The engine picks the stance variant (sneaking or not) of a weapon section group only when it starts the group, while
// the idle and movement groups and the camera switch to the new stance at once (the aim groups too: the engine restarts
// aim in the new stance). The third person weapon animations key the legs and the pelvis: an attack, equip, unequip or
// reload held the body standing until it ended while the camera was already at the sneak height (Service Rifle hip
// shot: 0.7 s), and a looping reload's variants don't key Bip01 NonAccum, so the feet sank into the ground or floated
// above it for the rest of the round. Cross fades the weapon animation (and an attack's aim pitch variants) to the new
// stance's variant at once, at the same point.
void BlendFixes::ApplyWeaponStanceFix()
{
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	auto* animData1st = g_thePlayer->firstPersonAnimData;
	auto* sourceSeq = animData3rd ? animData3rd->animSequence[kSequence_Weapon] : nullptr;
	if (!sourceSeq || !sourceSeq->animGroup)
		return;
	// the engine restarts aim in the new stance itself; a start/stop clip of an aim transition plays under the aimIS
	// group, whose variant is not the clip
	const auto baseGroupId = sourceSeq->animGroup->GetBaseGroupID();
	if (IsAnimGroupAim(baseGroupId) || TransitionAnims::Of(sourceSeq) != TransitionKind::None)
		return;
	auto* targetSeq = GetStanceVariant(animData3rd, sourceSeq);
	if (!targetSeq || !targetSeq->animGroup)
		return;
	const auto blend = GetStanceSwitchBlendTime(animData3rd, targetSeq);
	if (!TransitionToVariant(animData3rd, targetSeq, kSequence_Weapon, blend))
		return;

	// the up and down variants of an attack, weighted by the aim pitch (base group + 1 and + 2)
	for (const auto sequenceId : { kSequence_WeaponUp, kSequence_WeaponDown })
	{
		auto* sourceVariant = animData3rd->animSequence[sequenceId];
		if (sourceVariant && sourceVariant->animGroup
			&& sourceVariant->animGroup->GetBaseGroupID() == baseGroupId + (sequenceId - kSequence_Weapon))
		{
			if (auto* targetVariant = GetStanceVariant(animData3rd, sourceVariant))
				TransitionToVariant(animData3rd, targetVariant, sequenceId, blend);
		}
	}

	if (auto* sourceSeq1st = animData1st->animSequence[kSequence_Weapon]; sourceSeq1st && sourceSeq1st->animGroup
		&& sourceSeq1st->animGroup->GetBaseGroupID() == baseGroupId)
	{
		if (auto* targetSeq1st = GetStanceVariant(animData1st, sourceSeq1st))
			TransitionToVariant(animData1st, targetSeq1st, kSequence_Weapon, blend);
	}

	// the action (attack, equip, reload, ...) ends with its sequence: the player's is the third person one
	auto* process = g_thePlayer->baseProcess;
	if (process->GetCurrentSequence() == sourceSeq)
		g_thePlayer->SetAnimAction(static_cast<Decoding::AnimAction>(process->GetCurrentAnimAction()), targetSeq);
}

namespace
{
	// The ease of a sequence easing in that continues a variant of its group (TransitionToVariant) and has taken over
	// from it by the Animation's time; null for any other
	const Ease* FindContinuedVariantEase(const AnimData* animData, const NiControllerSequence* sequence)
	{
		if (sequence->m_eState != NiControllerSequence::EASEIN)
			return nullptr;
		const Ease* ease = Eases::Find(*sequence);
		return ease && ease->bContinuesVariant && animData->timePassed >= ease->fContinuesFrom ? ease : nullptr;
	}
}

// Animation::Update (0x491180) applies the group's speed (fWeaponReloadSpeed, fAttackSpeed, ...; call at 0x491B1B) and
// acts on its action keys (a looping reload's Hit loads a round and starts the next, an attack's keys fire and end it;
// call at 0x491C73) only while the section's current sequence is ANIMATING, and Animation::StartGroup (0x4949A0) restarts
// the current sequence in place (ResetSequence) only if it is ANIMATING (calls at 0x494BE8, 0x494C09), else cuts it
// (ClearGroup) and blends it in again from the pose. A variant cross faded in mid-sequence continues the ANIMATING
// sequence it replaced, but as EASEIN its keys were acted on only once the ease was over: a looping reload eased in
// across its Hit restarted the next round from 0.73 s instead of 0.5 s, the arms and weapon jumped 20 units in a frame;
// and a round started during the ease held the reload at its first frame for the blend.
NiControllerSequence::AnimState BlendFixes::ContinuedVariantState(const AnimData* animData, const NiControllerSequence* sequence)
{
	return FindContinuedVariantEase(animData, sequence) ? NiControllerSequence::ANIMATING : sequence->m_eState;
}

bool BlendFixes::ContinuesVariant(const NiControllerSequence& sequence)
{
	if (sequence.m_eState != NiControllerSequence::EASEIN)
		return false;
	const Ease* ease = Eases::Find(sequence);
	return ease && ease->bContinuesVariant;
}

namespace
{
	// Animation::Update runs only the current sequence of a section at the group's speed (fAttackSpeed,
	// fWeaponReloadSpeed, ...), and TransitionToVariant's variant counts as current, so the sequence it continues ran at
	// 1.0x while it eased out: Caravan Shotgun attack -> attackIS ran 1.5x vs 1.0x, the two drifted apart through the
	// blend and the attackIS reached its end first, stopping the weapon mid-blend. Keeps the sequences the variant's
	// switch declared continued (Ease::bContinuedByVariant; the section tells the weapon sequence from its up and down
	// variants) at the point of the variant: the proportion of its length TransitionToVariant started the variant at.
	void LockVariantSources(const AnimData* animData, const NiControllerSequence& variant, const Ease& variantEase)
	{
		if (!variantEase.uiSwitch || !variant.m_pkOwner || variant.m_fOffset == -NI_INFINITY || !(variant.m_fEndKeyTime > 0.0f)
			|| NOT_TYPE(&variant, BSAnimGroupSequence))
			return;
		const TESAnimGroup* variantGroup = static_cast<const BSAnimGroupSequence&>(variant).animGroup;
		if (!variantGroup)
			return;
		const auto sequenceType = variantGroup->GetGroupInfo()->sequenceType;
		const float fTime = animData->timePassed;
		const float fProgress = (fTime + variant.m_fOffset) * variant.m_fFrequency / variant.m_fEndKeyTime;
		for (auto* other : variant.m_pkOwner->m_kActiveSequences)
		{
			if (!other || other->m_eState != NiControllerSequence::EASEOUT || other->m_fOffset == -NI_INFINITY
				|| !(other->m_fFrequency > 0.0f) || NOT_TYPE(other, BSAnimGroupSequence))
				continue;
			auto* source = static_cast<BSAnimGroupSequence*>(other);
			const Ease* ease = Eases::Find(*source);
			if (!ease || !ease->bContinuedByVariant || ease->uiSwitch != variantEase.uiSwitch || !source->animGroup
				|| source->animGroup->GetGroupInfo()->sequenceType != sequenceType)
				continue;
			source->m_fOffset = fProgress * source->m_fEndKeyTime / source->m_fFrequency - fTime;
		}
	}

	// Animation::Update (0x491B1B): the state of a section's current sequence, deciding whether its clock runs at the
	// group's speed (Animation at [ebp-0x18C])
	NiControllerSequence::AnimState __fastcall SectionClockStateHook(const NiControllerSequence* sequence)
	{
		return BlendFixes::ContinuedVariantState(GET_CALLER_VAR(const AnimData*, -0x18C), sequence);
	}

	// Animation::StartGroup (0x494BE8, 0x494C09): whether the sequence to start is the current one, still playing, to
	// restart in place (Animation at [ebp-0x5C])
	NiControllerSequence::AnimState __fastcall StartGroupStateHook(const NiControllerSequence* sequence)
	{
		return BlendFixes::ContinuedVariantState(GET_CALLER_VAR(const AnimData*, -0x5C), sequence);
	}

	// Animation::Update (0x491C73): the state of a section's current sequence once the engine ran its clock at the
	// group's speed (Animation at [ebp-0x18C]), before the sequences update
	NiControllerSequence::AnimState __fastcall SectionClockRanStateHook(const NiControllerSequence* sequence)
	{
		const auto* animData = GET_CALLER_VAR(const AnimData*, -0x18C);
		const Ease* ease = FindContinuedVariantEase(animData, sequence);
		if (!ease)
			return sequence->m_eState;
		LockVariantSources(animData, *sequence, *ease);
		return NiControllerSequence::ANIMATING;
	}
}

void BlendFixes::WriteDelayedHooks()
{
	// Stewie's NVAC inlines these GetState calls in its NVSEPlugin_Load
	WriteRelCall(0x491B1B, SectionClockStateHook);
	for (const UInt32 addr : { 0x494BE8, 0x494C09 })
		WriteRelCall(addr, StartGroupStateHook);
	WriteRelCall(0x491C73, SectionClockRanStateHook);
}

void BlendFixes::ApplyAimBlendHooks()
{
	// Animation::BlendOut: JMP 0x4996E7 -> 0x499709 over its clearing of the aim up and down groups. ApplyAimBlendFix
	// blends them instead, and HandleAnimationChange clears them where the next group has none
	SafeWriteBuf(0x4996E7, "\xEB\x20\x90\x90", 4);
}


void FixConflictingPriorities(NiControllerSequence* pkSource, NiControllerSequence* pkDest, NiControllerSequence* pkIdle)
{
	const auto destControlledBlocks = pkDest->GetControlledBlocks();
    const auto tags = pkDest->GetIDTags();
    auto index = 0;

    const static std::array<NiFixedString, 1> s_ignoredInterps = {
        "Bip01 NonAccum"
    };
    for (auto& destBlock : destControlledBlocks)
    {
        const auto& tag = tags[index++];
        auto* blendInterpolator = destBlock.m_pkBlendInterp;  
        if (!destBlock.m_spInterpolator || !blendInterpolator || static_cast<unsigned char>(destBlock.m_ucPriority) == 0xFF || blendInterpolator->m_ucInterpCount <= 2)
            continue;
        if (ra::contains(s_ignoredInterps, tag.m_kAVObjectName))
            continue;
    	const auto sourceInterp = pkSource->GetControlledBlock(tag.m_kAVObjectName);
    	const auto idleInterp = pkIdle ? pkIdle->GetControlledBlock(tag.m_kAVObjectName) : nullptr;
        if (sourceInterp && idleInterp)
        {
            if (destBlock.m_ucPriority != sourceInterp->m_ucPriority
            	|| idleInterp->m_ucPriority != blendInterpolator->m_cNextHighPriority
            	|| destBlock.m_ucPriority != blendInterpolator->m_cHighPriority)
                continue;
            std::span blendInterpItems(blendInterpolator->m_pkInterpArray, blendInterpolator->m_ucArraySize);
            auto destInterpItem = std::ranges::find_if(blendInterpItems, [&](const NiBlendInterpolator::InterpArrayItem& item)
            {
                return item.m_spInterpolator == destBlock.m_spInterpolator;
            });
            if (destInterpItem == blendInterpItems.end())
                continue;
            const auto newHighPriority = static_cast<char>(destInterpItem->m_cPriority + 1);
        	const auto newNextHighPriority = sourceInterp->m_ucPriority;
            blendInterpolator->m_cHighPriority = newHighPriority;
            blendInterpolator->m_cNextHighPriority = newNextHighPriority;
        	for (auto& item : blendInterpItems)
        	{
        		if (item.m_cPriority == destInterpItem->m_cPriority && item.m_spInterpolator.data != sourceInterp->m_spInterpolator)
        			item.m_cPriority = newHighPriority;
        	}
        }
    }
}

void BlendFixes::FixConflictingPriorities(BSAnimGroupSequence* pkSource, BSAnimGroupSequence* pkDest, BSAnimGroupSequence* pkIdle)
{
	if (!g_pluginSettings.fixBlendSamePriority || !pkDest || !pkSource || !pkDest->animGroup || !pkSource->animGroup)
		return;
	auto* groupInfo = pkDest->animGroup->GetGroupInfo();
	if (!groupInfo)
		return;
	if (groupInfo->sequenceType != kSequence_Weapon)
		return;
	if (HasNoFixTextKey(pkDest))
		return;
	if (pkSource->m_eState == NiControllerSequence::EASEOUT && pkDest->m_eState == NiControllerSequence::EASEIN)
		::FixConflictingPriorities(pkSource, pkDest, pkIdle);
}

void BlendFixes::ApplyHooks()
{
}

void BlendFixes::FixPrematureFirstPersonEnd(AnimData* animData, BSAnimGroupSequence* anim)
{
	if (animData->actor != g_thePlayer
		|| anim->m_eCycleType == NiControllerSequence::LOOP
		|| !IsPlayersOtherAnimData(animData)
		|| anim->m_fLastScaledTime >= anim->m_fEndKeyTime
		|| !HasRespectEndKey(anim))
		return;
	const auto* otherAnimData = g_thePlayer->GetAnimData(!g_thePlayer->IsThirdPerson());
	const auto* anim3rd = otherAnimData ? otherAnimData->animSequence[kSequence_Weapon] : nullptr;
	if (anim3rd && anim3rd->m_fEndKeyTime == anim->m_fEndKeyTime)
		return;
	// set the time of the anim to end
	anim->m_fOffset = anim->m_fEndKeyTime - animData->timePassed;
}

void BlendFixes::ApplyMissingUpDownAnims(AnimData* animData)
{
	if (animData == g_thePlayer->firstPersonAnimData)
		return;
	auto* baseProcess = animData->actor->baseProcess;
	if (!baseProcess)
		return;
	const auto* currentAnim = baseProcess->weaponSequence[0];
	if (!currentAnim)
	{
		// another bethesda retard moment
		// you need to aim before these are set
		baseProcess->weaponSequence[0] = GetAnimByGroupID(animData, kAnimGroup_Aim);
		baseProcess->weaponSequence[1] = GetAnimByGroupID(animData, kAnimGroup_AimUp);
		baseProcess->weaponSequence[2] = GetAnimByGroupID(animData, kAnimGroup_AimDown);
	}
}

void BlendFixes::AddMissingMTIdleInterps(const AnimData* animData, BSAnimGroupSequence* anim)
{
	static bool s_doOnce = false;
	if (s_doOnce || !anim || animData != g_thePlayer->firstPersonAnimData)
		return;
	s_doOnce = true;

	const auto addMissingInterpolator = [&](const char* name) -> bool
	{
		NiFixedString nodeName(name);
		if (anim->GetControlledBlock(nodeName))
			return false;

		const auto* object = BSUtilities::GetObjectByName(animData->nBip01, nodeName);
		if (!object)
			return false;

		const auto* node = NI_DYNAMIC_CAST(NiNode, object);
		if (!node)
			return false;

		auto* interp = NiTransformInterpolator::Create(node->m_kLocal);
		const auto idTag = NiControllerSequence::IDTag {
			nodeName, nullptr, "NiTransformController", nullptr, nullptr
		};
		anim->AddInterpolator(interp, idTag, 0);
		return true;
	};

	bool result = false;
	result |= addMissingInterpolator("Bip01 Translate");
	result |= addMissingInterpolator("Bip01 Rotate");
	auto* target = NI_DYNAMIC_CAST(NiAVObject, animData->controllerManager->m_pkTarget);
	if (result && target)
		anim->StoreTargets(target);
}
