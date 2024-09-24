#include "anim_fixes.h"

#include <ranges>

#include "class_vtbls.h"
#include "game_types.h"
#include "hooks.h"
#include "SafeWrite.h"
#include "utility.h"

void LogAnimError(const BSAnimGroupSequence* anim, const std::string& msg)
{
	ERROR_LOG("Fixed error in animation: " + std::string(anim->m_kName) + "\n\t" + msg);
}

void LogAnimError(const char* anim, const std::string& msg)
{
	ERROR_LOG("Fixed error in animation: " + std::string(anim) + "\n\t" + msg);
}

bool HasNoFixTextKey(const BSAnimGroupSequence* anim)
{
	if (!anim->m_spTextKeys || anim->m_spTextKeys->FindFirstByName("noFix"))
		return true;
	return false;
}

void AnimFixes::FixInconsistentEndTime(BSAnimGroupSequence* anim)
{
	if (!g_pluginSettings.fixEndKeyTimeShorterThanStopTime || HasNoFixTextKey(anim))
		return;
	const auto endKeyTime = anim->m_fEndKeyTime;
	if (auto* endKey = anim->m_spTextKeys->FindFirstByName("end"))
	{
		endKey->m_fTime = endKeyTime;
	}
#ifdef _DEBUG
	const auto tags = anim->GetIDTags();
	auto idx = 0;
#endif
	for (const auto& block : anim->GetControlledBlocks())
	{
#ifdef _DEBUG
		auto& tag = tags[idx++];
#endif
		NiTransformInterpolator* interpolator = static_cast<NiTransformInterpolator*>(block.m_spInterpolator.data);
		if (interpolator && IS_TYPE(interpolator, NiTransformInterpolator))
		{
			unsigned int numKeys;
			NiAnimationKey::KeyType keyType;
			unsigned char keySize;

			// PosData
			auto* posData = interpolator->GetPosData(numKeys, keyType, keySize);
			if (numKeys)
			{
				auto* key = posData->GetKeyAt(numKeys - 1, keySize);
				if (key->m_fTime < endKeyTime)
					key->m_fTime = endKeyTime;
			}

			// RotData
			auto* rotData = interpolator->GetRotData(numKeys, keyType, keySize);
			if (numKeys)
			{
				auto* key = rotData->GetKeyAt(numKeys - 1, keySize);
				if (key->m_fTime < endKeyTime)
					key->m_fTime = endKeyTime;
			}

			// ScaleData
			auto* scaleData = interpolator->GetScaleData(numKeys, keyType, keySize);
			if (numKeys)
			{
				auto* key = scaleData->GetKeyAt(numKeys - 1, keySize);
				if (key->m_fTime < endKeyTime)
					key->m_fTime = endKeyTime;
			}
		}
	}
}

char GetCharAfterAKey(const char* name)
{
	if (name[0] == 'a' && name[1] == ':')
		return static_cast<char>(std::tolower(name[2]));
	return '\0';
}

char GetACharForAnimGroup(AnimGroupID groupId)
{
	switch (groupId)
	{
	case kAnimGroup_Attack3:
		return '3';
	case kAnimGroup_Attack4:
		return '4';
	case kAnimGroup_Attack5:
		return '5';
	case kAnimGroup_Attack6:
		return '6';
	case kAnimGroup_Attack7:
		return '7';
	case kAnimGroup_Attack8:
		return '8';
	case kAnimGroup_AttackLeft:
		return 'l';
	default:
		return '\0';
	}
}

AnimGroupID GetAnimGroupForAChar(char c)
{
	switch (c)
	{
	case '3':
		return kAnimGroup_Attack3;
	case '4':
		return kAnimGroup_Attack4;
	case '5':
		return kAnimGroup_Attack5;
	case '6':
		return kAnimGroup_Attack6;
	case '7':
		return kAnimGroup_Attack7;
	case '8':
		return kAnimGroup_Attack8;
	case 'l':
		return kAnimGroup_AttackLeft;
	default:
		return kAnimGroup_Invalid;
	}
}

NiTextKey* GetNextAttackAnimGroupTextKey(BSAnimGroupSequence* sequence)
{
	for (auto& key : sequence->m_spTextKeys->GetKeys())
	{
		if (auto c = GetCharAfterAKey(key.m_kText.CStr()); c != '\0')
		{
			return &key;
		}
	}
	return nullptr;
}

bool HasRespectEndKey(BSAnimGroupSequence* anim)
{
	return anim->m_spTextKeys->FindFirstByName("respectEndKey") || anim->m_spTextKeys->FindFirstByName(
		"respectTextKeys");
}

