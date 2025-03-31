#include "blend_fixes.h"

#include "commands_animation.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "hooks.h"
#include "main.h"
#include "nihooks.h"
#include <array>
#include "SafeWrite.h"
#include "anim_fixes.h"
#include "NiObjects.h"


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
	animData->groupIDs[sequenceId] = animGroupId;
	if (resetSequenceState)
		animData->sequenceState1[sequenceId] = 0;
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

float GetIniBlend()
{
	return GetIniFloat(0x11C56FC);
}

void BlendFixes::FixAimPriorities(AnimData* animData, BSAnimGroupSequence* destAnim)
{
	return;
	// Fix priorities for aim to aim is when move sequence has higher priorities than aim
	// this happens when running in third person right and aiming down sights in and out
	// we achieve this by setting the move priorities to aim priorities - 1 if both are playing
	if (animData == g_thePlayer->firstPersonAnimData)
		return;
	const auto baseAnimGroup = static_cast<AnimGroupID>(destAnim->animGroup->groupID);
	const auto sequenceType = destAnim->animGroup->GetSequenceType();

	if (!destAnim->animGroup->IsBaseMovement() && baseAnimGroup != kAnimGroup_AimIS)
		return;

	BSAnimGroupSequence* moveAnim;
	if (sequenceType == kSequence_Movement)
		moveAnim = destAnim;
	else
		moveAnim = animData->animSequence[kSequence_Movement];

	if (!moveAnim || !moveAnim->animGroup)
		return;
	
	const auto* aimISAnim = GetActiveSequenceByGroupID(animData, kAnimGroup_AimIS);
	if (sequenceType == kSequence_Movement && !aimISAnim)
	{
		return;
	}

	BSAnimGroupSequence* aimAnim = GetActiveSequenceByGroupID(animData, kAnimGroup_Aim);
	if (!aimAnim || !aimAnim->animGroup)
		return;

	auto* tempBlendSequence = animData->controllerManager->FindSequence([](const NiControllerSequence* seq)
	{
		return sv::starts_with_ci(seq->m_kName.CStr(), "__");
	});

	const static NiFixedString objName = "Bip01 Spine1";
	// only affect upper body
	auto* bip01Spine = animData->nBip01->GetObjectByName(objName);
	if (!bip01Spine || !bip01Spine->GetAsNiNode())
		return;

	const auto processSequence = [&](const NiControllerSequence* sequenceToProcess)
	{
		if (!sequenceToProcess)
			return;
            
		const auto fn = [&](const NiAVObject* node)
		{
			auto* seqBlock = sequenceToProcess->GetControlledBlock(node->m_pcName);
			if (!seqBlock || !seqBlock->m_spInterpolator || !seqBlock->m_pkBlendInterp)
				return;
			auto* aimBlock = aimAnim->GetControlledBlock(node->m_pcName);
			if (!aimBlock || !aimBlock->m_spInterpolator || !aimBlock->m_pkBlendInterp
			   || aimBlock->m_ucPriority > seqBlock->m_ucPriority)
				return;
			const char newPriority = aimBlock->m_ucPriority != 0 ? static_cast<char>(aimBlock->m_ucPriority - 1) : 0;
			seqBlock->m_pkBlendInterp->SetPriority(newPriority, seqBlock->m_ucBlendIdx);
		};

		bip01Spine->GetAsNiNode()->RecurseTree(fn);
	};

	// Process both animations
	processSequence(moveAnim);
	processSequence(tempBlendSequence);

	const static auto sAimBlendFix = NiFixedString("__AimBlendFix__");
	moveAnim->m_spTextKeys->SetOrAddKey(sAimBlendFix, 1.0f);
}

void BlendFixes::RevertFixAimPriorities(AnimData* animData)
{
	auto* moveAnim = animData->animSequence[kSequence_Movement];
	if (!moveAnim || !moveAnim->animGroup)
		return;
	const static auto sAimBlendFix = NiFixedString("__AimBlendFix__");
	const auto fixKey = moveAnim->m_spTextKeys->FindFirstByName(sAimBlendFix);
	if (!fixKey || fixKey->m_fTime == 0.0f)
		return;

	auto* aimISAnim = GetActiveSequenceByGroupID(animData, kAnimGroup_AimIS);
	if (aimISAnim) 
		return;
	
	// revert it now that aimIS is done
	for (auto& controlledBlock : moveAnim->GetControlledBlocks())
	{
		if (!controlledBlock.m_spInterpolator || !controlledBlock.m_pkBlendInterp)
			continue;
		controlledBlock.m_pkBlendInterp->SetPriority(controlledBlock.m_ucPriority, controlledBlock.m_ucBlendIdx);
	}
	moveAnim->m_spTextKeys->SetOrAddKey(sAimBlendFix, 0.0f);
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

	SetCurrentSequence(animData, destAnim, true);

	const auto blendTime = destAnim->GetEaseInTime();
	
	if (srcAnim->m_eState != NiControllerSequence::EASEIN)
	{
		destAnim->Activate(0, true, destAnim->m_fSeqWeight, blendTime, nullptr, false);
		srcAnim->Deactivate(blendTime, false);
		return SKIP;
	}

	srcAnim->DeactivateNoReset(blendTime);

	if (destAnim->m_eState == NiControllerSequence::EASEOUT)
	{
		destAnim->ActivateNoReset(blendTime);
		return SKIP;
	}

	destAnim->Activate(0, true, destAnim->m_fSeqWeight, blendTime, nullptr, false);
	return SKIP;
}

