#include "hooks.h"

#include <GameUI.h>
#include <span>
#include "GameProcess.h"
#include "GameObjects.h"
#include "commands_animation.h"
#include "SafeWrite.h"
#include "GameAPI.h"
#include "nitypes.h"
#include "SimpleINILibrary.h"
#include "utility.h"

AnimationContext g_animationHookContext;
bool g_startedAnimation = false;
BSAnimGroupSequence* g_lastLoopSequence = nullptr;

bool __fastcall HandleAnimationChange(AnimData* animData, UInt32 animGroupId, BSAnimGroupSequence** toMorph, UInt8* basePointer)
{
#if _DEBUG
	auto& groupInfo = g_animGroupInfos[animGroupId & 0xFF];
	auto* curSeq = animData->animSequence[groupInfo.sequenceType];
#endif
	g_animationHookContext = AnimationContext(basePointer);
	if (animData && animData->actor)
	{
#if 0
		if (*toMorph && animData->actor->GetFullName() /*&& animData->actor != (*g_thePlayer)*/)
			Console_Print("%X %s %s", animGroupId, animData->actor->GetFullName()->name.CStr(), (*toMorph)->sequenceName);
		auto* highProcess = (Decoding::HighProcess*) animData->actor->baseProcess;
		auto* queuedIdleFlags = &highProcess->queuedIdleFlags;
		auto* currAction = &highProcess->currentAction;
#endif
		const auto firstPerson = animData == g_thePlayer->firstPersonAnimData;

		if (auto* anim = GetActorAnimation(animGroupId, firstPerson, animData, *toMorph ? (*toMorph)->sequenceName : nullptr))
		{
			*toMorph = anim;
#if 0
			if (animData->actor->GetFullName() /*&& animData->actor != (*g_thePlayer)*/)
				Console_Print("%X %s %s", animGroupId, animData->actor->GetFullName()->name.CStr(), (*toMorph)->sequenceName);
#endif		
		}
	}
	return true;
}

__declspec(naked) void AnimationHook()
{
	static auto fn_AnimDataContainsAnimSequence = 0x498EA0;
	static auto returnAddress = 0x4949D5;
	__asm
	{
		push ecx

		push ebp
		lea ecx, [ebp + 0x8] // BSAnimGroupSequence* toMorph
		push ecx
		mov edx, [ebp + 0xC] // animGroupId
		mov ecx, [ebp - 0x5C] // animData
		call HandleAnimationChange
		test al, al
		jz prematureReturn
		pop ecx
		call fn_AnimDataContainsAnimSequence
		jmp returnAddress
	prematureReturn:
		add esp, 4
		mov eax, 0
		mov esp, ebp
		pop ebp
		ret 0xC
	}
}

