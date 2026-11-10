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
			currentMoveAnim->DeactivateNoReset(fDuration, false, animData->timePassed);
		else
			currentMoveAnim->Deactivate(fDuration, false);
	}

	if (pkSequence->m_eState == NiControllerSequence::EASEOUT)
	{
		if (!pkSequence->ActivateNoReset(fDuration, false, animData->timePassed))
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

// when a sequence's ease-out ends, in its owner's animation time
static float GetEaseOutEndTime(const NiControllerSequence* sequence, float fTime)
{
	// Deactivate leaves the duration in m_fEndTime until the next update turns it into a time
	return sequence->m_fStartTime == -NI_INFINITY ? fTime + sequence->m_fEndTime : sequence->m_fEndTime;
}

// Animation::ClearGroup (0x496080) remembers the movement sequence it deactivates (animSeq100), and while that one eases
// out with no movement group playing, Animation::Update (0x491180) drops the movement delta. PlayMovementAnim's cross
// fade leaves the previous movement sequence easing out as well: when the remembered one ended first (a stop within
// half the blend, as on every analog stick release: run -> walk -> stop), the other's root motion moved the actor
// until its ease-out ended, a lunge at the animation's unscaled speed.
// ClearGroup copies animSequence[kSequence_Movement] right after deactivating it: leave the one that ends last there.
void MovementBlendFixes::TrackLastEaseOut(AnimData* animData)
{
	BSAnimGroupSequence* last = nullptr;
	float fLastEndTime = -NI_INFINITY;
	for (auto* sequence : animData->controllerManager->m_kActiveSequences)
	{
		auto* bsSequence = static_cast<BSAnimGroupSequence*>(sequence);
		if (!bsSequence || NOT_TYPE(bsSequence, BSAnimGroupSequence) || bsSequence->m_eState != NiControllerSequence::EASEOUT)
			continue;
		if (!bsSequence->animGroup || bsSequence->animGroup->GetSequenceType() != kSequence_Movement)
			continue;
		const float fEndTime = GetEaseOutEndTime(bsSequence, animData->timePassed);
		if (fEndTime > fLastEndTime)
		{
			last = bsSequence;
			fLastEndTime = fEndTime;
		}
	}
	if (last)
		animData->animSequence[kSequence_Movement] = last;
}