void AnimFixes::FixWrongAKeyInRespectEndKey(AnimData* animData, BSAnimGroupSequence* anim)
{
	if (!g_pluginSettings.fixWrongAKeyInRespectEndKeyAnim || animData != g_thePlayer->firstPersonAnimData || !anim->animGroup || HasNoFixTextKey(anim))
		return;
	auto fullGroupId = anim->animGroup->groupID;
	if (anim->animGroup->IsAttackIS())
		fullGroupId -= 3;
	const auto groupId = static_cast<AnimGroupID>(fullGroupId);
	if (groupId < kAnimGroup_AttackLeft || groupId > kAnimGroup_Attack8)
		return;
	if (!HasRespectEndKey(anim))
		return;
	const auto nextAttackKey = GetNextAttackAnimGroupTextKey(anim);
	if (!nextAttackKey)
		return;
	const auto charAfterAKey = GetCharAfterAKey(nextAttackKey->m_kText.CStr());
	if (charAfterAKey == '\0')
		return;
	const auto nextAttack = GetAnimGroupForAChar(charAfterAKey);
	// check if nextAttack is the same as the current group, if not then it's probably a mistake
	if (nextAttack == kAnimGroup_Invalid || nextAttack == groupId)
		return;
	// check if this is intentional by looking up the 3rd person version of this anim and checking if it also has it
	const auto* anim3rd = GetAnimByGroupID(g_thePlayer->baseProcess->animData, groupId);
	if (anim3rd && anim3rd->m_spTextKeys->FindFirstByName(nextAttackKey->m_kText.CStr()))
		return;
	const auto aChar = GetACharForAnimGroup(groupId);
	const auto newText = std::string("a:") + std::string(1, aChar);
	LogAnimError(anim, FormatString("Fixed wrong a: key in respectEndKey anim, changed %s to %s", nextAttackKey->m_kText.CStr(), newText.c_str()));
	nextAttackKey->m_kText.Set(newText.c_str());
}

void AnimFixes::EraseNullTextKeys(const BSAnimGroupSequence* anim)
{
	auto* textKeys = anim->m_spTextKeys;
	if (ra::any_of(textKeys->GetKeys(), [](const NiTextKey& key) { return key.m_kText.CStr() == nullptr; }))
	{
		// LogAnimError(anim, "Erased null text keys");
		auto newKeys = textKeys->GetKeys()
			| ra::views::filter([](const NiTextKey& key) { return key.m_kText.CStr() != nullptr; })
			| ra::to<NiFixedArray<NiTextKey>>();
		textKeys->m_kKeyArray = std::move(newKeys);
	}
}

void AnimFixes::EraseNegativeAnimKeys(const BSAnimGroupSequence* anim)
{
#if 0
	// disabled as if transform data is freed it will crash the game since heap pointer is incremented
	if (HasNoFixTextKey(anim))
		return;
	for (const auto& block : anim->GetControlledBlocks())
	{
		auto* interp = static_cast<NiTransformInterpolator*>(block.m_spInterpolator);
		if (!interp || NOT_TYPE(interp, NiTransformInterpolator))
			continue;
		unsigned int numKeys;
		NiAnimationKey::KeyType keyType;
		unsigned char keySize;
		auto* posData = interp->GetPosData(numKeys, keyType, keySize);
		auto* transformData = interp->m_spData.data;
		for (int i = 0; i < numKeys; ++i)
		{
			const auto* key = posData->GetKeyAt(i, keySize);
			if (key->m_fTime < 0)
			{
				auto** mpPosKeys = reinterpret_cast<UInt8**>(&transformData->m_pkPosKeys);
				*mpPosKeys += keySize;
				transformData->m_uiNumPosKeys--;
			}
			else break;
		}
		auto* rotData = interp->GetRotData(numKeys, keyType, keySize);
		for (int i = 0; i < numKeys; ++i)
		{
			const auto* key = rotData->GetKeyAt(i, keySize);
			if (key->m_fTime < 0)
			{
				auto** mpRotKeys = reinterpret_cast<UInt8**>(&transformData->m_pkRotKeys);
				*mpRotKeys += keySize;
				transformData->m_uiNumRotKeys--;
			}
			else break;
		}
		auto* scaleData = interp->GetScaleData(numKeys, keyType, keySize);
		for (int i = 0; i < numKeys; ++i)
		{
			const auto* key = scaleData->GetKeyAt(i, keySize);
			if (key->m_fTime < 0)
			{
				auto** mpScaleKeys = reinterpret_cast<UInt8**>(&transformData->m_pkScaleKeys);
				*mpScaleKeys += keySize;
				transformData->m_uiNumScaleKeys--;
			}
			else break;
		}
	}
#endif
}

