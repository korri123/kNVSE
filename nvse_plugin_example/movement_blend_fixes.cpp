#include "movement_blend_fixes.h"

#include <algorithm>

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

void SetTempBlendSequenceName(NiControllerSequence* kTempBlendSeq, NiControllerSequence* pkBlendSequence)
{
	const sv::stack_string<0x100> tempBlendSeqName("__ActivateTmpBlend__%s", sv::get_file_name(pkBlendSequence->m_kName.CStr()).data());
	kTempBlendSeq->m_kName = tempBlendSeqName.c_str();
}

BSAnimGroupSequence* MovementBlendFixes::PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* pkSequence)
{

	FixJumpLoopMovementSpeed(animData, pkSequence);
	
	auto* manager = animData->controllerManager;
	const float fDuration = pkSequence->GetEaseInTime();
	const auto existingTempBlendSeq = manager->FindSequence([](const NiControllerSequence* seq)
	{
		return sv::starts_with_ci(seq->m_kName.CStr(), "__");
	});
	auto* currentMoveAnim = animData->controllerManager->FindSequence([](const NiControllerSequence* seq)
	{
		if (NOT_TYPE(seq, BSAnimGroupSequence))
			return false;
		const TESAnimGroup* animGroup = static_cast<const BSAnimGroupSequence*>(seq)->animGroup;
		if (!animGroup)
			return false;
		return animGroup->GetSequenceType() == kSequence_Movement;
	});

	if (currentMoveAnim && existingTempBlendSeq)
		existingTempBlendSeq->Deactivate(0.0f, false);
		
	if (!currentMoveAnim)
		currentMoveAnim = existingTempBlendSeq;

	if (auto* currAnim = animData->animSequence[kSequence_Movement]; currAnim && currAnim->animGroup && currAnim->animGroup->IsTurning())
		currAnim->Deactivate(0.0f, false);

#if 1
	const auto hasInactiveBlocks = std::ranges::any_of(pkSequence->GetControlledBlocks(), [](const auto& block) {
		return block.m_pkBlendInterp && block.m_pkBlendInterp->m_ucInterpCount == 0;
	});
	const static auto sBip01 = NiFixedString("Bip01");

	if (hasInactiveBlocks && !currentMoveAnim)
	{
		// prevent snapping for nodes that weren't previously being animated
		auto* tempBlendSeq = manager->CreateTempBlendSequence(pkSequence, nullptr);
		const sv::stack_string<0x100> tempBlendSeqName("__InactiveInterpsSequence__%s", sv::get_file_name(pkSequence->m_kName.CStr()).data());
		tempBlendSeq->m_kName = tempBlendSeqName.c_str();
		for (auto& block : tempBlendSeq->GetControlledBlocks())
		{
			if (!block.m_pkBlendInterp)
				continue;
			if (block.m_pkBlendInterp->m_ucInterpCount != 0)
				tempBlendSeq->RemoveInterpolator(&block - tempBlendSeq->m_pkInterpArray);
		}
		if (const auto* bip01 = pkSequence->GetControlledBlock(sBip01))
		{
			tempBlendSeq->RemoveInterpolator(bip01 - pkSequence->m_pkInterpArray);
		}
		tempBlendSeq->Deactivate(0.0f, false);
		tempBlendSeq->Activate(0, true, pkSequence->m_fSeqWeight, 0.0f, nullptr, false);
		tempBlendSeq->Deactivate(fDuration, false);
	}
#endif
	
	if (pkSequence->m_eState == NiControllerSequence::EASEOUT && !currentMoveAnim)
	{
		if (!pkSequence->ActivateNoReset(fDuration))
			return pkSequence;
	}
	else
	{
		if (currentMoveAnim)
		{
			auto* tmpBlendSeq = manager->CreateTempBlendSequence(pkSequence, nullptr);
			SetTempBlendSequenceName(tmpBlendSeq, pkSequence);
			tmpBlendSeq->RemoveInterpolator(sBip01);
			tmpBlendSeq->Activate(0, true, tmpBlendSeq->m_fSeqWeight, 0.0f, nullptr, false);
			tmpBlendSeq->Deactivate(fDuration, false);
			currentMoveAnim->Deactivate(0.0f, false);
			if (!pkSequence->Activate(0, true, pkSequence->m_fSeqWeight, fDuration, nullptr, false))
				return pkSequence;
		}
		else
		{
			if (!pkSequence->Activate(0, true, pkSequence->m_fSeqWeight, fDuration, nullptr, false))
				return pkSequence;
		}
	}

	SetAnimDataState(animData, pkSequence);
	return pkSequence;
}

