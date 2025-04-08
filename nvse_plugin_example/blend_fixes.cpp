#include "blend_fixes.h"

#include "commands_animation.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "hooks.h"
#include "main.h"
#include "nihooks.h"
#include <array>

#include "additive_anims.h"
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

	auto* aimISAnim = GetActiveSequenceByGroupID(animData, kAnimGroup_AimIS);

	if (destAnim->animGroup->GetBaseGroupID() != kAnimGroup_Aim || !aimISAnim)
		return;

	auto* tempBlendSequence = animData->controllerManager->FindSequence([](const NiControllerSequence* seq)
	{
		return sv::starts_with_ci(seq->m_kName.CStr(), "__");
	});
	if (!tempBlendSequence)
		return;

	for (auto& aimISTags : aimISAnim->GetIDTags())
	{
		auto* tempSeqControlledBlock = tempBlendSequence->GetControlledBlock(aimISTags.m_kAVObjectName);
		if (!tempSeqControlledBlock || !tempSeqControlledBlock->m_spInterpolator || !tempSeqControlledBlock->m_pkBlendInterp)
			continue;
		tempSeqControlledBlock->m_pkBlendInterp->RemoveInterpInfo(tempSeqControlledBlock->m_ucBlendIdx);
		tempSeqControlledBlock->m_pkBlendInterp = nullptr;
		tempSeqControlledBlock->m_ucBlendIdx = NiBlendInterpolator::INVALID_INDEX;
	}
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

struct WeightSmoothingData
{
	NiPointer<NiInterpolator> interpolator = nullptr;
	float previousSmoothedWeight = 0.0f;
	float updateTime = -NI_INFINITY;
	bool isActive = false;
	bool isRemoved = false;
	bool queueDelete = false;
	unsigned char blendIndex = 0xFF;
};

thread_local std::unordered_map<NiInterpolator*, std::shared_ptr<WeightSmoothingData>> g_interpToWeightSmoothingDataMap;

struct WeightSmoothingDataItems
{
	std::vector<std::shared_ptr<WeightSmoothingData>> items;
	bool isFirstFrame = true;

	WeightSmoothingData& GetItem(const NiBlendInterpolator::InterpArrayItem& item)
	{
		const auto iter = std::ranges::find_if(items, [&](const auto& it)
		{
			return it->interpolator == item.m_spInterpolator;
		});
		if (iter == items.end())
		{
			const auto weightSmoothingData = std::make_shared<WeightSmoothingData>(WeightSmoothingData{
				.interpolator = item.m_spInterpolator,
				.previousSmoothedWeight = item.m_fNormalizedWeight,
				.updateTime = item.m_fUpdateTime,
				.isActive = false,
				.isRemoved = false,
				.queueDelete = false,
				.blendIndex = 0xFF
			});
			items.push_back(weightSmoothingData);
			g_interpToWeightSmoothingDataMap[item.m_spInterpolator] = weightSmoothingData;
			return *weightSmoothingData;
		}
		return **iter;
	}

	void ResetActive()
	{
		for (auto& item : items)
		{
			item->isActive = false;
		}
	}
};

thread_local std::unordered_map<NiBlendInterpolator*, WeightSmoothingDataItems> g_weightSmoothingDataMap;
// TODO handle deletes (possibly on another thread)

