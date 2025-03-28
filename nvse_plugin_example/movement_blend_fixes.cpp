#include "movement_blend_fixes.h"

#include <algorithm>

#include "blend_fixes.h"
#include "NiObjects.h"

void SetAnimDataState(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	constexpr auto sequenceId = kSequence_Movement;
	animData->groupIDs[sequenceId] = pkSequence->animGroup->groupID;
	animData->animSequence[sequenceId] = pkSequence;
	animData->sequenceState1[sequenceId] = 0;
}

BSAnimGroupSequence* MovementBlendFixes::PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* pkSequence)
{
	auto* manager = animData->controllerManager;
	const float fDuration = pkSequence->GetEaseInTime();
	const static auto sTempBlendSequenceName = NiFixedString("__TempBlendSequence__");
	const auto existingTempBlendSeq = manager->FindSequence([](const NiControllerSequence* seq)
	{
		return seq->m_kName == sTempBlendSequenceName;
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
		
	if (!currentMoveAnim)
		currentMoveAnim = existingTempBlendSeq;

	if (auto* currAnim = animData->animSequence[kSequence_Movement]; currAnim && currAnim->animGroup && currAnim->animGroup->IsTurning())
		currAnim->Deactivate(0.0f, false);

#if 0
	const auto hasInactiveBlocks = std::ranges::any_of(pkSequence->GetControlledBlocks(), [](const auto& block) {
		return block.m_pkBlendInterp && block.m_pkBlendInterp->m_ucInterpCount == 0;
	});
	if (existingTempBlendSeq)
		existingTempBlendSeq->Deactivate(0.0f, false);
	
	if (hasInactiveBlocks && !currentMoveAnim)
	{
		// prevent snapping for nodes that weren't previously being animated
		auto* tempBlendSeq = manager->CreateTempBlendSequence(pkSequence, nullptr);
		for (auto& block : tempBlendSeq->GetControlledBlocks())
		{
			if (!block.m_pkBlendInterp)
				continue;
			if (block.m_pkBlendInterp->m_ucInterpCount != 0)
				tempBlendSeq->RemoveInterpolator(&block - tempBlendSeq->m_pkInterpArray);
		}
		const static auto sBip01 = NiFixedString("Bip01");
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
		if (!pkSequence->ActivateNoReset(fDuration, false))
			return pkSequence;
	}
	else
	{
		if (currentMoveAnim)
		{
			auto* tmpBlendSeq = manager->CreateTempBlendSequence(pkSequence, nullptr);
			tmpBlendSeq->PopulateIDTags(pkSequence);
			tmpBlendSeq->RemoveInterpolator("Bip01");
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

#if 0
	auto activeSequences = manager->m_kActiveSequences.ToSpan();
	const auto lastMoveSequence = std::ranges::find_if(activeSequences, [&](NiControllerSequence* sequence)
	{
		if (NOT_TYPE(sequence, BSAnimGroupSequence))
			return false;
		TESAnimGroup* animGroup = static_cast<BSAnimGroupSequence*>(sequence)->animGroup;
		if (!animGroup)
			return false;
		return animGroup->GetSequenceType() == kSequence_Movement && sequence->m_eState == NiControllerSequence::EASEOUT;
	});

	if (lastMoveSequence != activeSequences.end())
	{
		auto idleSequence = std::ranges::find_if(activeSequences, [&](NiControllerSequence* sequence)
		{
			if (NOT_TYPE(sequence, BSAnimGroupSequence))
				return false;
			TESAnimGroup* animGroup = static_cast<BSAnimGroupSequence*>(sequence)->animGroup;
			if (!animGroup)
				return false;
			return animGroup->GetSequenceType() == kSequence_Idle && sequence->m_eState == NiControllerSequence::ANIMATING;
		});
		if (idleSequence != activeSequences.end())
		{
			FixConflictingPriorities(*lastMoveSequence, pkSequence, *idleSequence);
		}
	}
#endif

	SetAnimDataState(animData, pkSequence);
	
	return pkSequence;
}

NiControllerSequence* MovementBlendFixes::CreateExtraIdleSequence(AnimData* animData, BSAnimGroupSequence* pkIdleSequence)
{
	auto* bip01 = animData->nBip01;
	auto* interpIdleSequence = NiControllerSequence::Create("InactiveInterpIdleSequence", 80, 12);

	auto processNode = [&](auto&& self, NiAVObject* node) -> void
	{
		if (!node)
			return;

		if (pkIdleSequence->GetControlledBlock(node->m_pcName))
			return;
        
		NiQuatTransform kTransform;
		kTransform.m_kRotate.FromRotation(node->GetRotate());
		kTransform.m_kTranslate = node->GetTranslate();
		kTransform.m_fScale = node->GetScale();
		auto* transform = NiTransformInterpolator::Create(kTransform);
		const static auto sTransformController = NiFixedString("NiTransformController");
		auto idTag = NiControllerSequence::IDTag {
			.m_kAVObjectName = node->m_pcName,
			.m_kPropertyType = nullptr,
			.m_kCtlrType = sTransformController,
			.m_kCtlrID = nullptr,
			.m_kInterpolatorID = nullptr
		};
		interpIdleSequence->AddInterpolator(transform, idTag, 0);

		auto* niNode = node->GetAsNiNode();
		if (niNode)
		{
			for (NiAVObject* child : niNode->m_children)
			{
				self(self, child);
			}
		}
	};

	for (NiAVObject* child : bip01->m_children)
	{
		processNode(processNode, child);
	}

	animData->controllerManager->AddSequence(interpIdleSequence, nullptr, false);
	interpIdleSequence->Activate(0, true, 1.0f, 0.0f, nullptr, false);
	
	return interpIdleSequence;
}
