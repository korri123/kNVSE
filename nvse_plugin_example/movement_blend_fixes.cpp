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

void CopyPriorities(NiControllerSequence* pkSequenceTo, NiControllerSequence* pkSequenceFrom)
{
	for (auto& controlledBlock : pkSequenceTo->GetControlledBlocks())
	{
		auto* pkBlendInterp = controlledBlock.m_pkBlendInterp;
		if (!pkBlendInterp || !controlledBlock.m_spInterpolator)
			continue;
		auto& idTag = controlledBlock.GetIDTag(pkSequenceTo);
		const static auto sBip01 = NiFixedString("Bip01");
		if (idTag.m_kAVObjectName == sBip01)
			continue;
		auto* fromBlock = pkSequenceFrom->GetControlledBlock(controlledBlock.m_pkBlendInterp);
		if (!fromBlock || !fromBlock->m_spInterpolator || fromBlock->m_ucBlendIdx == INVALID_INDEX)
			continue;
		const auto& fromBlendItem = pkBlendInterp->m_pkInterpArray[fromBlock->m_ucBlendIdx];
		pkBlendInterp->SetPriority(fromBlendItem.m_cPriority, controlledBlock.m_ucBlendIdx);
	}
}

void RemoveOtherEaseOutInterpolators(NiControllerSequence* pkSequence)
{
	for (auto& controlledBlock : pkSequence->GetControlledBlocks())
	{
		auto* pkBlendInterp = controlledBlock.m_pkBlendInterp;
		if (!pkBlendInterp || !controlledBlock.m_spInterpolator)
			continue;
		for (auto& item : pkBlendInterp->GetItems())
		{
			if (item.m_spInterpolator == controlledBlock.m_spInterpolator || !item.m_spInterpolator)
				continue;
			auto* owner = pkSequence->m_pkOwner->GetInterpolatorOwner(item.m_spInterpolator);
			if (!owner)
				continue;
			if (owner->m_eState == NiControllerSequence::EASEOUT)
			{
				auto* ownerBlock = owner->GetControlledBlock(item.m_spInterpolator);
				pkBlendInterp->SetPriority(0, ownerBlock->m_ucBlendIdx);
			}
		}
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
			currentMoveAnim->DeactivateNoReset(fDuration);
		else
			currentMoveAnim->Deactivate(fDuration, false);
	}
	
	
	//if (currentMoveAnim && existingTempBlendSeq)
	//	existingTempBlendSeq->Deactivate(0.0f, false);
		
	//if (!currentMoveAnim)
	//	currentMoveAnim = existingTempBlendSeq;

#if 0
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
	
	if (pkSequence->m_eState == NiControllerSequence::EASEOUT)
	{
		if (!pkSequence->ActivateNoReset(fDuration))
			return pkSequence;
	}
	else
	{
#if 0
		if (currentMoveAnim)
		{
			auto* tmpBlendSeq = manager->CreateTempBlendSequence(pkSequence, nullptr);
			SetTempBlendSequenceName(tmpBlendSeq, pkSequence);
			tmpBlendSeq->RemoveInterpolator(sBip01);
			tmpBlendSeq->Activate(0, true, tmpBlendSeq->m_fSeqWeight, 0.0f, nullptr, false);
			tmpBlendSeq->Deactivate(fDuration, false);
			if (existingTempBlendSeq)
				existingTempBlendSeq->Deactivate(0.0f, false);
			currentMoveAnim->Deactivate(0.0f, false);
			if (!pkSequence->Activate(0, true, pkSequence->m_fSeqWeight, fDuration, nullptr, false))
				return pkSequence;
			SetSequenceInterpolatorsHighPriority(tmpBlendSeq);
			//RemoveOtherEaseOutInterpolators(tmpBlendSeq);
		}
		else
		{
			
		}
#endif
		if (!pkSequence->ActivateBlended(0, true, pkSequence->m_fSeqWeight, fDuration, nullptr, false))
			return pkSequence;
	}

	//if (currentMoveAnim)
	//	CopyPriorities(pkSequence, currentMoveAnim);

	SetAnimDataState(animData, pkSequence);
	return pkSequence;
}

