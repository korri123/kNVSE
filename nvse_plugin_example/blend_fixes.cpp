#include "blend_fixes.h"

#include "commands_animation.h"
#include "GameObjects.h"
#include "hooks.h"
#include "main.h"
#include "nihooks.h"
#include <array>

#include "SafeWrite.h"
#include "anim_fixes.h"


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

std::vector<BSAnimGroupSequence*> GetActiveAttackSequences(AnimData* animData)
{
	return GetActiveSequencesWhere(animData, [&](BSAnimGroupSequence* sequence)
	{
		return IsAnimGroupAttack(sequence->animGroup->GetBaseGroupID());
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

	const auto blendTime = destAnim->GetEaseInTime();

	if (srcAnim->m_eState != NiControllerSequence::EASEIN)
	{
		srcAnim->Deactivate(blendTime, false);
		destAnim->Activate(0, true, destAnim->m_fSeqWeight, blendTime, nullptr, false);
		return SKIP;
	}

	if (destAnim->m_eState == NiControllerSequence::EASEOUT)
	{
		srcAnim->DeactivateNoReset(blendTime, false);
		destAnim->ActivateNoReset(blendTime, false);
		return SKIP;
	}

	animData->controllerManager->CrossFade(srcAnim, destAnim, blendTime, 0, true, destAnim->m_fSeqWeight);
	return SKIP;
}

#if 1

bool TransitionToAttack(AnimData* animData, AnimGroupID currentGroupId, AnimGroupID targetGroupId)
{
	auto* currentSequence = GetAnimByGroupID(animData, currentGroupId);
	auto* targetSequence = GetAnimByGroupID(animData, targetGroupId);
	if (!targetSequence || !currentSequence || !currentSequence->animGroup)
		return false;
	const auto blend = GetDefaultBlendTime(targetSequence, currentSequence);

	if (currentSequence->m_eState == NiControllerSequence::ANIMATING && targetSequence->m_eState == NiControllerSequence::INACTIVE)
	{
		const auto destFrame = currentSequence->m_fLastScaledTime / currentSequence->m_fEndKeyTime * targetSequence->m_fEndKeyTime;
		currentSequence->StartBlend(targetSequence, blend, destFrame, 0, currentSequence->m_fSeqWeight, targetSequence->m_fSeqWeight, nullptr);
	}
	else
	{
		return false;
	}

	HandleExtraOperations(animData, targetSequence);
	animData->SetCurrentSequence(targetSequence, false);

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
	return true;
}
#endif

#if 1


void BlendFixes::ApplyAttackISToAttackFix()
{
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	auto* animData1st = g_thePlayer->firstPersonAnimData;
	auto* animData = g_thePlayer->IsThirdPerson() ? animData3rd : animData1st;
	
	auto* curWeaponAnim = animData->animSequence[kSequence_Weapon];
	if (!curWeaponAnim || !curWeaponAnim->animGroup || !curWeaponAnim->animGroup->IsAttackIS())
		return;
	auto* highProcess = g_thePlayer->GetHighProcess();
	const auto curGroupId = curWeaponAnim->animGroup->groupID;
	if (highProcess->isAiming)
		return;

	if (animData3rd->sequenceState1[kSequence_Weapon] < kSeqState_HitOrDetach)
		return;

	// ThisStdCall(0x8BB650, g_thePlayer, false, false, false); // Actor::AimWeapon

	const auto attackGroupId = static_cast<AnimGroupID>(curGroupId - 3);
	const auto attackISGroupId = static_cast<AnimGroupID>(curGroupId);
	
	auto result = TransitionToAttack(animData3rd, attackISGroupId, attackGroupId);
	if (!result && g_thePlayer->IsThirdPerson())
		return;
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackISGroupId + 1), static_cast<AnimGroupID>(attackGroupId + 1)); // up
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackISGroupId + 2), static_cast<AnimGroupID>(attackGroupId + 2)); // down

	result = TransitionToAttack(animData1st, attackISGroupId, attackGroupId);
	if (!result && !g_thePlayer->IsThirdPerson())
		return;
	
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

	// ThisStdCall(0x8BB650, g_thePlayer, true, false, false); // Actor::AimWeapon

	const auto attackGroupId = static_cast<AnimGroupID>(curGroupId);
	const auto attackISGroupId = static_cast<AnimGroupID>(curGroupId + 3);
	
	auto result = TransitionToAttack(animData3rd, attackGroupId, attackISGroupId);
	if (!result && g_thePlayer->IsThirdPerson())
		return;

	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackGroupId + 1), static_cast<AnimGroupID>(attackISGroupId + 1)); // up
	TransitionToAttack(animData3rd, static_cast<AnimGroupID>(attackGroupId + 2), static_cast<AnimGroupID>(attackISGroupId + 2)); // down

	result = TransitionToAttack(g_thePlayer->firstPersonAnimData, attackGroupId, attackISGroupId);
	if (!result && !g_thePlayer->IsThirdPerson())
		return;

	auto* attackISSequence = GetAnimByGroupID(animData3rd, attackISGroupId);
	const auto currentAnimAction = static_cast<Decoding::AnimAction>(g_thePlayer->GetHighProcess()->GetCurrentAnimAction());
	GameFuncs::Actor_SetAnimActionAndSequence(g_thePlayer, currentAnimAction, attackISSequence);
}
#endif
void BlendFixes::ApplyAimBlendHooks()
{
	// JMP 0x4996E7 -> 0x499709
	// disable the game resetting sequence state for aimup and aimdown as we will do it ourselves
	if (g_pluginSettings.fixSpineBlendBug)
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
