#include "hooks.h"

#include <GameUI.h>
#include <span>
#include <unordered_set>

#include "anim_fixes.h"
#include "GameProcess.h"
#include "GameObjects.h"
#include "commands_animation.h"
#include "SafeWrite.h"
#include "nitypes.h"
#include "SimpleINILibrary.h"
#include "utility.h"
#include "main.h"
#include "nihooks.h"
#include "blend_fixes.h"
#include "knvse_events.h"

#define CALL_EAX(addr) __asm mov eax, addr __asm call eax
#define JMP_EAX(addr)  __asm mov eax, addr __asm jmp eax
#define JMP_EDX(addr)  __asm mov edx, addr __asm jmp edx


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
	const auto baseAnimGroup = static_cast<AnimGroupID>(animGroupId);
	if (g_disableFirstPersonTurningAnims && animData == g_thePlayer->firstPersonAnimData && (baseAnimGroup == kAnimGroup_TurnLeft || baseAnimGroup == kAnimGroup_TurnRight))
		return destAnim;
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

		if (const auto interceptedAnimGroup = InterceptPlayAnimGroup::Dispatch(animData->actor, animGroupId))
		{
			const auto newGroupId = *interceptedAnimGroup;
			if (newGroupId == 0xFF)
				return destAnim;
			if (auto* anim = GetAnimByGroupID(animData, newGroupId); anim && anim->animGroup)
			{
				destAnim = anim;
				animGroupId = anim->animGroup->groupID;
			}
		}

		if (!g_doNotSwapAnims && !queuedAnim && (animResult = GetActorAnimation(animGroupId, animData)))
		{
			auto* newAnim = LoadAnimationPath(*animResult, animData, animGroupId);
			if (newAnim)
			{
				destAnim = newAnim;
			}
		}
		else if (destAnim)
		{
			// allow non AnimGroupOverride anims to use custom text keys
			HandleExtraOperations(animData, destAnim);
		}
	}

	if (g_fixSpineBlendBug && BlendFixes::ApplyAimBlendFix(animData, destAnim) == BlendFixes::SKIP)
		return destAnim;

	

	BSAnimGroupSequence* currentAnim = nullptr;
	if (destAnim && destAnim->animGroup)
		if (auto* groupInfo = destAnim->animGroup->GetGroupInfo())
			currentAnim = animData->animSequence[groupInfo->sequenceType];

	if (destAnim && currentAnim)
	{
		BlendFixes::FixConflictingPriorities(currentAnim, destAnim);
	}
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
	auto* baseProcess = g_thePlayer->baseProcess;
	if (!g_lastLoopSequence)
		return baseProcess->ResetAttackLoopTimer(false);
	if (g_startedAnimation)
	{
		//p->time1D4 = g_lastLoopSequence->endKeyTime - g_lastLoopSequence->startTime; // start time is actually curTime
		baseProcess->time1D4 = g_lastLoopSequence->endKeyTime;
		g_startedAnimation = false;
	}
	const auto oldTime = baseProcess->time1D4;
	baseProcess->DecreaseAttackLoopShootTime(g_thePlayer);
	if (baseProcess->time1D4 >= oldTime)
	{
		baseProcess->time1D4 = 0;
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
		"noBlend", "respectEndKey", "Script:", "interruptLoop", "burstFire", "respectTextKeys", "SoundPath:",
		"blendToReloadLoop", "scriptLine:", "replaceWithGroup:", "allowAttack", "noFix"
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

void __fastcall FixBlendMult(BSAnimGroupSequence* anim, void*, float fTime, bool bUpdateInterpolators)
{
	const auto* animData = GET_CALLER_VAR(AnimData*, -0x5C, false);
	auto* animBlend = GET_CALLER_VAR_PTR(float*, -0x34, false);
	const auto mult = GetAnimMult(animData, static_cast<UInt8>(anim->animGroup->groupID));
	if (mult > 1.0f)
	{
		*animBlend /= mult;
	}
	// hooked call
	ThisStdCall(0xA328B0, anim, fTime, bUpdateInterpolators);
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

bool LookupAnimFromMap(UInt16 groupId, AnimSequenceBase** base, AnimData* animData)
{
	// we do not want non existing 3rd anim data to try to get played as nothing gets played if it can't find it
	//if (animData == g_thePlayer->firstPersonAnimData)
	//	animData = g_thePlayer->baseProcess->GetAnimData();

	const auto findCustomAnim = [&](AnimData* animData) -> bool
	{
		if (const auto animResult = GetActorAnimation(groupId, animData))
		{
			if (const auto animCtx = LoadCustomAnimation(*animResult->parent, groupId, animData))
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

BSAnimGroupSequence* GetAnimByFullGroupID(AnimData* animData, UInt16 groupId)
{
	if (const auto result = GetActorAnimation(groupId, animData))
		if (const auto ctx = LoadCustomAnimation(*result->parent, groupId, animData))
			return ctx->anim;
	if (auto* base = animData->mapAnimSequenceBase->Lookup(groupId))
		return base->GetSequenceByIndex(-1);
	return nullptr;
}

BSAnimGroupSequence* GetAnimByGroupID(AnimData* animData, AnimGroupID groupId)
{
	if (groupId == 0xFF)
		return nullptr;
	const auto nearestGroupId = GetNearestGroupID(animData, groupId);
	if (nearestGroupId == 0xFFFF)
		return nullptr;
	return GetAnimByFullGroupID(animData, nearestGroupId);
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

bool __fastcall HasAnimBaseDuplicate(AnimSequenceBase* base, KFModel* kfModel)
{
	auto* anim = kfModel->controllerSequence;

	// done here as it's done once
	AnimFixes::EraseNullTextKeys(anim);
	AnimFixes::FixInconsistentEndTime(anim);
	
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

bool g_fixSpineBlendBug = false;
bool g_fixAttackISTransition = false;
bool g_fixBlendSamePriority = false;
bool g_fixLoopingReloadStart = false;
bool g_disableFirstPersonTurningAnims = false;

bool g_fixEndKeyTimeShorterThanStopTime = false;
bool g_fixWrongAKeyInRespectEndKeyAnim = false;

namespace LoopingReloadPauseFix
{
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
}

void ApplyHooks()
{
	const auto iniPath = GetCurPath() + R"(\Data\NVSE\Plugins\kNVSE.ini)";
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto errVal = ini.LoadFile(iniPath.c_str());
	g_logLevel = ini.GetOrCreate("General", "iConsoleLogLevel", 0, "; 0 = no console log, 1 = error console log, 2 = ALL logs go to console");

#if 0
	g_fixSpineBlendBug = ini.GetOrCreate("General", "bFixSpineBlendBug", 1, "; fix spine blend bug when aiming down sights in 3rd person and cancelling the aim while looking up or down");
	g_fixBlendSamePriority = ini.GetOrCreate("General", "bFixBlendSamePriority", 1, "; fix blending weapon animations with same bone priorities causing flickering as game tries to blend them with bones with the next high priority");
	g_fixAttackISTransition = ini.GetOrCreate("General", "bFixAttackISTransition", 1, "; fix iron sight attack anims being glued to player's face even after player has released aim control");
	g_fixLoopingReloadStart = ini.GetOrCreate("General", "bFixLoopingReloadStart", 1, "; fix looping reload start anims transitioning to aim anim before main looping reload anim");

	g_disableFirstPersonTurningAnims = ini.GetOrCreate("General", "bDisableFirstPersonTurningAnims", 1, "; disable first person turning anims (they mess with shit and serve barely any purpose)");
#endif
	g_fixBlendSamePriority = ini.GetOrCreate("Blend Fixes", "bFixBlendSamePriority", 1, "; try to fix blending weapon animations with same bone priorities causing flickering as game tries to blend them with bones with the next high priority anim (usually mtidle.kf)");
	g_fixEndKeyTimeShorterThanStopTime = ini.GetOrCreate("Anim Fixes", "bFixEndKeyTimeShorterThanStopTime", 1, "; try to fix animations with broken export stop time where it's greater than the end key time and time of last transform data");
	g_fixWrongAKeyInRespectEndKeyAnim = ini.GetOrCreate("Anim Fixes", "bFixWrongAKeyInRespectEndKeyAnim", 1, "; try to fix animations where animator messed up the a: text key value in the first person animation. Previous versions of kNVSE did allow respectEndKey to affect this but newer versions will causing issues with people who update kNVSE but not animations.");
	//WriteRelJump(0x4949D0, AnimationHook);
	WriteRelCall(0x494989, HandleAnimationChange);
	WriteRelCall(0x495E2A, HandleAnimationChange);
	WriteRelCall(0x4956FF, HandleAnimationChange);
	WriteRelCall(0x4973B4, HandleAnimationChange);


	WriteRelJump(0x5F444F, KeyStringCrashFixHook);
	WriteRelJump(0x941E4C, EndAttackLoopHook);

	WriteRelJump(0x492BF2, LoopingReloadPauseFix::NoAimInReloadLoop);

	// WriteRelJump(0x4951D7, FixSpineBlendBug);


	if (g_fixSpineBlendBug)
		BlendFixes::ApplyAimBlendHooks();
	
	if (ini.GetOrCreate("General", "bFixLoopingReloads", 1, "; see https://www.youtube.com/watch?v=Vnh2PG-D15A"))
	{
		WriteRelCall(0x8BABCA, LoopingReloadFixHook);
		WriteRelCall(0x948CEC, LoopingReloadFixHook); // part of cancelling looping reload after shooting
	}

	if (ini.GetOrCreate("General", "bFixIdleStrafing", 1, "; allow player to strafe/turn sideways mid idle animation"))
		FixIdleAnimStrafe();

	if (ini.GetOrCreate("General", "bFixBlendAnimMultipliers", 1, "; fix blend times not being affected by animation multipliers (fixes animations playing twice in 1st person when an anim multiplier is big)"))
		WriteRelCall(0x4951D2, FixBlendMult);

	ApplyNiHooks();
	

#if 0 // obsidian fucked up looping reload priorities so the flicker bug appears with this enabled
	if (g_fixLoopingReloadStart)
	{
		// fix reloadloopstart blending into aim before reloadloop
		WriteRelCall(0x492CF6, INLINE_HOOK(void, __fastcall, AnimData* animData, void*, eAnimSequence sequenceId, bool bChar)
		{
			const auto* anim = GET_CALLER_VAR(BSAnimGroupSequence*, -0xA0, true);
			if (anim && anim->animGroup)
			{
				const auto groupId = anim->animGroup->GetBaseGroupID();
				if (groupId >= kAnimGroup_ReloadWStart && groupId <= kAnimGroup_ReloadZStart)
					return; // do nothing
			}
			// original call; blends to aim anim
			ThisStdCall(0x4994F0, animData, sequenceId, bChar);
		}));
	}
#endif


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
	// WriteRelCall(0x493DC0, NonExistingAnimHook<-0x98>); no go - causes stack overflow in PickAnimations since it calls LoadCustomAnimation that calls this
	WriteRelCall(0x493115, NonExistingAnimHook<-0x18C>);

	//WriteRelCall(0x490626, NonExistingAnimHook<-0x90>);
	//WriteRelCall(0x49022F, NonExistingAnimHook<-0x54>);
	//WriteRelCall(0x490066, NonExistingAnimHook<-0x54>);
	/* experimental end */


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
		auto* _ebp = GetParentBasePtr(_AddressOfReturnAddress(), true);
		auto* actor = *reinterpret_cast<Actor**>(_ebp - 0x1C);
		HandleOnReload(actor);
		return form->flags;
	}));

	WriteRelJump(0x490A45, PreventDuplicateAnimationsHook);

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
			if (iter->second->respectEndKey && iter->second->povState == POVSwitchState::POV1st)
				return firstPersonAnim->textKeyData;
		}
		return defaultData;
	}));


	// AnimData::GetSequenceOffsetPlusTimePassed
	// return 1st person anim time-passed in function where most or all AnimData::sequenceState1 are updated
	// This case would normally be handled by respectEndKey but if 3rd person anim has blend and 1st noBlend the anim times can get desynced
	WriteRelJump(0x493800, INLINE_HOOK(double, __fastcall, AnimData* animData, void*, BSAnimGroupSequence* anim)
	{
		// GetAnimTime doesn't work as it bugs out looping reloads in 3rd person
		const auto toAnimTime = [](const AnimData* animData, const BSAnimGroupSequence* seq)
		{
			return seq->offset + animData->timePassed;
		};
		const auto result = toAnimTime(animData, anim);
		if (animData != g_thePlayer->baseProcess->animData || !anim->animGroup)
			return result;
		const auto sequence = anim->animGroup->GetGroupInfo()->sequenceType;
		const auto anim1st = g_thePlayer->firstPersonAnimData->animSequence[sequence];
		if (!anim1st)
		{
			// 1st person anim may have ended before 3rd person anim (can happen with 1p anim noBlend and high speed mult)
			const auto iter = ra::find_if(g_timeTrackedAnims, _L(auto& p, p.second->anim3rdCounterpart == anim));
			if (iter == g_timeTrackedAnims.end())
				return result;
			const auto* timeTracked1stPersonAnim = iter->first;
			if (!timeTracked1stPersonAnim)
				return result;
			return toAnimTime(g_thePlayer->firstPersonAnimData, timeTracked1stPersonAnim);
		}
		if (!anim1st->animGroup)
			return result;
		if (anim1st->animGroup->GetBaseGroupID() != anim->animGroup->GetBaseGroupID())
			return result;
		if (const auto iter = g_timeTrackedAnims.find(anim1st); iter != g_timeTrackedAnims.end())
		{
			const auto& animTime = *iter->second;
			if (animTime.respectEndKey && animTime.povState == POVSwitchState::POV1st)
				return toAnimTime(g_thePlayer->firstPersonAnimData, anim1st);
		}
		return result;
	}));

	// Apply dest frame hook
	// NiControllerSequence::StartBlend
	WriteRelCall(0xA2F844, INLINE_HOOK(bool, __fastcall, NiControllerSequence* tempSeq,
		void*, NiControllerSequence* seq, float fDuration, float fDestFrame, int iPriority,
		float fSourceWeight, float fDestWeight, NiControllerSequence *pkTimeSyncSeq)
	{
		if (fDestFrame == 0.0f && seq->destFrame != -NI_INFINITY)
		{
			fDestFrame = seq->destFrame;
		}
		return ThisStdCall<bool>(0xA350D0, tempSeq, seq, fDuration, fDestFrame, iPriority, fSourceWeight, fDestWeight, pkTimeSyncSeq);
	}));

	// AnimData::RemovesAnimSequence
	WriteRelCall(0x496086, INLINE_HOOK(eAnimSequence, __fastcall, AnimData* animData)
	{
		// we are hooking in a mov instruction so this hook will be a bit unusual
		auto* addressOfReturn = GetLambdaAddrOfRetnAddr(_AddressOfReturnAddress());
		auto* parentEbp = GetParentBasePtr(addressOfReturn);

		// restore lost instructions
		*reinterpret_cast<AnimData**>(parentEbp - 0x8) = animData;
		
		const auto sequenceId = *reinterpret_cast<eAnimSequence*>(parentEbp + 0x8);
		const auto result = InterceptStopSequence::Dispatch(animData->actor, sequenceId);
		if (result && *result)
		{
			// jump to end of function
			*addressOfReturn = 0x49626B;
			return static_cast<eAnimSequence>(0);
		}
		*addressOfReturn = 0x49608C;
		return sequenceId; // this is kind of a hack because the next instruction is a mov eax, sequenceId
	}));
	PatchMemoryNop(0x496089 + 2, 1);

	ini.SaveFile(iniPath.c_str(), false);
}