void BlendFixes::ApplyWeightSmoothing(NiBlendInterpolator* blendInterpolator)
{
	auto& data = g_weightSmoothingDataMap[blendInterpolator];
	if (data.isFirstFrame)
	{
		data.items.reserve(blendInterpolator->m_ucArraySize);
		for (auto& item : blendInterpolator->GetItems())
		{
			if (!item.m_spInterpolator || AdditiveManager::IsAdditiveInterpolator(item.m_spInterpolator))
				continue;
			const auto dataItem = std::make_shared<WeightSmoothingData>(WeightSmoothingData{
				.interpolator = item.m_spInterpolator,
				.previousSmoothedWeight = item.m_fNormalizedWeight,
				.updateTime = item.m_fUpdateTime,
				.isActive = false,
				.isRemoved = false,
				.queueDelete = false,
				.blendIndex = 0xFF
			});
			data.items.push_back(dataItem);
			g_interpToWeightSmoothingDataMap[item.m_spInterpolator] = dataItem;
		}
		data.isFirstFrame = false;
		return;
	}

	// check duplicates
	for (auto& dataItemPtr : data.items)
	{
		auto& dataItem = *dataItemPtr;
		unsigned int count = 0;
		for (auto& item : blendInterpolator->GetItems())
		{
			if (!item.m_spInterpolator)
				continue;		
			if (dataItem.interpolator == item.m_spInterpolator)
				count++;
			if (count > 1)
			{
				if (!dataItem.isRemoved || dataItem.blendIndex == 0xFF || dataItem.blendIndex >= blendInterpolator->m_ucArraySize)
				{
					DebugBreak();
				}
				blendInterpolator->RemoveInterpInfo(dataItem.blendIndex);
				dataItem.isRemoved = false;
				dataItem.blendIndex = 0xFF;
				break;
			}
		}
	}
	
	const auto deltaTime = g_timeGlobal->secondsPassed;
	constexpr auto smoothingTime = 0.1f;
	const auto smoothingRate = 1.0f - std::exp(-deltaTime / smoothingTime);
	//const auto smoothingRate = std::clamp(deltaTime / smoothingTime, 0.0f, 1.0f);

	data.ResetActive();

	float totalWeight = 0.0f;
	constexpr float MIN_WEIGHT = 0.001f;
	for (auto& item : blendInterpolator->GetItems())
	{
		if (!item.m_spInterpolator || AdditiveManager::IsAdditiveInterpolator(item.m_spInterpolator))
			continue;
		auto& dataItem = data.GetItem(item);
		dataItem.isActive = true;
		if (dataItem.previousSmoothedWeight == 0.0f && item.m_fNormalizedWeight == 0.0f)
			continue;
		
		dataItem.updateTime = item.m_fUpdateTime;
		const auto targetWeight = item.m_fNormalizedWeight;
		const auto smoothedWeight = std::lerp(dataItem.previousSmoothedWeight, targetWeight, smoothingRate);
		if (smoothedWeight < 0.0f)
			DebugBreak();
		dataItem.previousSmoothedWeight = smoothedWeight;
		item.m_fNormalizedWeight = smoothedWeight;
		if (smoothedWeight < MIN_WEIGHT)
		{
			dataItem.previousSmoothedWeight = 0.0f;
			item.m_fNormalizedWeight = 0.0f;
			continue;
		}
		totalWeight += smoothedWeight;
	}

	for (auto& dataItemPtr : data.items)
	{
		auto& dataItem = *dataItemPtr;
		if (dataItem.isActive || dataItem.isRemoved || dataItem.previousSmoothedWeight == 0.0f)
			continue;
		const auto blendItems = blendInterpolator->GetItems();
		if (std::ranges::find_if(blendItems, [&](const auto& item)
		{
			return item.m_spInterpolator == dataItem.interpolator;
		}) != blendItems.end())
			DebugBreak();
		dataItem.isRemoved = true;
		dataItem.blendIndex = blendInterpolator->AddInterpInfo(dataItem.interpolator, 0.0f, 0, 0.0f);
		auto& item = blendInterpolator->GetItems()[dataItem.blendIndex];
		constexpr auto targetWeight = 0.0f;
		const auto smoothedWeight = std::lerp(dataItem.previousSmoothedWeight, targetWeight, smoothingRate);
		if (smoothedWeight < 0.0f)
			DebugBreak();
		item.m_fUpdateTime = dataItem.updateTime;
		if (smoothedWeight < MIN_WEIGHT)
		{
			dataItem.previousSmoothedWeight = 0.0f;
			item.m_fNormalizedWeight = 0.0f;
			continue;
		}
		dataItem.previousSmoothedWeight = smoothedWeight;
		item.m_fNormalizedWeight = smoothedWeight;
		totalWeight += smoothedWeight;
	}

	for (auto& dataItemPtr : data.items)
	{
		auto& dataItem = *dataItemPtr;
		if (!dataItem.isRemoved)
			continue;
		
		if (dataItem.previousSmoothedWeight < MIN_WEIGHT)
		{
			dataItem.previousSmoothedWeight = 0.0f;
			if (dataItem.blendIndex == 0xFF || dataItem.blendIndex >= blendInterpolator->m_ucArraySize)
				DebugBreak();
			blendInterpolator->RemoveInterpInfo(dataItem.blendIndex);
			dataItem.isRemoved = false;
			dataItem.blendIndex = 0xFF;
		}
	}

	for (auto& itemPtr : data.items)
	{
		auto& item = *itemPtr;
		if (item.queueDelete && item.previousSmoothedWeight == 0.0f)
			g_interpToWeightSmoothingDataMap.erase(item.interpolator);
	}

	std::erase_if(data.items, [&](auto& item)
	{
		return item->queueDelete && item->previousSmoothedWeight == 0.0f;
	});

	float newTotalWeight = 0.0f;
	
	if (totalWeight > 0.0f)
	{
		// renormalize weights
		const auto invTotalWeight = 1.0f / totalWeight;
		for (auto& item : blendInterpolator->GetItems())
		{
			if (!item.m_spInterpolator || AdditiveManager::IsAdditiveInterpolator(item.m_spInterpolator))
				continue;
			item.m_fNormalizedWeight *= invTotalWeight;
			newTotalWeight += item.m_fNormalizedWeight;
		}
	}

	if (newTotalWeight != 0.0f && std::abs(newTotalWeight - 1.0f) > 0.001f)
		DebugBreak();
}

NiInterpolator* CreatePoseInterpolator(NiInterpController* controller, const NiControllerSequence::IDTag& idTag)
{
	const auto index = controller->GetInterpolatorIndexByIDTag(&idTag);
	if (index == 0xFFFF)
		return nullptr;
	return controller->CreatePoseInterpolator(index);
}

