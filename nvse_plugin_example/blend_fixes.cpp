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
			GameFuncs::DeactivateSequence(upAnim->m_pkOwner, upAnim, 0.0f);
		if (downAnim)
			GameFuncs::DeactivateSequence(downAnim->m_pkOwner, downAnim, 0.0f);
	}
}

float GetIniBlend()
{
	return GetIniFloat(0x11C56FC);
}

std::unordered_map<NiControllerManager*, std::array<NiControllerSequence*, 8>> g_tempBlendSequences;

void ManageTempBlendSequence(BSAnimGroupSequence* destAnim)
{
	const auto sequenceType = destAnim->animGroup->GetGroupInfo()->sequenceType;
	auto* manager = destAnim->m_pkOwner;
	auto* lastTempBlendSequence = g_lastTempBlendSequence[manager];
	auto& tempBlendSequenceArray = g_tempBlendSequences[manager];
	auto* currentSequence = tempBlendSequenceArray[sequenceType];
	if (currentSequence && currentSequence != lastTempBlendSequence && currentSequence->m_eState != kAnimState_Inactive)
		GameFuncs::DeactivateSequence(manager, currentSequence, 0.0f);
	tempBlendSequenceArray[sequenceType] = lastTempBlendSequence;
}

float CalculateTransitionBlendTime(AnimData* animData, BSAnimGroupSequence* src, BSAnimGroupSequence* dst)
{
	const auto blend = GetIniBlend();
	const auto* blockName = "Bip01 R Hand";
	auto* sceneRoot = animData->nSceneRoot;
	auto* weaponNode = sceneRoot->GetBlock(blockName);
	if (!weaponNode)
		return blend;
	const auto& weaponPoint = weaponNode->m_kLocal.m_Translate;
	auto* srcInterpItem = src->GetControlledBlock(blockName);
	auto* destInterpItem = dst->GetControlledBlock(blockName);
	if (!srcInterpItem || !destInterpItem)
		return blend;
	auto* srcInterp = srcInterpItem->m_spInterpolator;
	auto* destInterp = destInterpItem->m_spInterpolator;
	if (!srcInterp || !destInterp)
		return blend;
	NiQuatTransform destTransform;
	NiQuatTransform srcTransform;
	if (!srcInterp->Update(0.0f, sceneRoot, srcTransform) || !destInterp->Update(0.0f, sceneRoot, destTransform))
		return blend;
	const auto& srcPoint = srcTransform.m_kTranslate;
	const auto& dstPoint = destTransform.m_kTranslate;

	// calculate distance
	const auto ab = dstPoint - srcPoint;
	const auto ac = weaponPoint - srcPoint;
	const auto lerpValue = ab.Dot(ac) / ab.SqrLength();
	const auto result = blend * lerpValue;
	return blend - result;
}

#define USE_BLEND_FROM_POSE 1

