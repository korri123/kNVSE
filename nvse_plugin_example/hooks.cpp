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

void __fastcall HandleAnimationChange(AnimData* animData, UInt32 animGroupId, BSAnimGroupSequence** toMorph, UInt8* basePointer)
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
		pop ecx
		call fn_AnimDataContainsAnimSequence
		jmp returnAddress
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
	const auto iter = g_firstPersonAnimTimes.find(currentAnim);
	if (iter == g_firstPersonAnimTimes.end())
		return defaultReturn();
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	if (animData3rd->idleAnim && (animData3rd->idleAnim->idleForm->flags & 1) != 0)
		return false;
	auto* current3rdPersonAnim = animData3rd->animSequence[kSequence_LeftArm];
	UInt16 thirdPersonGroupID;
	if (current3rdPersonAnim && current3rdPersonAnim->animGroup && ((thirdPersonGroupID = current3rdPersonAnim->animGroup->groupID)) 
		&& (thirdPersonGroupID == kAnimGroup_PipBoy || thirdPersonGroupID == kAnimGroup_PipBoyChild))
		return false;
	if (ThisStdCall<bool>(0x967AE0, g_thePlayer)) // PlayerCharacter::HasPipboyOpen
		return false;
	const auto usingRangedWeapon = CdeclCall<bool>(0x9A96C0, g_thePlayer);
	if (!usingRangedWeapon)
		return defaultReturn();
	return iter->second.finished;
#if 0
	if (currentAnim->animGroup->IsAim())
		return true;
	
	const auto groupID = currentAnim->animGroup->groupID & 0xFF;
	if (groupID == kAnimGroup_AttackLoop || groupID == kAnimGroup_AttackLoopIS)
		return defaultReturn();

	if (usingRangedWeapon)
	{
		if (currentAnim->animGroup->IsAttack() || currentAnim->animGroup->IsReload())
			return false;
		return true;
	}
	else
	{
		// melee
#if 0
		const auto timePassed = GameFuncs::AnimData_GetSequenceOffsetPlusTimePassed(Player()->firstPersonAnimData, currentAnim);
		std::span timeKeys{currentAnim->textKeyData->m_pKeys, currentAnim->textKeyData->m_uiNumKeys};
		const auto iter = std::ranges::find_if(timeKeys, [&](NiTextKey& key) { return _stricmp(key.m_kText.data, "a") == 0; });
		if (iter == timeKeys.end())
			return Player()->baseProcess->IsReadyForAnim();
		auto endTime = iter->m_fTime;;
		if (timePassed >= endTime)
#endif
		return defaultReturn();
	}
	return false;
#endif
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
	auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo();
	if (!weaponInfo)
		return defaultRet();
	const auto fullGroupId = ThisStdCall<UInt16>(0x897910, animData->actor, groupID, weaponInfo, false, animData);
	if (fullGroupId == 0xFF)
		return defaultRet();
	return fullGroupId;
}

void FixIdleAnimStrafe()
{
	SafeWriteBuf(0x9EA0C8, "\xEB\xE", 2); // jmp 0x9EA0D8
}

void ApplyHooks()
{
	const auto iniPath = GetCurPath() + R"(\Data\NVSE\Plugins\kNVSE.ini)";
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto errVal = ini.LoadFile(iniPath.c_str());
	g_logLevel = ini.GetOrCreate("General", "iConsoleLogLevel", 0, "; 0 = no console log, 1 = error console log, 2 = ALL logs go to console");

	WriteRelJump(0x4949D0, AnimationHook);

	SafeWrite32(0x1087C5C, reinterpret_cast<UInt32>(IsPlayerReadyForAnim));

	WriteRelJump(0x941E4C, EndAttackLoopHook);

	if (ini.GetOrCreate("General", "bFixLoopingReloads", 1, "; see https://www.youtube.com/watch?v=Vnh2PG-D15A"))
		WriteRelCall(0x8BABCA, LoopingReloadFixHook);

	if (ini.GetOrCreate("General", "bFixIdleStrafing", 1, "; allow player to strafe/turn sideways mid idle animation"))
		FixIdleAnimStrafe();
	
	ini.SaveFile(iniPath.c_str(), false);
}