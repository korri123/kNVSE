﻿#include "anim_fixes.h"

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
	static std::unordered_set<BSAnimGroupSequence*> s_fixedAnims;
	if (s_fixedAnims.contains(anim))
		return;
	s_fixedAnims.insert(anim);
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
		if (interpolator && interpolator->m_spData && IS_TYPE(interpolator, NiTransformInterpolator))
		{
			const auto& data = *interpolator->m_spData;

			// this heap of mess is required since they keys are different sizes depending on the type
			const auto updateEndKeyTime = [&](auto getKeyFunction)
			{
				auto keys = (data.*getKeyFunction)();
				if (!keys.empty())
					keys.back().m_fTime = endKeyTime;
			};

			switch (data.m_ePosType)
			{
			case NiAnimationKey::BEZKEY:
				updateEndKeyTime(&NiTransformData::GetPosKeys<NiBezPosKey>);
				break;
			case NiAnimationKey::TCBKEY:
				updateEndKeyTime(&NiTransformData::GetPosKeys<NiTCBPosKey>);
				break;
			default:
				updateEndKeyTime(&NiTransformData::GetPosKeys<NiPosKey>);
				break;
			}
				
			switch (data.m_eRotType)
			{
			case NiAnimationKey::BEZKEY:
				updateEndKeyTime(&NiTransformData::GetRotKeys<NiBezRotKey>);
				break;
			case NiAnimationKey::TCBKEY:
				updateEndKeyTime(&NiTransformData::GetRotKeys<NiTCBRotKey>);
				break;
			case NiAnimationKey::EULERKEY:
				updateEndKeyTime(&NiTransformData::GetRotKeys<NiEulerRotKey>);
				break;
			default:
				updateEndKeyTime(&NiTransformData::GetRotKeys<NiRotKey>);
				break;
			}
			switch (data.m_eScaleType)
			{
			case NiAnimationKey::BEZKEY:
				updateEndKeyTime(&NiTransformData::GetScaleKeys<NiBezPosKey>);
				break;
			case NiAnimationKey::TCBKEY:
				updateEndKeyTime(&NiTransformData::GetScaleKeys<NiTCBPosKey>);
				break;
			default:
				updateEndKeyTime(&NiTransformData::GetScaleKeys<NiFloatKey>);
				break;
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

void AnimFixes::ApplyFixes(AnimData* animData, BSAnimGroupSequence* anim)
{
	FixInconsistentEndTime(anim);
	FixWrongAKeyInRespectEndKey(animData, anim);
}
