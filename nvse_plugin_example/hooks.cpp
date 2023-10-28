#include "hooks.h"

#include <GameUI.h>
#include <span>
#include <unordered_set>

#include "GameProcess.h"
#include "GameObjects.h"
#include "commands_animation.h"
#include "SafeWrite.h"
#include "GameAPI.h"
#include "nitypes.h"
#include "SimpleINILibrary.h"
#include "utility.h"
#include "main.h"

// allow first person anims to override even if their 3rd person counterpart is missing
SInt32 g_firstPersonAnimId = -1;

AnimationContext g_animationHookContext;
bool g_startedAnimation = false;
BSAnimGroupSequence* g_lastLoopSequence = nullptr;
extern bool g_fixHolster;
bool g_doNotSwapAnims = false;


UInt8* GetParentBasePtr(void* addressOfReturnAddress, bool lambda = false)
{
	auto* basePtr = static_cast<UInt8*>(addressOfReturnAddress) - 4;
#if _DEBUG
	if (lambda) // in debug mode, lambdas are wrapped inside a closure wrapper function, so one more step needed
		basePtr = *reinterpret_cast<UInt8**>(basePtr);
#endif
	return *reinterpret_cast<UInt8**>(basePtr);
}

// UInt32 animGroupId, BSAnimGroupSequence** toMorph, UInt8* basePointer
bool __fastcall HandleAnimationChange(AnimData* animData, void* _edx, BSAnimGroupSequence* toReplace)
{
	auto* basePointer = GetParentBasePtr(_AddressOfReturnAddress());
	auto** toMorph = reinterpret_cast<BSAnimGroupSequence**>(basePointer + 0x8);
	auto animGroupId = *reinterpret_cast<UInt32*>(basePointer + 0xC);
	// auto* toReplace = toMorph ? *toMorph : nullptr;
#if _DEBUG
	auto& groupInfo = g_animGroupInfos[animGroupId & 0xFF];
	auto* curSeq = animData->animSequence[groupInfo.sequenceType];
	if (animData->actor->baseForm->refID == 0xE5958)
	{
		if (groupInfo.sequenceType != eAnimSequence::kSequence_WeaponUp && groupInfo.sequenceType != kSequence_WeaponDown)
		{
			static std::string lastFromName;
			static std::string lastToName;
			std::string fromStr = curSeq ? curSeq->sequenceName: "\\None";
			std::string toStr = toReplace ? toReplace->sequenceName : "\\None";
			auto fromName = fromStr.substr(fromStr.find_last_of('\\'));
			auto toName = toStr.substr(toStr.find_last_of('\\'));
			
			if (fromName != lastFromName && toName != lastToName)
			//if (curSeq && curSeq->animGroup->GetBaseGroupID() == kAnimGroup_FastForward || toReplace && toReplace->animGroup->GetBaseGroupID() == kAnimGroup_Idle)
				Console_Print("%s %s -> %s", animData->actor->GetFullName()->name.CStr(), fromName.c_str(), toName.c_str());
			lastFromName = fromName;
			lastToName = toName;
		}
	}
#endif
	g_animationHookContext = AnimationContext(basePointer);

	if (animData && animData->actor)
	{
		if (IsAnimGroupReload(animGroupId) && !IsLoopingReload(animGroupId))
			g_partialLoopReloadState = PartialLoopingReloadState::NotSet;
		const auto firstPerson = animData == g_thePlayer->firstPersonAnimData;
		if (firstPerson && g_firstPersonAnimId != -1)
		{
			animGroupId = g_firstPersonAnimId;
			g_firstPersonAnimId = -1;
		}
		std::optional<AnimationResult> animResult;
		if (!g_doNotSwapAnims && (animResult = GetActorAnimation(animGroupId, firstPerson, animData)))
		{
			auto* newAnim = LoadAnimationPath(*animResult, animData, animGroupId);
			if (newAnim)
				*toMorph = newAnim;
		}
		else if (toReplace)
		{
			// allow non animgroupoverride anims to use custom text keys
			static std::unordered_map<std::string, AnimPath> customKeyAnims;
			if (auto iter = customKeyAnims.find(toReplace->sequenceName); iter != customKeyAnims.end())
				HandleExtraOperations(animData, toReplace, iter->second);
			else
			{
				AnimPath ctx;
				if (HandleExtraOperations(animData, toReplace, ctx))
				{
					customKeyAnims.emplace(toReplace->sequenceName, std::move(ctx));
				}
			}
		}
	}
	// hooked call
	return ThisStdCall(0x498EA0, animData, toReplace);
}