void AnimFixes::FixWrongPrnKey(BSAnimGroupSequence* anim)
{
	const auto* textKeys = anim->m_spTextKeys;
	const auto groupId = anim->animGroup->GetBaseGroupID();
	for (auto& key : textKeys->GetKeys())
	{
		if (std::string_view(key.m_kText.CStr()).starts_with("prn:"))
		{
			LogAnimError(anim, FormatString("Prn key %s does not exist so it was replaced by Bip01 Translate", key.m_kText.CStr()));
			if (groupId == kAnimGroup_Unequip)
				key.m_kText = "prn: Bip01 Translate";
			else if (groupId == kAnimGroup_Equip)
				key.m_kText = "prn: Bip01 R Hand";
			break;
		}
	}
	anim->animGroup->parentRootNode = "Bip01 Translate";
}

void AnimFixes::FixMissingPrnKey(BSAnimGroupSequence* anim, const char* filePath)
{
	if (!g_pluginSettings.fixMissingPrnKey || HasNoFixTextKey(anim) || !HasRespectEndKey(anim))
		return;
	const auto groupId = GroupNameToId(anim->m_kName); // animgroup is not loaded yet
	if (groupId != kAnimGroup_Unequip && groupId != kAnimGroup_Equip)
		return;
	auto prnKey = ra::find_if(anim->m_spTextKeys->GetKeys(), [](const NiTextKey& key) { return sv::contains_ci(key.m_kText, "prn:"); });
	if (prnKey != anim->m_spTextKeys->GetKeys().end())
		return;
	auto* prnName = groupId == kAnimGroup_Unequip ? "prn: Bip01 Translate" : "prn: Bip01 R Hand";
	LogAnimError(filePath, FormatString("Missing prn key in %s, added %s", anim->m_kName.CStr(), prnName));
	anim->m_spTextKeys->AddKey(prnName, 0.0f);
}

NiFixedString* __fastcall WrongPrnKeyHook(BSAnimGroupSequence* anim)
{
	if (!g_pluginSettings.fixWrongPrnKey || HasNoFixTextKey(anim) || !HasRespectEndKey(anim))
		return &anim->m_kName;

	if (const auto groupId = anim->animGroup->GetBaseGroupID(); groupId != kAnimGroup_Unequip && groupId != kAnimGroup_Equip)
		return &anim->m_kName;

	const std::string_view parentRootName(anim->animGroup->parentRootNode);
	if (parentRootName == "Bip01 Translate" || parentRootName == "Bip01 R Hand")
		return &anim->m_kName; // didn't work so lets not create an infinite loop
	
	AnimFixes::FixWrongPrnKey(anim);
	auto* addrOfRetn = static_cast<UInt32*>(_AddressOfReturnAddress());
	*addrOfRetn = 0x923B7E; // try again now
	return &anim->m_kName;
}

void AnimFixes::ApplyHooks()
{
	WriteRelCall(0x923BC6, &WrongPrnKeyHook);
}

void AnimFixes::ApplyFixes(AnimData* animData, BSAnimGroupSequence* anim)
{
	EraseNullTextKeys(anim);
	FixInconsistentEndTime(anim);
	FixWrongAKeyInRespectEndKey(animData, anim);
	// EraseNegativeAnimKeys(anim);
}

void AnimFixes::FixWrongKFName(BSAnimGroupSequence* anim, const char* filePath)
{
	if (!g_pluginSettings.fixWrongAnimName || HasNoFixTextKey(anim))
		return;
	const std::string_view fileName(filePath);
	const auto fileGroupName = sv::get_file_stem(fileName);
	const auto baseAnimGroupName = GetBaseAnimGroupName(fileGroupName);
	const auto kfGroupName = std::string_view(anim->m_kName);
	if (sv::equals_ci(baseAnimGroupName, kfGroupName)
		|| !sv::contains_ci(filePath, "animgroupoverride")) [[likely]]
		return;
	auto iter = ra::find_if(g_animGroupInfos, _L(AnimGroupInfo& i, sv::equals_ci(i.name, baseAnimGroupName)));
	if (iter == g_animGroupInfos.end())
		return;
	LogAnimError(filePath, FormatString("Fixed wrong KF name %s", anim->m_kName.CStr()));
	anim->m_kName = iter->name;
}

