#include "hooks.h"
#include "GameProcess.h"
#include "GameObjects.h"
#include "commands_animation.h"
#include "SafeWrite.h"
#include "utility.h"


#define EXPERIMENTAL 0

#if EXPERIMENTAL
std::unordered_map<void*, AnimData*> g_animDataMap;

void __fastcall ReplaceAnimation(void* map, UInt32 animGroupId, AnimSequenceBase** base)
{
	const auto animDataIter = g_animDataMap.find(map);
	if (animDataIter != g_animDataMap.end())
	{
		auto* animData = animDataIter->second;
		if (animData && animData->actor)
		{
			const auto animGroupMinor = static_cast<UInt8>(animGroupId);
			auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo();
			const auto firstPerson = animData == (*g_thePlayer)->firstPersonAnimData;
			if (weaponInfo && weaponInfo->weapon)
			{
				auto* anim = GetWeaponAnimationSingle(weaponInfo->weapon->refID, animGroupMinor, firstPerson);
				if (anim)
				{
					*base = anim->single;
					return;
				}
			}
			auto* actorAnim = GetActorAnimationSingle(animData->actor->refID, animGroupMinor, firstPerson);
			if (actorAnim)
			{
				*base = actorAnim;
			}
		}
	}
}

	__declspec(naked) void ReplaceAnimHook()
	{
		__asm
		{
			//mov ecx, [ebp + 0x4]
			//cmp ecx, 0x49062B // loadanimation
			//jz goBack
			
			push eax
			mov ecx, [ebp + 0xC] // AnimSequenceBase **a3
			push ecx
			movzx edx, [ebp + 0x8] // animgroupId
			mov ecx, [ebp - 0xC]
			call ReplaceAnimation
			pop eax
			//original asm
		goBack:
			mov esp, ebp
			pop ebp
			ret 8
		}
	}



void __fastcall SaveAnimDataMap(AnimData* animData)
{
	g_animDataMap[animData->mapAnimSequenceBase] = animData;
}

__declspec(naked) void AnimDataMapHook()
{
	static auto returnAddress = 0x490537;
	static auto fn_GetAnimGroup = 0x5585E0;
	__asm
	{
		push ecx
		mov ecx, [ebp - 0x90]
		call SaveAnimDataMap
		pop ecx
		call fn_GetAnimGroup
		jmp returnAddress
	}
}



/****************************************\\EXPERIMENTAL\\*********************************************/
/****************************************EXPERIMENTAL*********************************************/
#endif
void __fastcall HandleAnimationChange(AnimData* animData, UInt32 animGroupId, BSAnimGroupSequence** toMorph)
{
	_MESSAGE("%s %X", (*toMorph)->sequenceName, animGroupId);
	if (_stricmp((*toMorph)->sequenceName, "Characters\\_1stPerson\\2haattackloopis.kf") == 0)
		_MESSAGE("");
	if (animData && animData->actor)
	{
		auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo();
		const auto firstPerson = animData == (*g_thePlayer)->firstPersonAnimData;
		if (weaponInfo && weaponInfo->weapon)
		{
			auto* anim = GetWeaponAnimation(weaponInfo->weapon->refID, animGroupId, firstPerson, animData);
			if (anim)
			{
				anim->animGroup->groupID = animGroupId;
				*toMorph = anim;
				return;
			}
		}
		// NPCs animGroupId contains 0x8000 for some reason
		const auto actorAnimGroupId = animGroupId & 0xFFF;
		auto* actorAnim = GetActorAnimation(animData->actor->refID, actorAnimGroupId, firstPerson, animData);
		if (actorAnim)
		{
			//actorAnim->animGroup->groupID = animGroupId;
			*toMorph = actorAnim;
		}
	}
}



__declspec(naked) void AnimationHook()
{
	static auto fn_AnimDataContainsAnimSequence = 0x498EA0;
	static auto returnAddress = 0x4949D5;
	__asm
	{
		push ecx
		lea ecx, [ebp + 0x8] // BSAnimGroupSequence* toMorph
		push ecx
		mov edx, [ebp + 0xC] // animGroupId
		mov ecx, [ebp - 0x5C] // animData
		call HandleAnimationChange
		pop ecx
		call fn_AnimDataContainsAnimSequence
		jmp returnAddress
	}
}

void __fastcall ResetGroupId(BSAnimGroupSequence* sequence)
{
	if (sequence && sequence->animGroup)
		sequence->animGroup->groupID = 0xF5;
}

__declspec(naked) void ResetGroupIdHook()
{
	__asm
	{
		push eax
		mov ecx, [ebp + 0x8]
		call ResetGroupId
		pop eax
		mov esp, ebp
		pop ebp
		ret 0xC
	}
}

void ApplyHooks()
{
	WriteRelJump(0x4949D0, UInt32(AnimationHook));
	//WriteRelJump(0x49544D, UInt32(ResetGroupIdHook));
#if EXPERIMENTAL
	WriteRelJump(0x490532, UInt32(AnimDataMapHook));
	WriteRelJump(0x49C3FF, UInt32(ReplaceAnimHook));
#endif
}