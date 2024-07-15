#include "anim_fixes.h"

#include <ranges>

#include "class_vtbls.h"
#include "game_types.h"
#include "hooks.h"
#include "utility.h"

void LogAnimError(const BSAnimGroupSequence* anim, const std::string& msg)
{
	DebugPrint("Animation Error Detected: " + std::string(anim->sequenceName) + "\n\t" + msg);
}

bool HasNoFixTextKey(BSAnimGroupSequence* anim)
{
	if (!anim->textKeyData || anim->textKeyData->FindFirstByName("noFix"))
		return true;
	return false;
}

void AnimFixes::FixInconsistentEndTime(BSAnimGroupSequence* anim)
{
	if (!g_fixEndKeyTimeShorterThanStopTime || HasNoFixTextKey(anim))
		return;
	const auto* endKey = anim->textKeyData->FindFirstByName("end");
	if (!endKey)
	{
		LogAnimError(anim, "No end key found in anim");
		return;
	}
	const auto endKeyTime = endKey->m_fTime;
	anim->endKeyTime = endKeyTime;
#if _DEBUG
	const auto tags = anim->GetIDTags();
	auto idx = 0;
#endif
	for (const auto& block : anim->GetControlledBlocks())
	{
#if _DEBUG
		auto& tag = tags[idx++];
#endif
		auto* interpolator = block.interpolator;
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
				key->m_fTime = endKeyTime;
			}

			// RotData
			auto* rotData = interpolator->GetRotData(numKeys, keyType, keySize);
			if (numKeys)
			{
				auto* key = rotData->GetKeyAt(numKeys - 1, keySize);
				key->m_fTime = endKeyTime;
			}

			// ScaleData
			auto* scaleData = interpolator->GetScaleData(numKeys, keyType, keySize);
			if (numKeys)
			{
				auto* key = scaleData->GetKeyAt(numKeys - 1, keySize);
				key->m_fTime = endKeyTime;
			}
		}
	}
}

char GetCharAfterAKey(const char* name)
{
	if (name[0] == 'a' && name[1] == ':')
		return std::tolower(name[2]);
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
	for (auto& key : sequence->textKeyData->GetKeys())
	{
		if (auto c = GetCharAfterAKey(key.m_kText.CStr()); c != '\0')
		{
			return &key;
		}
	}
	return nullptr;
}

void AnimFixes::FixWrongAKeyInRespectEndKey(AnimData* animData, BSAnimGroupSequence* anim)
{
	if (!g_fixWrongAKeyInRespectEndKeyAnim || animData != g_thePlayer->firstPersonAnimData || !anim->animGroup || HasNoFixTextKey(anim))
		return;
	auto fullGroupId = anim->animGroup->groupID;
	if (anim->animGroup->IsAttackIS())
		fullGroupId -= 3;
	const auto groupId = static_cast<AnimGroupID>(fullGroupId);
	if (groupId < kAnimGroup_AttackLeft || groupId > kAnimGroup_Attack8)
		return;
	if (!anim->textKeyData->FindFirstByName("respectEndKey") && !anim->textKeyData->FindFirstByName("respectTextKeys"))
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
	if (anim3rd && anim3rd->textKeyData->FindFirstByName(nextAttackKey->m_kText.CStr()))
		return;
	const auto aChar = GetACharForAnimGroup(groupId);
	const auto newText = std::string("a:") + std::string(1, aChar);
	LogAnimError(anim, FormatString("Fixed wrong a: key in respectEndKey anim, changed %s to %s", nextAttackKey->m_kText.CStr(), newText.c_str()));
	nextAttackKey->m_kText.Set(newText.c_str());
}

void AnimFixes::EraseNullTextKeys(const BSAnimGroupSequence* anim)
{
	auto* textKeys = anim->textKeyData;
	if (!textKeys)
		return;
	if (ra::any_of(textKeys->GetKeys(), [](const NiTextKey& key) { return key.m_kText.CStr() == nullptr; }))
	{
		LogAnimError(anim, "Erased null text keys");
		const auto newKeys = textKeys->ToVector()
			| ra::views::filter([](const NiTextKey& key) { return key.m_kText.CStr() != nullptr; })
			| ra::to<std::vector<NiTextKey>>();
		textKeys->SetKeys(newKeys);
	}
}

void AnimFixes::ApplyFixes(AnimData* animData, BSAnimGroupSequence* anim)
{
	EraseNullTextKeys(anim);
	FixInconsistentEndTime(anim);
	FixWrongAKeyInRespectEndKey(animData, anim);
}