#if 0
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
#endif

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
	const static auto customKeys = {
		"noBlend", "respectEndKey", "Script:", "interruptLoop", "burstFire", "respectTextKeys", "SoundPath:", "blendToReloadLoop", "scriptLine:", "replaceWithGroup:", "allowAttack"
	};
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

extern float g_destFrame;

void __fastcall NiControllerSequence_ApplyDestFrameHook(NiControllerSequence* sequence, void* _EDX, float fTime, bool bUpdateInterpolators)
{

	if (sequence->state == kAnimState_Inactive)
		return;
	if (g_destFrame != -FLT_MAX && sequence->kNVSEFlags & NiControllerSequence::kFlag_DestframeStartTime)
	{
		if (sequence->offset == -FLT_MAX)
		{
			sequence->offset = -fTime;
		}
		sequence->offset += g_destFrame;
		g_destFrame = -FLT_MAX;
		sequence->kNVSEFlags ^= NiControllerSequence::kFlag_DestframeStartTime;
		sequence->destFrame = -FLT_MAX; // end my suffering
	}
	ThisStdCall(0xA34BA0, sequence, fTime, bUpdateInterpolators);
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


#if 0
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
	_A lea ecx,[ebp]
	_A call ProlongedAimFix
	_A cmp al, 1
	_A jmp retnAddr
	})

#endif

std::unordered_set<std::string> g_reloadStartBlendFixes;

bool __fastcall ShouldPlayAimAnim(UInt8* basePointer)
{
	const static std::unordered_set ids = { kAnimGroup_ReloadWStart, kAnimGroup_ReloadYStart, kAnimGroup_ReloadXStart, kAnimGroup_ReloadZStart };
	const auto* anim = *reinterpret_cast<BSAnimGroupSequence**>(basePointer - 0xA0);
	const auto queuedId = *reinterpret_cast<AnimGroupID*>(basePointer - 0x30);
	const auto currentId = *reinterpret_cast<AnimGroupID*>(basePointer - 0x34);
	auto* animData = *reinterpret_cast<AnimData**>(basePointer - 0x18c);
	const auto newCondition = _L(, queuedId == 0xFF && !ids.contains(currentId));
	const auto defaultCondition = _L(, queuedId == 0xFF);
	if (!anim || !anim->animGroup)
		return defaultCondition();
	if (!g_reloadStartBlendFixes.contains(anim->sequenceName))
	{
		if (IsPlayersOtherAnimData(animData) && !g_thePlayer->IsThirdPerson())
		{
			const auto seqType = GetSequenceType(anim->animGroup->groupID);
			auto* cur1stPersonAnim = g_thePlayer->firstPersonAnimData->animSequence[seqType];
			if (cur1stPersonAnim && g_reloadStartBlendFixes.contains(cur1stPersonAnim->sequenceName))
				return newCondition();
		}
		return defaultCondition();
	}
	return newCondition();
}

__declspec(naked) void NoAimInReloadLoop()
{
	constexpr static auto jumpIfTrue = 0x492CB1;
	constexpr static auto jumpIfFalse = 0x492BF8;
	__asm
	{
		lea ecx, [ebp]
		call ShouldPlayAimAnim
		test al, al
		jz isFalse
		jmp jumpIfTrue
	isFalse:
		jmp jumpIfFalse
	}
}

