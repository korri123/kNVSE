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
#include "nihooks.h"
#include "spine_snap.h"

#define CALL_EAX(addr) __asm mov eax, addr __asm call eax
#define JMP_EAX(addr)  __asm mov eax, addr __asm jmp eax
#define JMP_EDX(addr)  __asm mov edx, addr __asm jmp edx


AnimationContext g_animationHookContext;
bool g_startedAnimation = false;
BSAnimGroupSequence* g_lastLoopSequence = nullptr;
extern bool g_fixHolster;
bool g_doNotSwapAnims = false;

std::map<std::pair<FullAnimGroupID, AnimData*>, std::deque<BSAnimGroupSequence*>> g_queuedReplaceAnims;

#if _DEBUG
MapNode<const char*, NiControllerSequence> g_mapNode__;
NiTStringPointerMap<UInt32>* g_stringPointerMap__ = nullptr;
std::unordered_map<NiBlendInterpolator*, std::unordered_set<NiControllerSequence*>> g_debugInterpMap;
std::unordered_map<NiInterpolator*, const char*> g_debugInterpNames;
std::unordered_map<NiInterpolator*, const char*> g_debugInterpSequences;
#endif

UInt8* GetParentBasePtr(void* addressOfReturnAddress, bool lambda = false)
{
	auto* basePtr = static_cast<UInt8*>(addressOfReturnAddress) - 4;
#if _DEBUG
	if (lambda) // in debug mode, lambdas are wrapped inside a closure wrapper function, so one more step needed
		basePtr = *reinterpret_cast<UInt8**>(basePtr);
#endif
	return *reinterpret_cast<UInt8**>(basePtr);
}

BSAnimGroupSequence* GetQueuedAnim(AnimData* animData, FullAnimGroupID animGroupId)
{
	const auto key = std::make_pair(animGroupId, animData);
	const auto it = g_queuedReplaceAnims.find(key);
	if (it != g_queuedReplaceAnims.end())
	{
		auto& queue = it->second;
		if (!queue.empty())
		{
			auto* anim = queue.front();
			queue.pop_front();
			return anim;
		}
	}
	return nullptr;
}



