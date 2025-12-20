#include "movement_blend_fixes.h"

#include "blend_fixes.h"
#include "hooks.h"

void SetAnimDataState(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	constexpr auto sequenceId = kSequence_Movement;
	animData->groupIDs[sequenceId] = pkSequence->animGroup->groupID;
	animData->animSequence[sequenceId] = pkSequence;
	animData->sequenceState1[sequenceId] = 0;
}

void FixJumpLoopMovementSpeed(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	if (pkSequence->animGroup->GetBaseGroupID() == kAnimGroup_JumpLoop && animData != g_thePlayer->firstPersonAnimData)
	{
		auto* forwardAnim = GetAnimByGroupID(animData, kAnimGroup_Forward);
		if (forwardAnim && forwardAnim->animGroup)
			animData->movementSpeedMult = BSGlobals::walkSpeed / forwardAnim->animGroup->moveVector.Length() * animData->actor->GetWalkSpeedMult();
	}
}

BSAnimGroupSequence* MovementBlendFixes::PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	FixJumpLoopMovementSpeed(animData, pkSequence);
	
	const float fDuration = pkSequence->GetEaseInTime();
	auto* currentMoveAnim = animData->animSequence[kSequence_Movement];
	if (!currentMoveAnim)
	{
		currentMoveAnim = GetActiveSequenceWhere(animData, [](const BSAnimGroupSequence* seq)
		{
			return seq->animGroup->GetSequenceType() == kSequence_Movement && seq->m_eState != NiControllerSequence::EASEOUT;
		});
	}

	if (currentMoveAnim && currentMoveAnim != pkSequence && currentMoveAnim->m_eState != NiControllerSequence::EASEOUT)
	{
		if (currentMoveAnim->m_eState == NiControllerSequence::EASEIN)
			currentMoveAnim->DeactivateNoReset(fDuration, false);
		else
			currentMoveAnim->Deactivate(fDuration, false);
	}

	if (pkSequence->m_eState == NiControllerSequence::EASEOUT)
	{
		if (!pkSequence->ActivateNoReset(fDuration, false))
			return pkSequence;
	}
	else
	{
		if (!pkSequence->Activate(0, true, pkSequence->m_fSeqWeight, fDuration, nullptr, false))
			return pkSequence;
	}

	SetAnimDataState(animData, pkSequence);
	return pkSequence;
}