BlendFixes::Result BlendFixes::ApplyAimBlendFix(AnimData* animData, BSAnimGroupSequence* destAnim)
{
	if (!destAnim || !destAnim->animGroup)
		return RESUME;

	const auto* destInfo = destAnim->animGroup->GetGroupInfo();
	const auto sequenceId = static_cast<eAnimSequence>(destInfo->sequenceType);
	if (sequenceId == kSequence_None)
		return RESUME;

	const auto destGroupId = destAnim->animGroup->groupID;
	const auto destBaseGroupId = static_cast<AnimGroupID>(destGroupId & 0xFF);
	const auto isDestAim = IsAnimGroupAim(destBaseGroupId);
	if (!isDestAim)
		return RESUME;

	BSAnimGroupSequence* srcAnim = animData->animSequence[sequenceId];

	const auto isDestUpOrDown = sequenceId == kSequence_WeaponUp || sequenceId == kSequence_WeaponDown;

	if (!srcAnim || !srcAnim->animGroup || srcAnim == destAnim)
		return RESUME;


	if (!isDestAim && !isDestUpOrDown)
		return RESUME;

	const auto isSrcEasing = srcAnim->m_eState == kAnimState_EaseIn || srcAnim->m_eState == kAnimState_TransDest;
	
	//if (!isSrcEasing && ((isDestIS && isSrcIS) || (!isDestIS && !isSrcIS)))
	//	return RESUME;
	if (!isSrcEasing)
		return RESUME;

	if (animData != g_thePlayer->firstPersonAnimData)
	{
		// fix variant bs
		const auto* baseProcess = animData->actor->baseProcess;
		const auto* currentAnim = baseProcess->weaponSequence[sequenceId - 4];
		if (currentAnim)
			destAnim->m_fSeqWeight = currentAnim->m_fSeqWeight;
	}
#if USE_BLEND_FROM_POSE
	auto* aimAnim = GetAnimByGroupID(animData, kAnimGroup_Aim);
	auto* aimISAnim = GetAnimByGroupID(animData, kAnimGroup_AimIS);
	float blend;

	const auto toIS = IsAnimGroupIS(destBaseGroupId);


	if (aimAnim && aimISAnim)
	{
		if (toIS)
			blend = CalculateTransitionBlendTime(animData, aimAnim, aimISAnim);
		else
			blend = CalculateTransitionBlendTime(animData, aimISAnim, aimAnim);
	}
	else
		blend = GetIniBlend();
	
	if (destAnim->m_eState != kAnimState_Inactive)
		destAnim->m_pkOwner->DeactivateSequence(destAnim, 0.0f);
	
	if (srcAnim->m_eState != kAnimState_Inactive)
		srcAnim->m_pkOwner->DeactivateSequence(srcAnim, 0.0f);

	destAnim->m_pkOwner->BlendFromPose(destAnim, 0.0f, blend, 0, nullptr);

	ManageTempBlendSequence(destAnim);
	
	SetCurrentSequence(animData, destAnim, true);
#else
	auto currentAnimTime = GetAnimTime(animData, srcAnim);
	auto blend = GetIniBlend();
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
	if (isDestUpOrDown)//
	{
		// really annoyed with these desyncing
		const auto* weaponAnim = animData->animSequence[kSequence_Weapon];
		if (weaponAnim) // shouldn't ever be null
			destFrame = GetAnimTime(animData, weaponAnim);
	}
	ApplyDestFrame(destAnim, destFrame / destAnim->frequency);
	ApplyDestFrame(srcAnim, destFrame / srcAnim->frequency);
	GameFuncs::ActivateSequence(destAnim->owner, destAnim, 0, true, destAnim->seqWeight, blend, nullptr);
	FixConflictingPriorities(srcAnim, destAnim);
	SetCurrentSequence(animData, destAnim, false);
	if (animData != g_thePlayer->firstPersonAnimData)
		Console_Print("AimBlendFix: %s -> %s (%.4f)", srcAnim->animGroup->GetGroupInfo()->name, destAnim->animGroup->GetGroupInfo()->name, destFrame);
#endif
	
	return SKIP;
}

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

float GetKeyTime(BSAnimGroupSequence* anim, SequenceState1 keyTime)
{
	return anim->animGroup->keyTimes[keyTime];
}

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

void BlendFixes::ApplyAimBlendHooks()
{
	// JMP 0x4996E7 -> 0x499709
	// disable the game resetting sequence state for aimup and aimdown as we will do it ourselves
	SafeWriteBuf(0x4996E7, "\xEB\x20\x90\x90", 4);
}


