#include "spine_snap.h"

#include "commands_animation.h"
#include "GameObjects.h"
#include "hooks.h"
#include "main.h"

bool IsAnimGroupAim(AnimGroupID groupId)
{
	return groupId >= kAnimGroup_Aim && groupId <= kAnimGroup_AimISDown;
}

void SetCurrentSequence(AnimData* animData, BSAnimGroupSequence* destAnim)
{
	const auto sequenceId = destAnim->animGroup->GetGroupInfo()->sequenceType;
	const auto animGroupId = destAnim->animGroup->groupID;
	animData->animSequence[sequenceId] = destAnim;
	animData->sequenceState1[sequenceId] = 0;
	animData->groupIDs[sequenceId] = animGroupId;
}

void SetAimWeight(BSAnimGroupSequence* sequence)
{
	// 0x494B7D
	auto* baseProcess = g_thePlayer->GetHighProcess();
	if (baseProcess)
	{
		auto* aimSequence = baseProcess->animSequence[0];
		if (aimSequence)
			sequence->seqWeight = aimSequence->seqWeight;
	}
}

bool g_appliedFix = false;
float g_aimDestFrame = 0.0f;

SpineSnapFix::Result SpineSnapFix::ApplyFix(AnimData* animData, BSAnimGroupSequence* destAnim)
{
	if (!destAnim || !destAnim->animGroup || animData == g_thePlayer->firstPersonAnimData)
		return RESUME;
	const auto sequenceId = static_cast<eAnimSequence>(destAnim->animGroup->GetGroupInfo()->sequenceType);
	if (sequenceId == kSequence_None)
		return RESUME;
	const auto destGroupId = destAnim->animGroup->groupID;
	const auto destBaseGroupId = static_cast<AnimGroupID>(destGroupId & 0xFF);
	if (!IsAnimGroupAim(destBaseGroupId))
		return RESUME;
	BSAnimGroupSequence* currentSequence = animData->animSequence[sequenceId];

	const auto isUpOrDownAnim = sequenceId == kSequence_WeaponDown || sequenceId == kSequence_WeaponUp;

	if (destBaseGroupId == kAnimGroup_Aim || destBaseGroupId == kAnimGroup_AimIS)
		g_appliedFix = false;

	if (isUpOrDownAnim && !currentSequence)
	{
		// game sets current sequence to null when up/down anims are played
		UInt32 currentBaseGroupId;
		switch (destBaseGroupId)
		{
		case kAnimGroup_AimUp:
			currentBaseGroupId = kAnimGroup_AimISUp;
			break;
		case kAnimGroup_AimDown:
			currentBaseGroupId = kAnimGroup_AimISDown;
			break;
		case kAnimGroup_AimISUp:
			currentBaseGroupId = kAnimGroup_AimUp;
			break;
		case kAnimGroup_AimISDown:
			currentBaseGroupId = kAnimGroup_AimDown;
			break;
		default:
			return RESUME;
		}
		currentSequence = GetAnimByGroupID(animData, (destGroupId & 0xFF00) + currentBaseGroupId);
	}

	if (!currentSequence || !currentSequence->animGroup)
		return RESUME;

	const auto isTransitioningTo = [&](AnimGroupID fromGroupId, AnimGroupID toGroupId)
	{
		return currentSequence->animGroup->GetBaseGroupID() == fromGroupId && destAnim->animGroup->GetBaseGroupID() == toGroupId;
	};

	const auto isAimOrAimISAnim =  isTransitioningTo(kAnimGroup_AimIS, kAnimGroup_Aim) || isTransitioningTo(kAnimGroup_Aim, kAnimGroup_AimIS);

	const auto currentState = currentSequence->state;
	
	if (!isAimOrAimISAnim && !isUpOrDownAnim)
		return RESUME;

	if (isAimOrAimISAnim && currentState != kAnimState_EaseIn)
		// spine snap issue only happens when easing in aim/is anims
		return RESUME;

	if (isUpOrDownAnim && !g_appliedFix)
	{
		// handle the case when the game has already started easing in the up/down anim but otherwise aim/is finished blending
		// not happy with this hack
		return RESUME;
	}

	const auto blend = GetDefaultBlendTime(currentSequence);
	const auto currentAnimTime = GetAnimTime(animData, currentSequence);

	if (currentState != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(currentSequence->owner, currentSequence, blend);

	if (destAnim->state != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(destAnim->owner, destAnim, 0.0f);

	float destFrame = blend - currentAnimTime;
	if (isUpOrDownAnim)
		destFrame = g_aimDestFrame;
	else // anim already easing out so destframe for aim is used
		g_aimDestFrame = destFrame;
	destAnim->destFrame = destFrame / destAnim->frequency;
	currentSequence->destFrame = destFrame / currentSequence->frequency;

	if (!isUpOrDownAnim)
	{
		g_appliedFix = true;
		const auto destBlend = GetDefaultBlendTime(destAnim);
		GameFuncs::ActivateSequence(destAnim->owner, destAnim, 0, true, destAnim->seqWeight, destBlend, nullptr);
		SetCurrentSequence(animData, destAnim);
		SetAimWeight(destAnim);
		return SKIP;
	}
	return RESUME;
}

void SpineSnapFix::ApplyHooks()
{
	// JMP 0x4996E7 -> 0x499709
	//SafeWriteBuf(0x4996E7, "\xEB\x20\x90\x90", 4);
	// disable the game resetting sequence state for aimup and aimdown as we will do it ourselves
}