bool LookupAnimFromMap(NiTPointerMap<AnimSequenceBase>* animMap, UInt16 groupId, AnimSequenceBase** base, AnimData* animData)
{
	// we do not want non existing 3rd anim data to try to get played as nothing gets played if it can't find it
	//if (animData == g_thePlayer->firstPersonAnimData)
	//	animData = g_thePlayer->baseProcess->GetAnimData();

	const auto findCustomAnim = [&](AnimData* animData) -> bool
	{
		if (const auto animResult = GetActorAnimation(groupId, animData == g_thePlayer->firstPersonAnimData, animData))
		{
			const auto& animPaths = animResult->parent->anims;
			auto* animPath = !animPaths.empty() ? &animPaths.at(0) : nullptr;
			if (!animPath)
				return false;
			if (const auto animCtx = LoadCustomAnimation(animPath->path, animData))
			{
				*base = animCtx->base;
				return true;
			}
		}
		if (animData->actor == g_thePlayer && animData != g_thePlayer->firstPersonAnimData && groupId != GetActorRealAnimGroup(g_thePlayer, groupId))
		{
			// hack to allow pollCondition to work correctly in first person when groupid is decayed
			GetActorAnimation(groupId, true, g_thePlayer->firstPersonAnimData);
		}
		return false;
	};
	if (findCustomAnim(animData))
		return true;
	/*if (animData->actor == g_thePlayer && g_firstPersonAnimId == -1 && findCustomAnim(g_thePlayer->firstPersonAnimData))
	{
		// allow 1st person to use anim if it exists instead of 3rd person anim which doesn't exist
		g_firstPersonAnimId = groupId;
		*base = nullptr;
	}*/
	return false;
}

// attempt to fix anims that don't get loaded since they aren't in the game to begin with
template <int AnimDataOffset>
bool __fastcall NonExistingAnimHook(NiTPointerMap<AnimSequenceBase>* animMap, void* _EDX, UInt16 groupId, AnimSequenceBase** base)
{
	auto* parentBasePtr = GetParentBasePtr(_AddressOfReturnAddress());
	auto* animData = *reinterpret_cast<AnimData**>(parentBasePtr + AnimDataOffset);

	if (animData->actor && LookupAnimFromMap(animMap, groupId, base, animData))
		return true;

	if ((*base = animMap->Lookup(groupId)))
		return true;

	return false;
}

void __fastcall HandleOnReload(Actor* actor)
{
	if (!actor)
		return;
	if (auto iter = g_reloadTracker.find(actor->refID); iter != g_reloadTracker.end())
	{
		auto& handler = iter->second;
		ra::for_each(handler.subscribers, _L(auto& p, p.second = true));
	}
	//std::erase_if(g_burstFireQueue, _L(BurstFireData & b, b.actorId == g_thePlayer->refID));
}

bool __fastcall AllowAttacksEarlierInAnimHook(BaseProcess* baseProcess)
{
	// allow attacks before reload anim is done
	if (!baseProcess)
		return false; // test
	if (baseProcess == g_thePlayer->baseProcess)
	{
		const auto iter = ra::find_if(g_timeTrackedAnims, [](auto& p)
		{
			const auto& animTime = *p.second;
			const auto anim = animTime.anim;
			const auto currentTime = GetAnimTime(animTime.GetAnimData(g_thePlayer), anim);
			return animTime.allowAttackTime != -FLT_MAX && currentTime >= animTime.allowAttackTime && animTime.IsPlayer();

		});
		if (iter != g_timeTrackedAnims.end() && GameFuncs::GetControlState(ControlCode::Attack, Decoding::IsDXKeyState::IsPressed))
		{
			g_thePlayer->GetHighProcess()->currentAction = Decoding::AnimAction::kAnimAction_None; // required due to 0x893E32 check
			return true;
		}
	}
	return baseProcess->IsReadyForAnim();
}


