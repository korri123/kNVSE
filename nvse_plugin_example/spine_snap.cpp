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

	if (currentState != kAnimState_EaseIn)
		// spine snap issue only happens when easing in aim/is anims
		return RESUME;

	const auto blend = GetDefaultBlendTime(currentSequence);
	const auto currentAnimTime = GetAnimTime(animData, currentSequence);

	if (currentState != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(currentSequence->owner, currentSequence, blend);

	if (destAnim->state != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(destAnim->owner, destAnim, 0.0f);

	float destFrame = blend - currentAnimTime;
	destAnim->destFrame = destFrame / destAnim->frequency;
	currentSequence->destFrame = destFrame / currentSequence->frequency;

	const auto destBlend = GetDefaultBlendTime(destAnim);
	GameFuncs::ActivateSequence(destAnim->owner, destAnim, 0, true, destAnim->seqWeight, destBlend, nullptr);
	SetCurrentSequence(animData, destAnim);

	return SKIP;
}

void SpineSnapFix::ApplyHooks()
{
	// JMP 0x4996E7 -> 0x499709
	// disable the game resetting sequence state for aimup and aimdown as we will do it ourselves
	SafeWriteBuf(0x4996E7, "\xEB\x20\x90\x90", 4);
}