void FixConflictingPriorities(NiControllerSequence* pkSource, NiControllerSequence* pkDest)
{
    const auto sourceControlledBlocks = pkSource->GetControlledBlocks();
    std::unordered_map<NiBlendInterpolator*, NiControllerSequence::InterpArrayItem*> sourceInterpMap;
    for (auto& interp : sourceControlledBlocks)
    {
        if (!interp.m_spInterpolator || !interp.m_pkBlendInterp || interp.m_ucPriority == 0xFF)
            continue;
        sourceInterpMap.emplace(interp.m_pkBlendInterp, &interp);
    }
    const auto destControlledBlocks = pkDest->GetControlledBlocks();
    const auto tags = pkDest->GetIDTags();
    auto index = 0;

    const static std::unordered_set<std::string_view> s_ignoredInterps = {
        "Bip01 NonAccum", "Bip01 Translate", "Bip01 Rotate", "Bip01"
    };
    for (auto& destBlock : destControlledBlocks)
    {
        const auto& tag = tags[index++];
        auto blendInterpolator = destBlock.m_pkBlendInterp;
        if (!destBlock.m_spInterpolator || !blendInterpolator || destBlock.m_ucPriority == 0xFF)
            continue;
        if (s_ignoredInterps.contains(tag.m_kAVObjectName.CStr()))
            continue;
        if (auto it = sourceInterpMap.find(blendInterpolator); it != sourceInterpMap.end())
        {
            const auto& sourceInterp = *it->second;
            if (destBlock.m_ucPriority != sourceInterp.m_ucPriority)
                continue;
            std::span blendInterpItems(blendInterpolator->m_pkInterpArray, blendInterpolator->m_ucArraySize);
            auto destInterpItem = std::ranges::find_if(blendInterpItems, [&](const NiBlendInterpolator::InterpArrayItem& item)
            {
                return item.m_spInterpolator == destBlock.m_spInterpolator;
            });
            if (destInterpItem == blendInterpItems.end())
                continue;
            const auto newPriority = ++destInterpItem->m_cPriority;
            if (newPriority > blendInterpolator->m_cHighPriority)
            {
                blendInterpolator->m_cHighPriority = newPriority;
                blendInterpolator->m_cNextHighPriority = destBlock.m_ucPriority;
            }
        }
    }
}

void BlendFixes::FixConflictingPriorities(BSAnimGroupSequence* pkSource, BSAnimGroupSequence* pkDest)
{
	if (!g_pluginSettings.fixBlendSamePriority || !pkDest || !pkSource || !pkDest->animGroup || !pkSource->animGroup)
		return;
	auto* groupInfo = pkDest->animGroup->GetGroupInfo();
	if (!groupInfo)
		return;
	if (groupInfo->sequenceType != kSequence_Weapon)
		return;
	const auto srcGroupId = pkSource->animGroup->GetBaseGroupID();
	const auto destGroupId = pkDest->animGroup->GetBaseGroupID();
	
	if (!(srcGroupId == destGroupId ||
	  (srcGroupId == kAnimGroup_Aim && destGroupId == kAnimGroup_AimIS) ||
	  (srcGroupId == kAnimGroup_AimIS && destGroupId == kAnimGroup_Aim) ||
	  (pkSource->animGroup->IsAttackNonIS() && pkSource->animGroup->IsAttackIS()) ||
	  (pkSource->animGroup->IsAttackIS() && pkSource->animGroup->IsAttackNonIS()) ||
	  (pkSource->animGroup->IsLoopingReloadStart() && pkSource->animGroup->IsLoopingReload())))
	{
		return;
	}
	if (HasNoFixTextKey(pkDest))
		return;
	if (pkSource->m_eState == NiControllerSequence::EASEOUT && pkDest->m_eState == NiControllerSequence::EASEIN)
		::FixConflictingPriorities(pkSource, pkDest);
}

void BlendFixes::ApplyHooks()
{
	return;
#if 0
	// AnimData::GetSceneRoot
	WriteRelCall(0x896162, INLINE_HOOK(NiNode*, __fastcall, AnimData* animData)
	{
		auto* anim = animData->animSequence[kSequence_Movement];
		auto* result = animData->nSceneRoot;
		if (!anim || !anim->animGroup)
			return result;
		auto* block = anim->GetControlledBlock("Weapon");
		if (!block)
			return result;
		auto* interp = block->m_spInterpolator;
		if (NOT_TYPE(interp, NiTransformInterpolator))
			return result;
		interp->Pause();
		return result;
	}));
#endif
}