void ApplyHooks()
{
	const auto iniPath = GetCurPath() + R"(\Data\NVSE\Plugins\kNVSE.ini)";
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto errVal = ini.LoadFile(iniPath.c_str());
	g_logLevel = ini.GetOrCreate("General", "iConsoleLogLevel", 0, "; 0 = no console log, 1 = error console log, 2 = ALL logs go to console");

	//WriteRelJump(0x4949D0, AnimationHook);
	WriteRelCall(0x4949D0, HandleAnimationChange);

	WriteRelJump(0x5F444F, KeyStringCrashFixHook);
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

#if IS_TRANSITION_FIX
	//if (ini.GetOrCreate("General", "bFixAimAfterShooting", 1, "; stops the player from being stuck in irons sight aim mode after shooting a weapon while aiming"))
	{
		//APPLY_JMP(ProlongedAimFix);
		//SafeWriteBuf(0x491275, "\xEB\x0B", 2);
		WriteRelCall(0xA2E251, NiControllerSequence_ApplyDestFrameHook);
	}
#endif

	WriteRelJump(0x492BF2, NoAimInReloadLoop);

#if 1
	// attempt to fix anims that don't get loaded since they aren't in the game to begin with
	WriteRelCall(0x49575B, NonExistingAnimHook<-0x10>);
	WriteRelCall(0x495965, NonExistingAnimHook<-0x10>);
	WriteRelCall(0x4959B9, NonExistingAnimHook<-0x10>);
	WriteRelCall(0x495A03, NonExistingAnimHook<-0x10>);

	WriteRelCall(0x4948E6, NonExistingAnimHook<-0x14>);
	WriteRelCall(0x49472B, NonExistingAnimHook<-0x8>);

	/* experimental */
	WriteRelCall(0x9D0E80, NonExistingAnimHook<-0x1C>);
	WriteRelCall(0x97FF06, NonExistingAnimHook<-0x8>);
	WriteRelCall(0x97FE09, NonExistingAnimHook<-0xC>);
	WriteRelCall(0x97F36D, NonExistingAnimHook<-0xC>);
	WriteRelCall(0x8B7985, NonExistingAnimHook<-0x14>);
	WriteRelCall(0x49651B, NonExistingAnimHook<-0x14>);
	WriteRelCall(0x49651B, NonExistingAnimHook<-0x8>);
	WriteRelCall(0x495DC6, NonExistingAnimHook<-0x10>);
	WriteRelCall(0x4956CE, NonExistingAnimHook<-0x14>);
	WriteRelCall(0x495630, NonExistingAnimHook<-0x14>);
	WriteRelCall(0x49431B, NonExistingAnimHook<-0xC>);
	WriteRelCall(0x493DC0, NonExistingAnimHook<-0x98>);
	WriteRelCall(0x493115, NonExistingAnimHook<-0x18C>);

	//WriteRelCall(0x490626, NonExistingAnimHook<-0x90>);
	//WriteRelCall(0x49022F, NonExistingAnimHook<-0x54>);
	//WriteRelCall(0x490066, NonExistingAnimHook<-0x54>);
	/* experimental end */

#endif

	// BSAnimGroupSequence* destructor
	WriteRelCall(0x4EEB4B, INLINE_HOOK(void, __fastcall, BSAnimGroupSequence* anim)
	{
		HandleOnSequenceDestroy(anim);

		// hooked call
		ThisStdCall(0xA35640, anim);
	}));

	// AnimData destructor
	WriteRelCall(0x48FB82, INLINE_HOOK(void, __fastcall, AnimData* animData)
	{
		HandleOnAnimDataDelete(animData);

		// hooked call
		ThisStdCall(0x48FF50, animData);
	}));

	// hooking TESForm::GetFlags
	WriteRelCall(0x8A8C1B, INLINE_HOOK(UInt32, __fastcall, TESForm* form)
	{
		HandleOnReload(g_thePlayer);
		return form->flags;
	}));



	// PlayerCharacter::Update -> IsReadyForAttack
	WriteVirtualCall(0x9420E4, AllowAttacksEarlierInAnimHook);
	// FiresWeapon -> IsReadyForAttack
	WriteVirtualCall(0x893E86, AllowAttacksEarlierInAnimHook);

#if 0
	// DEBUG npcs sprint
	WriteRelJump(0x9DE060, INLINE_HOOK(void, __fastcall, ActorMover * mover, void* _edx, UInt32 flags)
	{
		if (mover->actor != g_thePlayer)
		{
			static auto beforeFlag = 0;
			auto* before = MoveFlagToString(beforeFlag);
			auto* after = MoveFlagToString(flags);
			beforeFlag = flags;
			//Console_Print("%s %s -> %s", mover->actor->GetFullName()->name.CStr(), before, after);
		}
		auto* playerMover = static_cast<PlayerMover*>(g_thePlayer->actorMover);
		if (playerMover->pcMovementFlags & 0xF)
		{
			mover->moveFlagStrafe38 = 0;
			mover->bStrafeMoveFlags71 = false;
			return;
		}
		mover->bStrafeMoveFlags71 = true;
		mover->moveFlagStrafe38 = flags;
	}));
#endif


	ini.SaveFile(iniPath.c_str(), false);
}

