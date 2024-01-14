#include "spine_snap.h"

#include "commands_animation.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "hooks.h"
#include "main.h"

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

void SetCurrentSequence(AnimData* animData, BSAnimGroupSequence* destAnim, bool resetSequenceState)
{
	const auto sequenceId = destAnim->animGroup->GetGroupInfo()->sequenceType;
	const auto animGroupId = destAnim->animGroup->groupID;
	animData->animSequence[sequenceId] = destAnim;
	if (resetSequenceState)
		animData->sequenceState1[sequenceId] = 0;
	animData->groupIDs[sequenceId] = animGroupId;
}

template <typename F>
BSAnimGroupSequence* GetActiveSequenceWhere(AnimData* animData, F&& predicate)
{
	for (auto* sequence : animData->controllerManager->m_kActiveSequences)
	{
		auto* bsSequence = static_cast<BSAnimGroupSequence*>(sequence);
		if (IS_TYPE(bsSequence, BSAnimGroupSequence) && bsSequence && predicate(bsSequence))
			return bsSequence;
	}
	return nullptr;
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

BSAnimGroupSequence* GetActiveSequenceByGroupID(AnimData* animData, AnimGroupID groupId)
{
	return GetActiveSequenceWhere(animData, [&](BSAnimGroupSequence* sequence)
	{
		return sequence->animGroup->GetBaseGroupID() == groupId;
	});
}

std::vector<BSAnimGroupSequence*> GetActiveAttackSequences(AnimData* animData)
{
	return GetActiveSequencesWhere(animData, [&](BSAnimGroupSequence* sequence)
	{
		return IsAnimGroupAttack(static_cast<AnimGroupID>(sequence->animGroup->GetBaseGroupID()));
	});
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

std::pair<BSAnimGroupSequence*, BSAnimGroupSequence*> GetUpDownSequences(AnimData* animData, const AnimGroupID baseGroupID)
{
	auto* upAnim = GetAnimByGroupID(animData, static_cast<AnimGroupID>(baseGroupID + 1));
	auto* downAnim = GetAnimByGroupID(animData, static_cast<AnimGroupID>(baseGroupID + 2));
	return { upAnim, downAnim };
}

void DeactivateUpDownVariants(AnimData* animData, const AnimGroupID baseGroupID)
{
	if (HasAnimGroupUpDownVariants(baseGroupID))
	{
		auto* upAnim = GetActiveSequenceByGroupID(animData, static_cast<AnimGroupID>(baseGroupID + 1));
		auto* downAnim = GetActiveSequenceByGroupID(animData, static_cast<AnimGroupID>(baseGroupID + 2));
		if (upAnim)
			GameFuncs::DeactivateSequence(upAnim->owner, upAnim, 0.0f);
		if (downAnim)
			GameFuncs::DeactivateSequence(downAnim->owner, downAnim, 0.0f);
	}
}

float GetIniBlend()
{
	return GetIniFloat(0x11C56FC);
}

SpineSnapFix::Result SpineSnapFix::ApplyAimBlendFix(AnimData* animData, BSAnimGroupSequence* destAnim)
{
	if (!destAnim || !destAnim->animGroup)
		return RESUME;

	const auto* destInfo = destAnim->animGroup->GetGroupInfo();
	const auto sequenceId = static_cast<eAnimSequence>(destInfo->sequenceType);
	if (sequenceId == kSequence_None)
		return RESUME;

	const auto destGroupId = destAnim->animGroup->groupID;
	const auto destBaseGroupId = static_cast<AnimGroupID>(destGroupId & 0xFF);
	const auto isDestAttack = IsAnimGroupAttack(destBaseGroupId);
	const auto isDestAim = IsAnimGroupAim(destBaseGroupId);
	if (!isDestAim && !isDestAttack)
		return RESUME;

	BSAnimGroupSequence* srcAnim = animData->animSequence[sequenceId];

	const auto isDestUpOrDown = sequenceId == kSequence_WeaponUp || sequenceId == kSequence_WeaponDown;

	if (!srcAnim || !srcAnim->animGroup || srcAnim == destAnim)
		return RESUME;

	const auto isAimOrAttack = !isDestUpOrDown && (isDestAim || isDestAttack);

	if (!isAimOrAttack && !isDestUpOrDown)
		return RESUME;

	auto srcGroupId = srcAnim->animGroup->groupID;
	auto srcBaseGroupId = static_cast<AnimGroupID>(srcGroupId & 0xFF);

	if (srcAnim->state != kAnimState_EaseIn && srcAnim->state != kAnimState_Inactive)
		// spine snap issue only happens when easing in aim/is anims
		return RESUME;

	const auto isDestAttackIS = isDestAttack && IsAnimGroupIS(destBaseGroupId);
	if (srcBaseGroupId != kAnimGroup_Aim && isDestAttackIS)
	{
		// allow blend mid aim to attack without snapping
		auto* aimAnim = GetActiveSequenceByGroupID(animData, kAnimGroup_Aim);
		if (aimAnim)
		{
			GameFuncs::DeactivateSequence(srcAnim->owner, srcAnim, 0.0f);
			DeactivateUpDownVariants(animData, srcBaseGroupId);

			srcAnim = aimAnim;
			srcGroupId = srcAnim->animGroup->groupID;
			srcBaseGroupId = static_cast<AnimGroupID>(srcGroupId & 0xFF);
		}
	}
	else if (srcBaseGroupId == kAnimGroup_Aim && isDestAttack && !IsAnimGroupIS(destBaseGroupId))
	{
		// when transitioning to normal attack from attackIS after releasing the iron sight, transition smoothly
		auto* prevWeapSeq = GetOtherActiveWeaponSequence(animData);

		if (prevWeapSeq)
		{
			GameFuncs::DeactivateSequence(srcAnim->owner, srcAnim, 0.0f);
			DeactivateUpDownVariants(animData, srcBaseGroupId);

			srcAnim = prevWeapSeq;
			srcGroupId = srcAnim->animGroup->groupID;
			srcBaseGroupId = static_cast<AnimGroupID>(srcGroupId & 0xFF);
		}
	}

	if (destAnim == srcAnim)
	{
		auto* aimAnim = GetActiveSequenceByGroupID(animData, kAnimGroup_Aim);
		if (aimAnim)
		{
			if (aimAnim->state == kAnimState_Inactive)
				GameFuncs::ActivateSequence(aimAnim->owner, aimAnim, 0, true, aimAnim->seqWeight, 0.0, nullptr);
			srcAnim = aimAnim;
			srcGroupId = srcAnim->animGroup->groupID;
			srcBaseGroupId = static_cast<AnimGroupID>(srcGroupId & 0xFF);
		}
	}

	if (sequenceId == kSequence_Weapon)
	{
		const auto activeSequences = GetActiveSequencesBySequenceId(animData, kSequence_Weapon);
		for (auto* sequence : activeSequences)
		{
			// prevent irrelevant sequences from messing with our blend
			const auto baseGroupID = static_cast<AnimGroupID>(sequence->animGroup->GetBaseGroupID());
			if (baseGroupID != srcBaseGroupId && baseGroupID != destBaseGroupId)
			{
				GameFuncs::DeactivateSequence(sequence->owner, sequence, 0.0f);
				DeactivateUpDownVariants(animData, baseGroupID);
			}
		}
	}


	{
		// fix variant bs
		const auto* baseProcess = animData->actor->baseProcess;
		const auto* currentAnim = baseProcess->animSequence[sequenceId - 4];
		if (currentAnim)
			destAnim->seqWeight = currentAnim->seqWeight;
	}

	auto blend = GetIniBlend();
	auto currentAnimTime = GetAnimTime(animData, srcAnim);
	if (currentAnimTime == -FLT_MAX)
	{
		currentAnimTime = 0.0f;
		blend = 0.0f;
	}

	if (srcAnim->state != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(srcAnim->owner, srcAnim, blend);

	if (destAnim->state != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(destAnim->owner, destAnim, 0.0f);

	float destFrame = blend - currentAnimTime;
	if (isDestUpOrDown)
	{
		// really annoyed with these desyncing
		const auto* weaponAnim = animData->animSequence[kSequence_Weapon];
		if (weaponAnim) // shouldn't ever be null
			destFrame = GetAnimTime(animData, weaponAnim);
	}
	destAnim->destFrame = destFrame / destAnim->frequency;
	srcAnim->destFrame = destFrame / srcAnim->frequency;

	GameFuncs::ActivateSequence(destAnim->owner, destAnim, 0, true, destAnim->seqWeight, blend, nullptr);
	SetCurrentSequence(animData, destAnim, false);

	return SKIP;
}

#define NOMINMAX

UInt32 GetHighestPriority(const BSAnimGroupSequence* anim)
{
	const std::span interpItems{ anim->controlledBlocks, anim->numControlledBlocks };
	const auto result = ra::max_element(interpItems, [](const auto& interp1, const auto& interp2)
	{
		return interp1.priority < interp2.priority;
	});

	if (result != interpItems.end())
		return result->priority;
	return -1;
}

UInt32 CountAnimsWithSequenceType(AnimData* animData, eAnimSequence sequenceId)
{
	UInt32 count = 0;
	for (auto* sequence : animData->controllerManager->m_kActiveSequences)
	{
		auto* bsSequence = static_cast<BSAnimGroupSequence*>(sequence);
		if (IS_TYPE(bsSequence, BSAnimGroupSequence) && bsSequence && bsSequence->animGroup->GetGroupInfo()->sequenceType == sequenceId)
			count++;
	}
	return count;
}

SpineSnapFix::Result SpineSnapFix::ApplySamePriorityFix(AnimData* animData, BSAnimGroupSequence* destAnim)
{
#if _DEBUG
	{
		BSAnimGroupSequence* srcAnim = animData->animSequence[kSequence_Weapon];
		auto* srcGroupInfo = srcAnim ? srcAnim->animGroup->GetGroupInfo() : nullptr;
		auto* dstGroupInfo = destAnim ? destAnim->animGroup->GetGroupInfo() : nullptr;
		if (dstGroupInfo && dstGroupInfo->sequenceType == kSequence_Weapon && animData == g_thePlayer->firstPersonAnimData)
			Console_Print("%s -> %s", srcGroupInfo ? srcGroupInfo->name : "null", dstGroupInfo ? dstGroupInfo->name : "null");
	}
#endif
	auto* idleAnim = animData->animSequence[kSequence_Idle];
	if (!destAnim || !idleAnim)
		return RESUME;
	if (idleAnim == destAnim)
	{
		const auto numWeaponSequences = CountAnimsWithSequenceType(animData, kSequence_Weapon);
		if (numWeaponSequences < 2)
			// carry on idle
			return RESUME;
	}

	if (idleAnim == destAnim && idleAnim->state == kAnimState_Inactive)
	{
		// don't activate idle anim while 2 weapon anims are active
		return SKIP;
	}
	
	const auto sequenceId = destAnim->animGroup->GetGroupInfo()->sequenceType;
	if (sequenceId != kSequence_Weapon)
		return RESUME;
	const auto* srcAnim = animData->animSequence[kSequence_Weapon];
	if (!srcAnim || srcAnim == destAnim)
		return RESUME;
		
	GameFuncs::DeactivateSequence(idleAnim->owner, idleAnim, 0.0f);
	return RESUME;
}

void TransitionToAttack(AnimData* animData, AnimGroupID attackIsGroupId, AnimGroupID attackGroupId)
{
	auto* attackISSequence = GetAnimByGroupID(animData, attackIsGroupId);
	auto* attackSequence = GetAnimByGroupID(animData, attackGroupId);
	if (!attackSequence || !attackISSequence || !attackISSequence->animGroup)
		return;
	const auto blend = GetIniBlend();
	const auto attackISTime = GetAnimTime(animData, attackISSequence);

	if (attackISSequence->state != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(attackISSequence->owner, attackISSequence, blend);
	attackSequence->destFrame = attackISTime / attackSequence->frequency;
	//ThisStdCall(0x4948C0, animData, attackGroupId, -1);
	GameFuncs::ActivateSequence(attackSequence->owner, attackSequence, 0, true, attackSequence->seqWeight, blend, nullptr);
	auto* idleSequence = animData->animSequence[kSequence_Idle];
	if (idleSequence && idleSequence->state != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(idleSequence->owner, idleSequence, 0.0f);
	SetCurrentSequence(animData, attackSequence, false);//

	const auto sequenceType = attackSequence->animGroup->GetGroupInfo()->sequenceType;
	if (animData != g_thePlayer->firstPersonAnimData && sequenceType >= kSequence_Weapon && sequenceType <= kSequence_WeaponDown)
	{
		// handle up down variant weights
		attackSequence->seqWeight = attackISSequence->seqWeight;
		auto* highProcess = animData->actor->baseProcess;
		if (IS_TYPE(highProcess, HighProcess))
		{
			highProcess->animSequence[sequenceType - kSequence_Weapon] = attackSequence;
		}
	}
}

float GetKeyTime(BSAnimGroupSequence* anim, SequenceState1 keyTime)
{
	const auto keyTimes = std::span { anim->animGroup->keyTimes, anim->animGroup->numKeys };
	return keyTimes[keyTime];
}

void SpineSnapFix::ApplyAttackISToAttackFix()
{
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	auto* curWeaponAnim = animData3rd->animSequence[kSequence_Weapon];
	if (!curWeaponAnim || !curWeaponAnim->animGroup || !curWeaponAnim->animGroup->IsAttackIS())
		return;
	auto* highProcess = g_thePlayer->GetHighProcess();
	const auto curGroupId = curWeaponAnim->animGroup->groupID;
	if (highProcess->isAiming)
		return;

	if (animData3rd->sequenceState1[kSequence_Weapon] < kSeqState_HitOrDetach)
		return;

	ThisStdCall(0x8BB650, g_thePlayer, false, false, false); // Actor::AimWeapon

	const auto attackGroupId = static_cast<AnimGroupID>(curGroupId - 3);
	const auto attackISGroupID = static_cast<AnimGroupID>(curGroupId);
	
	TransitionToAttack(animData3rd, attackISGroupID, attackGroupId);
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackISGroupID + 1), static_cast<AnimGroupID>(attackGroupId + 1)); // up
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackISGroupID + 2), static_cast<AnimGroupID>(attackGroupId + 2)); // down

	TransitionToAttack(g_thePlayer->firstPersonAnimData, attackISGroupID, attackGroupId);
	
	auto* attackSequence = GetAnimByGroupID(animData3rd, attackGroupId);
	highProcess->SetCurrentActionAndSequence(curGroupId - 3, attackSequence);
}


void SpineSnapFix::ApplyHooks()
{
	// JMP 0x4996E7 -> 0x499709
	// disable the game resetting sequence state for aimup and aimdown as we will do it ourselves
	SafeWriteBuf(0x4996E7, "\xEB\x20\x90\x90", 4);
}