// UInt32 animGroupId, BSAnimGroupSequence** toMorph, UInt8* basePointer
BSAnimGroupSequence* __fastcall HandleAnimationChange(AnimData* animData, void*, BSAnimGroupSequence* destAnim, UInt16 animGroupId, eAnimSequence animSequence)
{
	auto* basePointer = GetParentBasePtr(_AddressOfReturnAddress());
	
	// auto* toReplace = toMorph ? *toMorph : nullptr;

	g_animationHookContext = AnimationContext(basePointer);
	ra::copy(animData->animSequence, std::begin(g_animationHookContext.previousSequences));

	if (animData && animData->actor)
	{
		if (IsAnimGroupReload(animGroupId) && !IsLoopingReload(animGroupId))
			g_partialLoopReloadState = PartialLoopingReloadState::NotSet;
		std::optional<AnimationResult> animResult;

		auto* queuedAnim = GetQueuedAnim(animData, animGroupId);
		if (queuedAnim)
		{
			destAnim = queuedAnim;
		}

		if (!g_doNotSwapAnims && !queuedAnim && (animResult = GetActorAnimation(animGroupId, animData)))
		{
			auto* newAnim = LoadAnimationPath(*animResult, animData, animGroupId);
			if (newAnim)
				destAnim = newAnim;
		}
		else if (destAnim)
		{
			// allow non AnimGroupOverride anims to use custom text keys
			AnimPath ctx{};
			HandleExtraOperations(animData, destAnim, ctx);
		}
	}

	if (SpineSnapFix::ApplyFix(animData, destAnim) == SpineSnapFix::SKIP)
		return destAnim;

	// hooked call
	return ThisStdCall<BSAnimGroupSequence*>(0x4949A0, animData, destAnim, animGroupId, animSequence);
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

void __fastcall NiControllerSequence_ApplyDestFrameHook(NiControllerSequence* sequence, void*, float fTime, bool bUpdateInterpolators)
{

	if (sequence->state == kAnimState_Inactive)
		return;
	if (sequence->destFrame != -FLT_MAX)
	{
		if (sequence->offset == -FLT_MAX)
		{
			sequence->offset = -fTime;
		}

		if (sequence->startTime == -FLT_MAX)
		{
			const float easeTime = sequence->endTime;
			sequence->endTime = fTime + easeTime - sequence->destFrame;
			sequence->startTime = fTime - sequence->destFrame;
		}
		sequence->offset += sequence->destFrame;
		sequence->destFrame = -FLT_MAX; // end my suffering
		float fEaseSpinnerIn = (fTime - sequence->startTime) / (sequence->endTime - sequence->startTime);
		float fEaseSpinnerOut = (sequence->endTime - fTime) / (sequence->endTime - sequence->startTime);
		int i = 0;

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

bool LookupAnimFromMap(UInt16 groupId, AnimSequenceBase** base, AnimData* animData)
{
	// we do not want non existing 3rd anim data to try to get played as nothing gets played if it can't find it
	//if (animData == g_thePlayer->firstPersonAnimData)
	//	animData = g_thePlayer->baseProcess->GetAnimData();

	const auto findCustomAnim = [&](AnimData* animData) -> bool
	{
		if (const auto animResult = GetActorAnimation(groupId, animData))
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
			GetActorAnimation(groupId, g_thePlayer->firstPersonAnimData);
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

BSAnimGroupSequence* GetAnimByGroupID(AnimData* animData, UInt32 groupId)
{
	if (groupId == 0xFF)
		return nullptr;
	AnimSequenceBase* base = nullptr;
	if (animData->actor && LookupAnimFromMap(groupId, &base, animData) || (base = animData->mapAnimSequenceBase->Lookup(groupId)))
		return base->GetSequenceByIndex(-1);
	return nullptr;
}

// attempt to fix anims that don't get loaded since they aren't in the game to begin with
template <int AnimDataOffset>
bool __fastcall NonExistingAnimHook(NiTPointerMap<AnimSequenceBase>* animMap, void* _EDX, UInt16 groupId, AnimSequenceBase** base)
{
	auto* parentBasePtr = GetParentBasePtr(_AddressOfReturnAddress());
	auto* animData = *reinterpret_cast<AnimData**>(parentBasePtr + AnimDataOffset);

	if (animData->actor && LookupAnimFromMap(groupId, base, animData))
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

#if 0
bool __fastcall AllowAttacksEarlierInAnimHook(BaseProcess* baseProcess)
{
	// allow attacks before reload anim is done
	if (!baseProcess)
		return false; // test
	if (g_allowedNextAnims.contains(baseProcess))
	{
		// should be allowed except in exceptional cases
		return true;
	}
	if (baseProcess == g_thePlayer->baseProcess)
	{
		const auto iter = ra::find_if(g_timeTrackedAnims, [](auto& p)
		{
			const auto& animTime = *p.second;
			if (animTime.allowAttack == -FLT_MAX || !animTime.anim || animTime.anim->state == kAnimState_Inactive || !animTime.IsPlayer())
				return false;
			const auto anim = animTime.anim;
			// prevent first person / 3rd person mismatch
			if (animTime.firstPerson && g_thePlayer->IsThirdPerson())
				return false;
			const auto currentTime = GetAnimTime(animTime.GetAnimData(g_thePlayer), anim);
			return currentTime >= animTime.allowAttack;

		});
		if (iter != g_timeTrackedAnims.end() && GameFuncs::GetControlState(ControlCode::Attack, Decoding::IsDXKeyState::IsPressed))
		{
			auto* highProcess = g_thePlayer->GetHighProcess();
			highProcess->currentSequence = nullptr;
			highProcess->currentAction = Decoding::AnimAction::kAnimAction_None; // required due to 0x893E32 check
			return true;
		}
	}
	return baseProcess->IsReadyForAnim();
}
#endif

bool __fastcall HasAnimBaseDuplicate(AnimSequenceBase* base, KFModel* kfModel)
{
	auto* anim = kfModel->controllerSequence;
	if (base->IsSingle()) // single anims are always valid
		return false;
	auto* multiple = static_cast<AnimSequenceMultiple*>(base);
	for (auto* entry : *multiple->anims)
	{
		if (_stricmp(entry->sequenceName, anim->sequenceName) == 0)
			return true;
	}
	return false;
}

HOOK PreventDuplicateAnimationsHook()
{
	const static UInt32 skipAddress = 0x490D7E;
	const static UInt32 retnAddress = 0x490A4D;
    __asm
    {
        mov edx, [ebp+0x8]   // Load KFModel* into edx
        mov ecx, [ebp-0x10]  // Load AnimSequenceBase* into ecx
        call HasAnimBaseDuplicate

        test al, al
        jnz skip_to_address  // Jump if AL is not zero (HasAnimBaseDuplicate returned true)

        // Original code path
        mov eax, 0x43B300    // Load Ni_IsKindOf address into eax
        call eax             // Call Ni_IsKindOf
        jmp finish

    skip_to_address:
		add esp, 8          
        jmp skipAddress      // Jump to skipAddress

    finish:
        add esp, 8           // Adjust the stack pointer once for both branches
        jmp retnAddress      // Jump to return address
    }
}

#if 0
bool __fastcall ShouldActivateAimSequence(UInt8* _ebp)
{
	const auto* _currentSequence = *reinterpret_cast<BSAnimGroupSequence**>(_ebp - 0x20);
	const auto sequenceId = *reinterpret_cast<eAnimSequence*>(_ebp - 0x1c);
	if (!_currentSequence && (sequenceId == kSequence_WeaponUp || sequenceId == kSequence_WeaponDown)) // vanilla code
		return true;
	return g_animationHookContext.fixSpineSnap;
}

HOOK FixSpineBlendBug()
{
	__asm
	{
		lea ecx, [ebp]
		call ShouldActivateAimSequence
		test al, al
		jz skip
		JMP_EAX(0x4951E9)
	skip:
		JMP_EAX(0x495220)
	}
}
#endif


void ApplyHooks()
{
	const auto iniPath = GetCurPath() + R"(\Data\NVSE\Plugins\kNVSE.ini)";
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto errVal = ini.LoadFile(iniPath.c_str());
	g_logLevel = ini.GetOrCreate("General", "iConsoleLogLevel", 0, "; 0 = no console log, 1 = error console log, 2 = ALL logs go to console");

	//WriteRelJump(0x4949D0, AnimationHook);
	WriteRelCall(0x494989, HandleAnimationChange);

	WriteRelJump(0x5F444F, KeyStringCrashFixHook);
	WriteRelJump(0x941E4C, EndAttackLoopHook);

	// WriteRelJump(0x4951D7, FixSpineBlendBug);

	if (ini.GetOrCreate("General", "bFixLoopingReloads", 1, "; see https://www.youtube.com/watch?v=Vnh2PG-D15A"))
	{
		WriteRelCall(0x8BABCA, LoopingReloadFixHook);
		WriteRelCall(0x948CEC, LoopingReloadFixHook); // part of cancelling looping reload after shooting
	}

	if (ini.GetOrCreate("General", "bFixIdleStrafing", 1, "; allow player to strafe/turn sideways mid idle animation"))
		FixIdleAnimStrafe();

	if (ini.GetOrCreate("General", "bFixBlendAnimMultipliers", 1, "; fix blend times not being affected by animation multipliers (fixes animations playing twice in 1st person when an anim multiplier is big)"))
		WriteRelJump(0x4951D2, BlendMultHook);

	if (ini.GetOrCreate("General", "bFixPriority", 1 , "; fix engine bug where anims with same priority will flicker for a millisecond if blended together"))
		FixPriorityBug();

#if IS_TRANSITION_FIX
	//if (ini.GetOrCreate("General", "bFixAimAfterShooting", 1, "; stops the player from being stuck in irons sight aim mode after shooting a weapon while aiming"))
	{
		//APPLY_JMP(ProlongedAimFix);
		//SafeWriteBuf(0x491275, "\xEB\x0B", 2);
		WriteRelCall(0xA2E251, NiControllerSequence_ApplyDestFrameHook);
	}
#endif
	WriteRelCall(0xA2E251, NiControllerSequence_ApplyDestFrameHook);

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

	WriteRelJump(0x490A45, PreventDuplicateAnimationsHook);


#if _DEBUG && 0
	WriteRelJump(0x48F3A0, INLINE_HOOK(void, __fastcall, AnimSequenceMultiple * mult, void* _edx, BSAnimGroupSequence * sequence)
	{
		for (auto* anim : *mult->anims)
		{
			if (!_stricmp(anim->sequenceName, sequence->sequenceName))
			{
				//tList<const char> test;
				// auto* anims = ThisStdCall<tList<const char>*>(0x566970, g_thePlayer, 0xC);
				return;
			}
		}
		++sequence->m_uiRefCount;
		ThisStdCall(0x5597D0, mult->anims, &sequence);
	}));
#endif


	// No longer needed since AllowAttack

	// PlayerCharacter::Update -> IsReadyForAttack
	//WriteVirtualCall(0x9420E4, AllowAttacksEarlierInAnimHook);
	// FiresWeapon -> IsReadyForAttack
	//WriteVirtualCall(0x893E86, AllowAttacksEarlierInAnimHook);
	// Actor::Attack - Allow next anim
	//WriteVirtualCall(0x89371B, AllowAttacksEarlierInAnimHook);


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
	// AnimData::GetNextWeaponSequenceKey
	// fixes respectEndKey not working for a: keys
	WriteRelCall(0x495E6C, INLINE_HOOK(NiTextKeyExtraData*, __fastcall, BSAnimGroupSequence* sequence)
	{
		auto* defaultData = sequence->textKeyData;
		if (sequence->owner != g_thePlayer->baseProcess->animData->controllerManager || g_thePlayer->IsThirdPerson())
			return defaultData;
		auto* firstPersonAnim = g_thePlayer->firstPersonAnimData->animSequence[kSequence_Weapon];
		if (!firstPersonAnim)
			return defaultData;
		if (const auto iter = g_timeTrackedAnims.find(firstPersonAnim); iter != g_timeTrackedAnims.end())
		{
			if (iter->second->respectEndKey)
				return firstPersonAnim->textKeyData;
		}
		return defaultData;
	}));




	ini.SaveFile(iniPath.c_str(), false);
}