bool __fastcall IsPlayerReadyForAnim(Decoding::HighProcess* highProcess)
{
	const auto defaultReturn = [&]()
	{
		//return g_thePlayer->baseProcess->IsReadyForAnim();
		return ThisStdCall<bool>(0x8DA2A0, highProcess);
	};
	
	if (g_thePlayer->baseProcess != highProcess)
		return defaultReturn();
	if (g_thePlayer->IsThirdPerson())
		return defaultReturn();
	auto* currentAnim = g_thePlayer->firstPersonAnimData->animSequence[kSequence_Weapon];
	if (!currentAnim || !currentAnim->animGroup)
		// shouldn't happen
		return defaultReturn();
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	if (animData3rd->idleAnim && (animData3rd->idleAnim->idleForm->flags & 1) != 0)
		return false;
	auto iter = g_timeTrackedAnims.find(currentAnim);
	auto* curr3rdWeapAnim = animData3rd->animSequence[kSequence_Weapon];
	if (iter == g_timeTrackedAnims.end())
	{
		if (curr3rdWeapAnim && curr3rdWeapAnim->animGroup)
		{
			// 1st person finished if anim is shorter
			iter = std::ranges::find_if(g_timeTrackedAnims, [&](auto& p) {return p.first->animGroup->groupID == curr3rdWeapAnim->animGroup->groupID; });
			if (iter == g_timeTrackedAnims.end())
			{
				return defaultReturn();
			}
		}
		else
			return defaultReturn();
	}
	if (!iter->second.respectEndKey)
		return defaultReturn();
	UInt16 thirdPersonGroupID;
	auto* leftArm3rdAnim = animData3rd->animSequence[kSequence_LeftArm];
	if (leftArm3rdAnim && leftArm3rdAnim->animGroup && ((thirdPersonGroupID = leftArm3rdAnim->animGroup->groupID)) 
		&& (thirdPersonGroupID == kAnimGroup_PipBoy || thirdPersonGroupID == kAnimGroup_PipBoyChild))
		return false;
	if (ThisStdCall<bool>(0x967AE0, g_thePlayer)) // PlayerCharacter::HasPipboyOpen
		return false;
	const auto usingRangedWeapon = CdeclCall<bool>(0x9A96C0, g_thePlayer);
	if (!usingRangedWeapon)
		return defaultReturn();
	if (iter->second.finishedEndKey)
	{
		if (reinterpret_cast<UInt32>(_ReturnAddress()) == 0x9420E6 && GameFuncs::GetControlState(Decoding::ControlCode::Attack, Decoding::IsDXKeyState::IsPressed))
		{
			const auto groupID = iter->first->animGroup->groupID;
			const auto nextSequenceKey = (groupID & 0xFF00) + static_cast<UInt16>(animData3rd->GetNextAttackGroupID());
			auto* anim3rdPersonCounterpart = GetGameAnimation(animData3rd, nextSequenceKey);
			if (!anim3rdPersonCounterpart)
				return defaultReturn();
			GameFuncs::Actor_SetAnimActionAndSequence(g_thePlayer, Decoding::AnimAction::kAnimAction_Attack, anim3rdPersonCounterpart);
		}
		return true;
	}
	return false;

}

__declspec(naked) void IsPlayerReadyForAnimHook()
{
	const static auto retnAddr = 0x9420E6;
	__asm
	{
		call IsPlayerReadyForAnim
		jmp retnAddr
	}
}

void DecreaseAttackTimer()
{
	auto* p = static_cast<Decoding::HighProcess*>(g_thePlayer->baseProcess);
	if (!g_lastLoopSequence)
		return p->ResetAttackLoopTimer(false);
	if (g_startedAnimation)
	{
		p->time1D4 = g_lastLoopSequence->endKeyTime - g_lastLoopSequence->startTime; // start time is actually curTime
		g_startedAnimation = false;
	}
	const auto oldTime = p->time1D4;
	p->DecreaseAttackLoopShootTime(g_thePlayer);
	if (p->time1D4 >= oldTime)
	{
		p->time1D4 = 0;
		g_lastLoopSequence = nullptr;
	}
}

__declspec(naked) void EndAttackLoopHook()
{
	const static auto retnAddr = 0x941E64;
	__asm
	{
		call DecreaseAttackTimer
		jmp retnAddr
	}
}

int AnimErrorLogHook(const char* fmt, ...)
{
	va_list	args;
	va_start(args, fmt);
	char buf[0x400];
	vsprintf_s(buf, sizeof buf, fmt, args);
	DebugPrint(FormatString("GAME: %s", buf));
	va_end(args);

	return 0;
}

UInt16 __fastcall LoopingReloadFixHook(AnimData* animData, void* _edx, UInt16 groupID, bool noRecurse)
{
	const auto defaultRet = [&]()
	{
		return ThisStdCall<UInt16>(0x495740, animData, groupID, noRecurse);
	};
	groupID = groupID & 0xFF; // prevent swimming animation if crouched and reloading
	auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo();
	if (!weaponInfo)
		return defaultRet();
	const auto fullGroupId = ThisStdCall<UInt16>(0x897910, animData->actor, groupID, weaponInfo, false, animData);
	if (fullGroupId == 0xFF)
		return defaultRet();
	return fullGroupId;
}

bool __fastcall IsCustomAnimKey(const char* key)
{
	const static auto customKeys = { "noBlend", "respectEndKey", "Script:", "interruptLoop", "burstFire" };
	return ra::any_of(customKeys, _L(const char* key2, StartsWith(key, key2)));
}

