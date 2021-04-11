#include "hooks.h"
#include "GameProcess.h"
#include "GameObjects.h"
#include "commands_animation.h"
#include "SafeWrite.h"
#include "utility.h"
#include "GameAPI.h"

void __fastcall HandleAnimationChange(AnimData* animData, UInt32 animGroupId, BSAnimGroupSequence** toMorph)
{
	if (animData && animData->actor)
	{
#if _DEBUG
		if (*toMorph && animData->actor->GetFullName() /*&& animData->actor != (*g_thePlayer)*/)
			Console_Print("%X %s %s", animGroupId, animData->actor->GetFullName()->name.CStr(), (*toMorph)->sequenceName);
#endif
		auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo();
		const auto firstPerson = animData == (*g_thePlayer)->firstPersonAnimData;
		if (weaponInfo && weaponInfo->weapon)
		{
			if (auto* anim = GetWeaponAnimation(weaponInfo->weapon, animGroupId, firstPerson, animData))
			{
				*toMorph = anim;
				return;
			}
		}
		if (auto* actorAnim = GetActorAnimation(animData->actor, animGroupId, firstPerson, animData, *toMorph ? (*toMorph)->sequenceName : nullptr))
		{
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

void ApplyHooks()
{
	WriteRelJump(0x4949D0, UInt32(AnimationHook));
}