#if 0
void TransitionToAttack(AnimData* animData, AnimGroupID currentGroupId, AnimGroupID targetGroupId)
{
	auto* currentSequence = GetAnimByGroupID(animData, currentGroupId);
	auto* targetSequence = GetAnimByGroupID(animData, targetGroupId);
	if (!targetSequence || !currentSequence || !currentSequence->animGroup)
		return;
	const auto blend = GetIniBlend();
	const auto currentTime = currentSequence->m_fLastScaledTime;
#if USE_BLEND_FROM_POSE
	if (targetSequence->m_eState != NiControllerSequence::INACTIVE)
		targetSequence->m_pkOwner->DeactivateSequence(targetSequence, 0.0f);
	targetSequence->m_pkOwner->BlendFromPose(targetSequence, currentTime, blend, 0, nullptr);
	if (currentSequence->m_eState != NiControllerSequence::INACTIVE)
		targetSequence->m_pkOwner->DeactivateSequence(currentSequence, 0.0f);
#else
	if (attackISSequence->state != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(attackISSequence->owner, attackISSequence, blend);
	if (attackSequence->state != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(attackSequence->owner, attackSequence, 0.0f);
	ApplyDestFrame(attackSequence, attackISTime / attackSequence->frequency);
	GameFuncs::ActivateSequence(attackSequence->owner, attackSequence, 0, true, attackSequence->seqWeight, blend, nullptr);
	FixConflictingPriorities(attackISSequence, attackSequence);
#endif
	//GameFuncs::CrossFade(attackISSequence->owner, attackISSequence, attackSequence, blend, 0, false, attackSequence->seqWeight, nullptr);

	HandleExtraOperations(animData, targetSequence);
	SetCurrentSequence(animData, targetSequence, false);

	const auto sequenceType = targetSequence->animGroup->GetGroupInfo()->sequenceType;
	if (animData != g_thePlayer->firstPersonAnimData && sequenceType >= kSequence_Weapon && sequenceType <= kSequence_WeaponDown)
	{
		// handle up down variant weights
		targetSequence->m_fSeqWeight = currentSequence->m_fSeqWeight;
		auto* highProcess = animData->actor->baseProcess;
		if (IS_TYPE(highProcess, HighProcess))
		{
			highProcess->weaponSequence[sequenceType - kSequence_Weapon] = targetSequence;
		}
	}
}
#endif

float GetKeyTime(BSAnimGroupSequence* anim, SequenceState1 keyTime)
{
	return anim->animGroup->keyTimes[keyTime];
}

#if 0
void BlendFixes::ApplyAttackISToAttackFix()
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
	const auto attackISGroupId = static_cast<AnimGroupID>(curGroupId);
	
	TransitionToAttack(animData3rd, attackISGroupId, attackGroupId);
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackISGroupId + 1), static_cast<AnimGroupID>(attackGroupId + 1)); // up
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackISGroupId + 2), static_cast<AnimGroupID>(attackGroupId + 2)); // down

	TransitionToAttack(g_thePlayer->firstPersonAnimData, attackISGroupId, attackGroupId);
	
	auto* attackSequence = GetAnimByGroupID(animData3rd, attackGroupId);
	const auto currentAnimAction = static_cast<Decoding::AnimAction>(g_thePlayer->GetHighProcess()->GetCurrentAnimAction());
	GameFuncs::Actor_SetAnimActionAndSequence(g_thePlayer, currentAnimAction, attackSequence);
}


void BlendFixes::ApplyAttackToAttackISFix()
{
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	auto* curWeaponAnim = animData3rd->animSequence[kSequence_Weapon];
	if (!curWeaponAnim || !curWeaponAnim->animGroup || !curWeaponAnim->animGroup->IsAttack() || !curWeaponAnim->animGroup->IsAttackNonIS())
		return;
	auto* highProcess = g_thePlayer->GetHighProcess();
	const auto curGroupId = curWeaponAnim->animGroup->groupID;
	if (!highProcess->isAiming)
		return;

	if (animData3rd->sequenceState1[kSequence_Weapon] < kSeqState_HitOrDetach)
		return;

	ThisStdCall(0x8BB650, g_thePlayer, true, false, false); // Actor::AimWeapon

	const auto attackGroupId = static_cast<AnimGroupID>(curGroupId);
	const auto attackISGroupId = static_cast<AnimGroupID>(curGroupId + 3);
	
	TransitionToAttack(animData3rd, attackGroupId, attackISGroupId);
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackGroupId + 1), static_cast<AnimGroupID>(attackISGroupId + 1)); // up
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackGroupId + 2), static_cast<AnimGroupID>(attackISGroupId + 2)); // down

	TransitionToAttack(g_thePlayer->firstPersonAnimData, attackGroupId, attackISGroupId);
	
	auto* attackISSequence = GetAnimByGroupID(animData3rd, attackISGroupId);
	const auto currentAnimAction = static_cast<Decoding::AnimAction>(g_thePlayer->GetHighProcess()->GetCurrentAnimAction());
	GameFuncs::Actor_SetAnimActionAndSequence(g_thePlayer, currentAnimAction, attackISSequence);
}
#endif
void BlendFixes::ApplyAimBlendHooks()
{
	// JMP 0x4996E7 -> 0x499709
	// disable the game resetting sequence state for aimup and aimdown as we will do it ourselves
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
    	const auto idleInterp = pkIdle->GetControlledBlock(tag.m_kAVObjectName);
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