__declspec(naked) void KeyStringCrashFixHook()
{
	const static auto retnAddr = 0x5F4454;
	const static auto skipDest = 0x5F3BF1;
	const static auto this8addr = 0x413F40;
	__asm
	{
		call this8addr
		push eax
		mov ecx, [ebp-0x3B8]
		call IsCustomAnimKey
		test al, al
		jnz skip
		pop eax
		jmp retnAddr
	skip:
		add esp, 4
		jmp skipDest
	}
}

void FixIdleAnimStrafe()
{
	SafeWriteBuf(0x9EA0C8, "\xEB\xE", 2); // jmp 0x9EA0D8
}

#define HOOK __declspec(naked) void

void __fastcall FixBlendMult()
{
	const auto mult = GetAnimMult(g_animationHookContext.animData, *g_animationHookContext.groupID);
	if (mult > 1.0f)
	{
		*g_animationHookContext.blendAmount /= mult;
#if _DEBUG
		//Console_Print("applied mult %g, %g >> %g", mult, (*g_animationHookContext.blendAmount *= mult), *g_animationHookContext.blendAmount);
#endif
	}
}

HOOK BlendMultHook()
{
	const static auto hookedCall = 0xA328B0;
	const static auto retnAddr = 0x4951D7;
	__asm
	{
		call hookedCall
		call FixBlendMult
		jmp retnAddr
	}
}

bool __fastcall ProlongedAimFix(UInt8* basePointer)
{
	const auto keyType = *reinterpret_cast<UInt32*>(basePointer - 0xC);
	auto* actor = *reinterpret_cast<Actor**>(basePointer - 0x30);
	if (actor != g_thePlayer)
		return keyType == kAnimKeyType_LoopingSequenceOrAim;
	auto* process = g_thePlayer->GetHighProcess();
	return keyType == kAnimKeyType_Attack && !process->isAiming || keyType == kAnimKeyType_LoopingSequenceOrAim;
}

JMP_HOOK(0x8BB96A, ProlongedAimFix, 0x8BB974, {
	_A lea ecx, [ebp]
	_A call ProlongedAimFix
	_A cmp al, 1
	_A jmp retnAddr
})

void ApplyHooks()
{
	const auto iniPath = GetCurPath() + R"(\Data\NVSE\Plugins\kNVSE.ini)";
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto errVal = ini.LoadFile(iniPath.c_str());
	g_logLevel = ini.GetOrCreate("General", "iConsoleLogLevel", 0, "; 0 = no console log, 1 = error console log, 2 = ALL logs go to console");

	WriteRelJump(0x4949D0, AnimationHook);
	WriteRelJump(0x5F444F, KeyStringCrashFixHook);

	//SafeWrite32(0x1087C5C, reinterpret_cast<UInt32>(IsPlayerReadyForAnim));

	WriteRelJump(0x941E4C, EndAttackLoopHook);

	if (ini.GetOrCreate("General", "bFixLoopingReloads", 1, "; see https://www.youtube.com/watch?v=Vnh2PG-D15A"))
	{
		WriteRelCall(0x8BABCA, LoopingReloadFixHook);
		WriteRelCall(0x948CEC, LoopingReloadFixHook); // part of cancelling looping reload after shooting
	}

	if (ini.GetOrCreate("General", "bFixIdleStrafing", 1, "; allow player to strafe/turn sideways mid idle animation"))
		FixIdleAnimStrafe();

	if (ini.GetOrCreate("General", "bFixBlendAnimMultipliers", 1, "; fix blend times not being affected by animation multipliers (fixes animations playing twice in 1st person when an anim multiplier is big)"))
		WriteRelJump(0x4951D2, BlendMultHook);

	/*if (ini.GetOrCreate("General", "bFixAimAfterShooting", 1, "; stops the player from being stuck in irons sight aim mode after shooting a weapon while aiming"))
	{
		APPLY_JMP(ProlongedAimFix);
		SafeWriteBuf(0x491275, "\xEB\x0B", 2);
	}*/
	
	ini.SaveFile(iniPath.c_str(), false);
}