struct SecondaryTempInterpolatorData
{
	unsigned char blendIndex = 0xFF;
};

thread_local std::unordered_map<NiInterpolator*, SecondaryTempInterpolatorData> g_secondaryTempInterpolatorDataMap;

void BlendFixes::AttachSecondaryTempInterpolators(NiControllerSequence* pkSequence)
{
	for (auto& block : pkSequence->GetControlledBlocks())
	{
		auto* blendInterp = block.m_pkBlendInterp;
		if (!block.m_spInterpolator || !blendInterp)
			continue;
		if (blendInterp->m_ucInterpCount == 0)
		{
			const auto& idTag = block.GetIDTag(pkSequence);
			auto* poseInterp = CreatePoseInterpolator(block.m_spInterpCtlr, idTag);
			if (!poseInterp)
				continue;
			if (poseInterp == block.m_spInterpolator)
				DebugBreak();
			auto& data = g_secondaryTempInterpolatorDataMap[block.m_spInterpolator];
			data.blendIndex = blendInterp->AddInterpInfo(poseInterp, 1.0f, 0, 1.0f);
		}
		else if (blendInterp->m_ucInterpCount == 1)
		{
			if (auto iter = g_secondaryTempInterpolatorDataMap.find(blendInterp->m_pkSingleInterpolator); iter != g_secondaryTempInterpolatorDataMap.end())
			{
				block.m_pkBlendInterp->RemoveInterpInfo(iter->second.blendIndex);
				g_secondaryTempInterpolatorDataMap.erase(iter);
			}
		}
	}
}

void BlendFixes::DetachSecondaryTempInterpolators(NiControllerSequence* pkSequence)
{
	for (auto& block : pkSequence->GetControlledBlocks())
	{
		auto* blendInterpolator = block.m_pkBlendInterp;
		if (!block.m_spInterpolator || !blendInterpolator)
			continue;
		if (auto iter = g_secondaryTempInterpolatorDataMap.find(block.m_spInterpolator); iter != g_secondaryTempInterpolatorDataMap.end())
		{
			blendInterpolator->RemoveInterpInfo(iter->second.blendIndex);
			g_secondaryTempInterpolatorDataMap.erase(iter);
		}
		if (blendInterpolator->m_ucInterpCount == 2) // going to be 1 since this is going to be removed
		{
			const auto blendInterpItems = blendInterpolator->GetItems();
			auto otherInterpItem = std::ranges::find_if(blendInterpItems, [&](const auto& item)
			{
				return item.m_spInterpolator && item.m_spInterpolator != block.m_spInterpolator;
			});
			if (otherInterpItem == blendInterpItems.end())
				DebugBreak();

			const auto& idTag = block.GetIDTag(pkSequence);
			auto* poseInterpolator = CreatePoseInterpolator(block.m_spInterpCtlr, idTag);
			if (!poseInterpolator)
				continue;
			if (poseInterpolator == otherInterpItem->m_spInterpolator)
				DebugBreak();
			auto& data = g_secondaryTempInterpolatorDataMap[otherInterpItem->m_spInterpolator];
			data.blendIndex = blendInterpolator->AddInterpInfo(poseInterpolator, 1.0f, 0, 1.0f);
		}
	}
}

void BlendFixes::OnSequenceDestroy(NiControllerSequence* sequence)
{
}

void BlendFixes::OnInterpolatorDestroy(NiInterpolator* interpolator)
{
	if (!g_pluginSettings.blendSmoothing)
		return;
	g_secondaryTempInterpolatorDataMap.erase(interpolator);

	if (const auto iter = g_interpToWeightSmoothingDataMap.find(interpolator); iter != g_interpToWeightSmoothingDataMap.end())
	{
		iter->second->queueDelete = true;
	}
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
		srcAnim->Deactivate(blendTime, false);
		destAnim->ActivateBlended(0, true, destAnim->m_fSeqWeight, blendTime, nullptr, false);
		return SKIP;
	}

	srcAnim->DeactivateNoReset(blendTime);

	if (destAnim->m_eState == NiControllerSequence::EASEOUT)
	{
		destAnim->ActivateNoReset(blendTime);
		return SKIP;
	}

	destAnim->ActivateBlended(0, true, destAnim->m_fSeqWeight, blendTime, nullptr, false);
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
	// NiControllerSequence::DetachInterpolators
	static UInt32 uiDetachInterpolatorsAddr = 0xA30540;
	WriteRelCall(0xA350C5, INLINE_HOOK(void, __fastcall, NiControllerSequence* pkSequence)
	{
		//if (!AdditiveManager::IsAdditiveSequence(pkSequence))
		if (g_pluginSettings.blendSmoothing)
			DetachSecondaryTempInterpolators(pkSequence);
		ThisStdCall(uiDetachInterpolatorsAddr, pkSequence);
	}));
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
