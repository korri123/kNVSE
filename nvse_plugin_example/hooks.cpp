#include "hooks.h"

#include <GameUI.h>
#include <span>
#include <unordered_set>

#include "additive_anims.h"
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
#include "movement_blend_fixes.h"
#include "TempEaseSequence.h"

bool g_startedAnimation = false;
BSAnimGroupSequence* g_lastLoopSequence = nullptr;
extern bool g_fixHolster;

std::map<std::pair<FullAnimGroupID, AnimData*>, std::deque<BSAnimGroupSequence*>> g_queuedReplaceAnims;


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

void Apply3rdPersonRespectEndKeyEaseInFix(AnimData* animData, BSAnimGroupSequence* anim3rd);

// UInt32 animGroupId, BSAnimGroupSequence** toMorph, UInt8* basePointer
BSAnimGroupSequence* __fastcall HandleAnimationChange(AnimData* animData, void*, BSAnimGroupSequence* destAnim, UInt16 animGroupId, eAnimSequence animSequence)
{
	const auto baseAnimGroup = static_cast<AnimGroupID>(animGroupId);

	if (animData && animData->actor)
	{
		std::optional<AnimationResult> animResult;
		BlendFixes::ApplyMissingUpDownAnims(animData);

		if (auto* queuedAnim = GetQueuedAnim(animData, animGroupId))
		{
			destAnim = queuedAnim;
			HandleExtraOperations(animData, queuedAnim);
		}
		// first check matchBaseGroupId
		else if (const auto lResult = GetActorAnimation(animGroupId & 0xFF, animData); lResult && lResult->animBundle->matchBaseGroupId)
		{
			if (auto* newAnim = LoadAnimationPath(*lResult, animData, animGroupId))
			{
				destAnim = newAnim;
			}
		}
		else if ((animResult = GetActorAnimation(animGroupId, animData)))
		{
			if (auto* newAnim = LoadAnimationPath(*animResult, animData, animGroupId))
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

	bool bSkip = false;
	if (auto* interceptedAnim = InterceptPlayAnimGroup::Dispatch(animData, destAnim, bSkip);
		interceptedAnim && interceptedAnim->animGroup)
	{
		destAnim = interceptedAnim;
		animGroupId = destAnim->animGroup->groupID;
	}
	else if (bSkip)
		return destAnim;

	if (g_pluginSettings.fixSpineBlendBug && BlendFixes::ApplyAimBlendFix(animData, destAnim) == BlendFixes::SKIP)
	{
		// BlendFixes::FixAimPriorities(animData, destAnim);
		return destAnim;
	}

	BSAnimGroupSequence* currentAnim = nullptr;
	if (destAnim && destAnim->animGroup)
		if (auto* groupInfo = destAnim->animGroup->GetGroupInfo())
			currentAnim = animData->animSequence[groupInfo->sequenceType];
	if (currentAnim)
		BlendFixes::FixPrematureFirstPersonEnd(animData, currentAnim);
	Apply3rdPersonRespectEndKeyEaseInFix(animData, destAnim);

	BSAnimGroupSequence* result;
	
	if (destAnim->animGroup && destAnim->animGroup->GetSequenceType() == kSequence_Movement)
		result = MovementBlendFixes::PlayMovementAnim(animData, destAnim);
	else
		result = animData->MorphOrBlendToSequence(destAnim, animGroupId, animSequence);
	
	if (destAnim && currentAnim)
	{
		auto* idle = animData->animSequence[kSequence_Idle];
		if (idle && idle->m_eState == NiControllerSequence::ANIMATING)
			BlendFixes::FixConflictingPriorities(currentAnim, destAnim, idle);
	}
	return result;
}

void Apply3rdPersonRespectEndKeyEaseInFix(AnimData* animData, BSAnimGroupSequence* anim3rd)
{
	if (animData != g_thePlayer->baseProcess->animData || g_thePlayer->IsThirdPerson() || !anim3rd || !anim3rd->animGroup)
		return;
	auto* anim1st = GetAnimByGroupID(g_thePlayer->firstPersonAnimData, anim3rd->animGroup->GetBaseGroupID());
	if (!anim1st || !anim1st->animGroup || !anim1st->m_spTextKeys)
		return;
	auto* textKeys = anim1st->m_spTextKeys;
	if (!textKeys->FindFirstByName("respectEndKey") && !textKeys->FindFirstByName("respectTextKeys"))
		return;
	if (textKeys->FindFirstByName("noBlend"))
		animData->noBlend120 = true;
	else
	{
		Save3rdPersonAnimGroupData(anim3rd);
		Set3rdPersonAnimTimes(anim3rd, anim1st);
	}
}

void DecreaseAttackTimer()
{
	*static_cast<UInt32*>(_AddressOfReturnAddress()) = 0x941E64;
	auto* baseProcess = g_thePlayer->baseProcess;
	const bool isFirstPerson = !g_thePlayer->IsThirdPerson();
	const auto* animData = g_thePlayer->GetAnimData(isFirstPerson);
	if (!g_lastLoopSequence || g_lastLoopSequence->m_pkOwner != animData->controllerManager)
		return baseProcess->ResetAttackLoopTimer(false);
	if (g_startedAnimation)
	{
		//p->time1D4 = g_lastLoopSequence->endKeyTime - g_lastLoopSequence->startTime; // start time is actually curTime
		baseProcess->time1D4 = g_lastLoopSequence->m_fEndKeyTime;
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

int AnimErrorLogHook(const char* fmt, ...)
{
	va_list	args;
	va_start(args, fmt);
	char buf[0x400];
	vsprintf_s(buf, sizeof buf, fmt, args);
	ERROR_LOG(FormatString("GAME: %s", buf));
	va_end(args);

	return 0;
}

UInt16 __fastcall LoopingReloadFixHook(AnimData* animData, void*, UInt16 groupID, bool noRecurse)
{
	const auto defaultRet = [&]
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
		"blendToReloadLoop", "scriptLine:", "allowAttack", "noFix", "allowClampInLocomotion"
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

void __fastcall FixBlendMult(BSAnimGroupSequence* anim, void*, float fTime, bool bUpdateInterpolators)
{
	const auto* animData = GET_CALLER_VAR(AnimData*, -0x5C);
	auto* animBlend = GET_CALLER_VAR_PTR(float*, -0x34);
	const auto mult = GetAnimMult(animData, static_cast<UInt8>(anim->animGroup->groupID));
	if (mult > 1.0f)
	{
		*animBlend /= mult;
	}
	// hooked call
	ThisStdCall(0xA328B0, anim, fTime, bUpdateInterpolators);
}

bool LookupCustomAnim(FullAnimGroupID groupId, AnimSequenceBase** base, AnimData* animData)
{
	// we do not want non existing 3rd anim data to try to get played as nothing gets played if it can't find it
	//if (animData == g_thePlayer->firstPersonAnimData)
	//	animData = g_thePlayer->baseProcess->GetAnimData();

	if (const auto animResult = GetActorAnimation(groupId, animData))
	{
		if (const auto animCtx = LoadCustomAnimation(*animResult->animBundle, groupId, animData))
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
}

BSAnimGroupSequence* GetAnimByFullGroupID(AnimData* animData, UInt16 groupId)
{
	if (const auto result = GetActorAnimation(groupId, animData))
		if (const auto ctx = LoadCustomAnimation(*result->animBundle, groupId, animData))
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
bool __fastcall NonExistingAnimHook(NiTPointerMap<AnimSequenceBase>* animMap, void*, UInt16 groupId, AnimSequenceBase** base)
{
	auto* parentBasePtr = GetParentBasePtr(_AddressOfReturnAddress());
	auto* animData = *reinterpret_cast<AnimData**>(parentBasePtr + AnimDataOffset);
	
	if ((*base = animMap->Lookup(groupId)))
		return true;

	// do this last as it's worse for performance
	if (LookupCustomAnim(groupId, base, animData)) [[msvc::noinline_calls]]
	{
		return true;
	}
	
	return false;
}

// here we first try to get kNVSE anims without murdering performance
template <int AnimDataOffset>
bool __fastcall OverrideWithCustomAnimHook(NiTPointerMap<AnimSequenceBase>* animMap, void*, UInt16 groupId, AnimSequenceBase** base)
{
	auto* parentBasePtr = GetParentBasePtr(_AddressOfReturnAddress());
	auto* animData = *reinterpret_cast<AnimData**>(parentBasePtr + AnimDataOffset);

	if (LookupCustomAnim(groupId, base, animData)) [[msvc::noinline_calls]]
	{
		return true;
	}
	
	if ((*base = animMap->Lookup(groupId)))
		return true;
	
	return false;
}

[[msvc::noinline]]
bool HasAnimBaseDuplicate(AnimSequenceBase* base, const BSAnimGroupSequence* anim)
{
	if (base->IsSingle()) // single anims are always valid
		return false;
	auto* multiple = static_cast<AnimSequenceMultiple*>(base);
	for (auto* entry : *multiple->anims)
	{
		if (_stricmp(entry->m_kName.CStr(), anim->m_kName.CStr()) == 0)
			return true;
	}
	return false;
}

// 0x490A35
void RemoveDuplicateAnimsHook() {
	auto* addrOfRetn = static_cast<UInt32*>(_AddressOfReturnAddress());
	auto* base = GET_CALLER_VAR(AnimSequenceBase*, -0x10);
	const auto* kfModel = GET_CALLER_VAR(KFModel*, 0x8);
	const auto* anim = kfModel->controllerSequence;
	if (HasAnimBaseDuplicate(base, anim)){
		*addrOfRetn = 0x490D7E;
	} else {
		// This hook replaces an opcode that takes 7 bytes, so return to next valid ins
		*addrOfRetn = 0x490A3C;
	}
}

namespace LoopingReloadPauseFix
{
	std::unordered_set<std::string_view> g_reloadStartBlendFixes;

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
		if (!g_reloadStartBlendFixes.contains(anim->m_kName.Str()))
		{
			if (IsPlayersOtherAnimData(animData) && !g_thePlayer->IsThirdPerson())
			{
				const auto seqType = GetSequenceType(anim->animGroup->GetBaseGroupID());
				auto* cur1stPersonAnim = g_thePlayer->firstPersonAnimData->animSequence[seqType];
				if (cur1stPersonAnim && g_reloadStartBlendFixes.contains(cur1stPersonAnim->m_kName.Str()))
					return newCondition();
			}
			return defaultCondition();
		}
		return newCondition();
	}

	void __fastcall NoAimInReloadLoopHook(AnimData* animData, void*, eAnimSequence sequenceId, bool bChar)
	{
		auto* addrOfRetn = static_cast<UInt32*>(_AddressOfReturnAddress());
		const static std::unordered_set ids = { kAnimGroup_ReloadWStart, kAnimGroup_ReloadYStart, kAnimGroup_ReloadXStart, kAnimGroup_ReloadZStart };
		auto* anim = animData->animSequence[sequenceId];

		const auto dontEndSequence = _L(, *addrOfRetn = 0x492BF8 );
		const auto endSequence = _L(, ThisStdCall(0x4994F0, animData, sequenceId, bChar));
		if (!anim || !anim->animGroup || !ids.contains(anim->animGroup->GetBaseGroupID()))
		{
			endSequence();
			return;
		}
		if (g_reloadStartBlendFixes.contains(anim->m_kName.Str()))
		{
			dontEndSequence();
			return;
		}
		if (IsPlayersOtherAnimData(animData) && !g_thePlayer->IsThirdPerson())
		{
			const auto seqType = anim->animGroup->GetSequenceType();
			auto* cur1stPersonAnim = g_thePlayer->firstPersonAnimData->animSequence[seqType];
			if (cur1stPersonAnim && g_reloadStartBlendFixes.contains(cur1stPersonAnim->m_kName.Str()))
			{
				dontEndSequence();
				return;
			}
		}
		endSequence();
	}
}

BSAnimGroupSequence* Find1stPersonRespectEndKeyAnim(AnimData* animData, BSAnimGroupSequence* anim3rd)
{
	if (animData != g_thePlayer->baseProcess->animData || !anim3rd || !anim3rd->animGroup)
		return nullptr;
	const auto iter = ra::find_if(g_timeTrackedAnims, [&](const auto& pair)
	{
		return pair.second->respectEndKeyData.anim3rdCounterpart == anim3rd;
	});
	if (iter != g_timeTrackedAnims.end())
	{
		const auto& animTime = *iter->second;
		const auto& respectEndKeyData = animTime.respectEndKeyData;
		if (!animTime.respectEndKey || respectEndKeyData.povState != POVSwitchState::POV1st || animTime.actorId != g_thePlayer->refID)
			return nullptr;
		auto* anim = iter->first;
		if (anim)
			return anim;
	}
	return nullptr;
}

bool IsReadyForAnim()
{
	UInt8 opcode = *reinterpret_cast<UInt8*>(0x9420E4);
	if (opcode == 0xE8)
	{
		// tweaks hooks here
		const auto addr = *reinterpret_cast<UInt32*>(0x9420E4 + 1);
		return ThisStdCall<bool>(addr, g_thePlayer->baseProcess);
	}
	return g_thePlayer->baseProcess->IsReadyForAnim();
}

namespace AllowAttackKey
{
	UInt32 hookedAddr = 0;

	const auto defaultResult = [](BaseProcess* process)
	{
		if (hookedAddr)
			return ThisStdCall<bool>(hookedAddr, process);
		return process->IsReadyForAnim();
	};

	bool HasAllowAttackKeyPassed()
	{
		
		const auto isFirstPerson = !g_thePlayer->IsThirdPerson();
		const auto iter = ra::find_if(g_timeTrackedAnims, [&](const auto& p)
		{
			return p.second->allowAttack && p.second->actorId == g_thePlayer->refID && p.second->firstPerson == isFirstPerson;
		});

		if (iter == g_timeTrackedAnims.end())
			return false;

		const auto* anim = iter->first;
		const auto allowAttackTime = iter->second->allowAttackTime;
		if (!anim || allowAttackTime == INVALID_TIME || anim->m_eState != NiControllerSequence::ANIMATING)
			return false;
		if (anim->m_fLastScaledTime < allowAttackTime)
			return false;
		return true;
	}

	bool HasInterruptLoopPassed()
	{
		const auto* animData = g_thePlayer->GetAnimData();
		const auto* attackSequence = animData->animSequence[kSequence_Weapon];
		if (!attackSequence || !attackSequence->m_spTextKeys->FindFirstByName("interruptLoop")
			|| attackSequence->m_eState != NiControllerSequence::ANIMATING)
			return false;
		const auto weaponInfo = g_thePlayer->baseProcess->GetWeaponInfo();
		if (!weaponInfo || !weaponInfo->weapon || !weaponInfo->weapon->IsAutomatic())
			return false;
		const auto attackSpeed = weaponInfo->GetShotTime();
		return attackSequence->m_fLastScaledTime >= attackSpeed;
	}

	bool __fastcall AllowAttackHook(Decoding::HighProcess* process)
	{
		if (process == g_thePlayer->baseProcess)
		{
			if (HasAllowAttackKeyPassed())
				return true;
			if (HasInterruptLoopPassed())
				return true;
		}
		
		return defaultResult(process);
	}

	UInt32 g_reloadAnimActionAddr1 = 0x8A7570;
	UInt32 g_reloadAnimActionAddr2 = 0x8A7570;

	template <UInt32* Address>
	AnimAction __fastcall ReloadAnimActionHook(Actor* actor)
	{
		if (HasAllowAttackKeyPassed())
			return kAnimAction_Attack;
		return ThisStdCall<AnimAction>(*Address, actor);
	}
	
	void ApplyHooks()
	{
		UInt8 opcode = *reinterpret_cast<UInt8*>(0x9420E4);
		if (opcode == 0xE8) // tweaks also hooks here
			hookedAddr = GetRelJumpAddr(0x9420E4);
		for (UInt32 patchAddr : {0x9420E4, 0x893E86, 0x941FE1})
		{
			WriteRelCall(patchAddr, AllowAttackHook);
			SafeWrite16(patchAddr + 5, 0xC084); // test al, al
		}

		WriteRelCall(0x893E32, ReloadAnimActionHook<&g_reloadAnimActionAddr1>, &g_reloadAnimActionAddr1);
		WriteRelCall(0x893E42, ReloadAnimActionHook<&g_reloadAnimActionAddr2>, &g_reloadAnimActionAddr2);
	}
	
}


namespace HolsterUnholsterLocomotionFixes
{
	void ApplyHooks()
	{
	
		// AnimData::GetNthSequenceGroupID(animData, sequenceType_1);
		// Fix issue where new movement anim is played during equip or unequip
		WriteRelCall(0x897237, INLINE_HOOK(UInt16, __fastcall, AnimData* animData, void*, eAnimSequence sequenceType)
		{
			auto* addrOfRetn = GetLambdaAddrOfRetnAddr(_AddressOfReturnAddress());
			auto* _ebp = GetParentBasePtr(addrOfRetn);
			const auto groupId = animData->groupIDs[sequenceType];
			const auto nextGroupId = *reinterpret_cast<UInt16*>(_ebp - 0x24);
			if (sequenceType == kSequence_Movement && groupId != nextGroupId && (groupId & 0xFF) == (nextGroupId & 0xFF))
			{
				if (auto* weaponAnim = animData->animSequence[kSequence_Weapon]; weaponAnim && weaponAnim->animGroup)
				{
					const auto baseWeaponGroupId = weaponAnim->animGroup->GetBaseGroupID();
					if (baseWeaponGroupId == kAnimGroup_Equip || baseWeaponGroupId == kAnimGroup_Unequip)
					{
						*addrOfRetn = 0x897682;
						return 0;
					}
				}
			}
			return groupId;
		}));
	}

	bool IsTryingToEndEquip(UInt32* addrOfRetn)
	{
		// need to check here to prevent game from trying to end movement sequence during equip/unequip

		auto* _ebp = GetParentBasePtr(addrOfRetn);
		const auto nextGroupId = *reinterpret_cast<AnimGroupID*>(_ebp - 0x40);
		return nextGroupId == kAnimGroup_Equip || nextGroupId == kAnimGroup_Unequip;
	}
}


void ApplyFixLoopingReloadStartHooks()
{
#if 0 // obsidian fucked up looping reload priorities so the flicker bug appears with this enabled
	if (g_fixLoopingReloadStart)
	{
		// fix reloadloopstart blending into aim before reloadloop
		WriteRelCall(0x492CF6, INLINE_HOOK(void, __fastcall, AnimData* animData, void*, eAnimSequence sequenceId, bool bChar)
		{
			const auto* anim = GET_CALLER_VAR_LAMBDA(BSAnimGroupSequence*, -0xA0);
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
}

PluginINISettings g_pluginSettings;
thread_local PluginGlobalData g_globals;

thread_local bool g_isThreadCacheEnabled = false;

SynchronizedQueue g_aiLinearTask1Queue;

void ApplyHooks()
{
	const auto iniPath = R"(Data\NVSE\Plugins\kNVSE.ini)";
	CSimpleIniA ini;
	ini.SetUnicode();
	if (ini.LoadFile(iniPath) < 0)
	{
		ERROR_LOG("INI file missing, generating a new one.");
	}
	g_logLevel = ini.GetOrCreate("General", "iConsoleLogLevel", 1, "; 0 = no log, 1 = kNVSE.log, 2 = console log");
	g_errorLogLevel = ini.GetOrCreate("General", "iErrorLogLevel", 1, "; 0 = no log, 1 = kNVSE.log, 2 = error console log");
	auto& conf = g_pluginSettings;

#if 0
	conf.fixSpineBlendBug = ini.GetOrCreate("General", "bFixSpineBlendBug", 1, "; fix spine blend bug when aiming down sights in 3rd person and cancelling the aim while looking up or down");
	conf.fixBlendSamePriority = ini.GetOrCreate("General", "bFixBlendSamePriority", 1, "; fix blending weapon animations with same bone priorities causing flickering as game tries to blend them with bones with the next high priority");
	conf.fixAttackISTransition = ini.GetOrCreate("General", "bFixAttackISTransition", 1, "; fix iron sight attack anims being glued to player's face even after player has released aim control");
	conf.fixLoopingReloadStart = ini.GetOrCreate("General", "bFixLoopingReloadStart", 1, "; fix looping reload start anims transitioning to aim anim before main looping reload anim");

	g_disableFirstPersonTurningAnims = ini.GetOrCreate("General", "bDisableFirstPersonTurningAnims", 1, "; disable first person turning anims (they mess with shit and serve barely any purpose)");
#endif
	conf.fixBlendSamePriority = ini.GetOrCreate("Blend Fixes", "bFixBlendSamePriority", 1, "; try to fix blending weapon animations with same bone priorities causing flickering as game tries to blend them with bones with the next high priority anim (usually mtidle.kf)");
	conf.fixEndKeyTimeShorterThanStopTime = ini.GetOrCreate("Anim Fixes", "bFixEndKeyTimeShorterThanStopTime", 1, "; try to fix animations with broken export stop time where it's greater than the end key time and time of last transform data");
	conf.fixWrongAKeyInRespectEndKeyAnim = ini.GetOrCreate("Anim Fixes", "bFixWrongAKeyInRespectEndKeyAnim", 1, "; try to fix animations where animator messed up the a: text key value in the first person animation. Previous versions of kNVSE ignored this key in first person animations.");
	conf.fixWrongPrnKey = ini.GetOrCreate("Anim Fixes", "bFixWrongPrnKey", 1, "; try to fix animations where animator messed up the prn text key value in the first person animation. Previous versions of kNVSE ignored this key in first person animations.");
	conf.fixWrongAnimName = ini.GetOrCreate("Anim Fixes", "bFixWrongAnimName", 1, "; try to fix animations where the name of the animation file does not match anim group name.");
	conf.fixMissingPrnKey = ini.GetOrCreate("Anim Fixes", "bFixMissingPrnKey", 1, "; try to fix animations where the prn key is missing in the first person animation.");
	conf.fixReloadStartAllowReloadTweak = ini.GetOrCreate("Anim Fixes", "bFixReloadStartAllowReloadTweak", 1, "; fix looping reloads in Stewie Tweak \"Allow Reload In Attack\" when attacking when attack is done when Aim is EaseIn and ReloadXStart becomes TransDest");
	const std::string legacyAnimTimePaths = ini.GetOrCreate("Anim Fixes", "sLegacyAnimTimePaths", "B42Inject,B42Interact,B42Loot", "; use legacy anim time algorithm for these paths (these mods rely on bugged behavior from previous versions of kNVSE)");
	if (!legacyAnimTimePaths.empty())
	{
		conf.legacyAnimTimePaths = SplitString(legacyAnimTimePaths);
	}
	//WriteRelJump(0x4949D0, AnimationHook);
	
	WriteRelCall(0x494989, HandleAnimationChange);
	WriteRelCall(0x495E2A, HandleAnimationChange);
	WriteRelCall(0x4956FF, HandleAnimationChange);
	WriteRelCall(0x4973B4, HandleAnimationChange);

	WriteRelJump(0x5F444F, KeyStringCrashFixHook);
	WriteRelCall(0x941E4C, DecreaseAttackTimer);

	WriteRelCall(0x492CF6, LoopingReloadPauseFix::NoAimInReloadLoopHook);

	// WriteRelJump(0x4951D7, FixSpineBlendBug);

	HolsterUnholsterLocomotionFixes::ApplyHooks();
	
	if (conf.fixSpineBlendBug)
		BlendFixes::ApplyAimBlendHooks();
	BlendFixes::ApplyHooks();

	
	
	if (ini.GetOrCreate("Engine Fixes", "bFixLoopingReloads", 1, "; see https://www.youtube.com/watch?v=Vnh2PG-D15A"))
	{
		WriteRelCall(0x8BABCA, LoopingReloadFixHook);
		WriteRelCall(0x948CEC, LoopingReloadFixHook); // part of cancelling looping reload after shooting
	}

	if (ini.GetOrCreate("Engine Fixes", "bFixIdleStrafing", 1, "; allow player to strafe/turn sideways mid idle animation"))
		FixIdleAnimStrafe();

	if (ini.GetOrCreate("Blend Fixes", "bFixBlendAnimMultipliers", 1, "; fix blend times not being affected by animation multipliers (fixes animations playing twice in 1st person when an anim multiplier is big)"))
		WriteRelCall(0x4951D2, FixBlendMult);

	if (ini.SaveFile(iniPath, false) < 0)
	{
		ERROR_LOG("Failed to save ini file");
	}

	ApplyNiHooks();

#if 1
	// attempt to fix anims that don't get loaded since they aren't in the game to begin with
	
	// AnimData::FindNearestGroupIDAnim
	WriteRelCall(0x49575B, NonExistingAnimHook<-0x10>);
	WriteRelCall(0x495965, NonExistingAnimHook<-0x10>);
	WriteRelCall(0x4959B9, NonExistingAnimHook<-0x10>);
	WriteRelCall(0x495A03, NonExistingAnimHook<-0x10>); 
	// AnimData::MorphToSequenceIDOrGroup
	WriteRelCall(0x4948E6, NonExistingAnimHook<-0x14>); 
	WriteRelCall(0x495DC6, NonExistingAnimHook<-0x10>);
	WriteRelCall(0x4956CE, NonExistingAnimHook<-0x14>);

	// AnimData::GetPlayingAnimGroupMovementVectorMagnitude (not sure this is needed)
	WriteRelCall(0x49431B, OverrideWithCustomAnimHook<-0xC>);
	// determines the velocity of jumploopforward, do not remove since enhanced movement relies on it
	WriteRelCall(0x493115, OverrideWithCustomAnimHook<-0x18C>);

	
	/* experimental */
	// WriteRelCall(0x9D0E80, NonExistingAnimHook<-0x1C>);
	// WriteRelCall(0x97FF06, NonExistingAnimHook<-0x8>);
	// WriteRelCall(0x97FE09, NonExistingAnimHook<-0xC>);
	// WriteRelCall(0x97F36D, NonExistingAnimHook<-0xC>);
	// WriteRelCall(0x8B7985, NonExistingAnimHook<-0x14>);
	// WriteRelCall(0x495630, NonExistingAnimHook<-0x14>);
	// WriteRelCall(0x493DC0, NonExistingAnimHook<-0x98>); no go - causes stack overflow in PickAnimations since it calls LoadCustomAnimation that calls this
	// WriteRelCall(0x493115, NonExistingAnimHook<-0x18C>);
	//WriteRelCall(0x490626, NonExistingAnimHook<-0x90>);
	//WriteRelCall(0x49022F, NonExistingAnimHook<-0x54>);
	//WriteRelCall(0x490066, NonExistingAnimHook<-0x54>);
	/* experimental end */

	// AnimData::GetSequenceBaseFromMap
	WriteRelCall(0x49472B, OverrideWithCustomAnimHook<-0x8>);
	//  AnimData::GetAnimGroupForID
	WriteRelCall(0x49651B, OverrideWithCustomAnimHook<-0x8>);
#endif

	// AnimData destructor
	WriteRelCall(0x48FB82, INLINE_HOOK(void, __fastcall, AnimData* animData)
	{
		HandleOnAnimDataDelete(animData);

		// hooked call
		ThisStdCall(0x48FF50, animData);
	}));

	// BSTasklet::SetData
	// Runs when actor switches ammo type in Stewie Tweaks
	using BSTasklet = void;
	static UInt32 uiBSTaskletSetDataAddr1;
	WriteRelCall(0x9465FF, INLINE_HOOK(void, __fastcall, BSTasklet* tasklet, void*, BaseProcess::AmmoInfo* ammoInfo)
	{
		OnReloadHandler::SetDidReload(g_thePlayer, ReloadType::AmmoSwap);
		ThisStdCall(uiBSTaskletSetDataAddr1, tasklet, ammoInfo);
	}), &uiBSTaskletSetDataAddr1);

	// BSTasklet::SetData
	// Runs when actor switches ammo type in pip-boy
	static UInt32 uiBSTaskletSetDataAddr2;
	WriteRelCall(0x780723, INLINE_HOOK(void, __fastcall, BSTasklet* tasklet, void*, BaseProcess::AmmoInfo* ammoInfo)
	{
		OnReloadHandler::SetDidReload(g_thePlayer, ReloadType::AmmoSwap);
		ThisStdCall(uiBSTaskletSetDataAddr2, tasklet, ammoInfo);
	}), &uiBSTaskletSetDataAddr2);


	WriteRelCall(0x490A35, RemoveDuplicateAnimsHook);

	// AnimData::GetNextWeaponSequenceKey
	// fixes respectEndKey not working for a: keys
	WriteRelCall(0x495E6C, INLINE_HOOK(NiTextKeyExtraData*, __fastcall, BSAnimGroupSequence* sequence)
	{
		auto* defaultData = sequence->m_spTextKeys;
		const auto* anim1st = Find1stPersonRespectEndKeyAnim(g_thePlayer->firstPersonAnimData, sequence);
		if (anim1st)
			return anim1st->m_spTextKeys;
		return defaultData;
	}));

	// BSAnimGroupSequence::GetCycleType
	// stop dumb limitation on CLAMP sequences not being allowed to locomotion
	WriteRelCall(0x4909BC, INLINE_HOOK(UInt32, __fastcall, BSAnimGroupSequence* anim)
	{
		if (anim->m_spTextKeys->FindFirstByName("allowClampInLocomotion"))
			return NiControllerSequence::kCycle_Loop;
		return anim->m_eCycleType;
	}));

	// BSAnimGroupSequence* destructor
	WriteRelCall(0x4EEB4B, INLINE_HOOK(void, __fastcall, BSAnimGroupSequence* anim)
	{
		if (AdditiveManager::IsAdditiveSequence(anim))
			AdditiveManager::EraseAdditiveSequence(anim);
		// EraseTimeTrackedAnim(anim); we do store a smart pointer to the anim in g_timeTrackedAnims
		// hooked call
		ThisStdCall(0xA35640, anim);
	}));

	

#if 0
	// AnimData::GetSequenceOffsetPlusTimePassed
	// return 1st person anim time-passed in function where most or all AnimData::sequenceState1 are updated
	// This case would normally be handled by respectEndKey but if 3rd person anim has blend and 1st noBlend the anim times can get desynced
	// Also, animData->timePassed and animData1st->timePassed will desync as well
	WriteRelJump(0x493800, INLINE_HOOK(double, __fastcall, AnimData* animData, void*, BSAnimGroupSequence* anim)
	{
		// GetAnimTime doesn't work as it bugs out looping reloads in 3rd person
		const auto toAnimTime = [](const AnimData* animData, const BSAnimGroupSequence* seq)
		{
			return seq->offset + animData->timePassed;
		};
		if (const auto* anim1st = Find1stPersonRespectEndKeyAnim(animData, anim))
			return toAnimTime(g_thePlayer->firstPersonAnimData, anim1st);
		return toAnimTime(animData, anim);
	}));
#endif
	
	// Apply dest frame hook
	// NiControllerSequence::StartBlend
	WriteRelCall(0xA2F844, INLINE_HOOK(bool, __fastcall, NiControllerSequence* tempSeq,
		void*, NiControllerSequence* seq, float fDuration, float fDestFrame, int iPriority,
		float fSourceWeight, float fDestWeight, NiControllerSequence *pkTimeSyncSeq)
	{
		if (fDestFrame == 0.0f)
		{
			auto* destFrameKey = seq->m_spTextKeys->FindFirstByName("fDestFrame");
			if (destFrameKey && destFrameKey->m_fTime != -NI_INFINITY)
			{
				fDestFrame = destFrameKey->m_fTime;
				destFrameKey->SetTime(-NI_INFINITY);
			}
		}
		return ThisStdCall<bool>(0xA350D0, tempSeq, seq, fDuration, fDestFrame, iPriority, fSourceWeight, fDestWeight, pkTimeSyncSeq);
	}));

	// AnimData::RemovesAnimSequence
	WriteRelCall(0x4994F6, INLINE_HOOK(eAnimSequence, __fastcall, AnimData* animData)
	{
		// we are hooking in a mov instruction so this hook will be a bit unusual
		auto* addressOfReturn = GetLambdaAddrOfRetnAddr(_AddressOfReturnAddress());
		auto* parentEbp = GetParentBasePtr(addressOfReturn);

		// restore lost instructions
		*reinterpret_cast<AnimData**>(parentEbp - 0x24) = animData;
		
		const auto sequenceId = *reinterpret_cast<eAnimSequence*>(parentEbp + 0x8);
		const auto result = InterceptStopSequence::Dispatch(animData->actor, sequenceId);
		if (result && *result)
		{
			// jump to end of function
			*addressOfReturn = 0x49979C;
			return static_cast<eAnimSequence>(0);
		}
		*addressOfReturn = 0x4994FC;
		return sequenceId; // this is kind of a hack because the next instruction is a mov eax, sequenceId
	}));
	PatchMemoryNop(0x4994F9 + 2, 1);

	

	// BSAnimGroupSequence::GetName
	// apply text key fixes before AnimGroup is init
	WriteRelCall(0x43B838, INLINE_HOOK(NiFixedString*, __fastcall, BSAnimGroupSequence* anim)
	{
		auto* fileName = GET_CALLER_VAR_LAMBDA(const char*, 0x8);
		
		if (anim->m_spTextKeys) [[msvc::noinline_calls]]
		{
			AnimFixes::EraseNullTextKeys(anim);
			AnimFixes::FixInconsistentEndTime(anim);
			// AnimFixes::EraseNegativeAnimKeys(anim);
			AnimFixes::FixWrongKFName(anim, fileName);
			AnimFixes::FixMissingPrnKey(anim, fileName);
		}
		return &anim->m_kName;
	}));

	// BSAnimGroupSequence::SetSequenceName
	WriteRelCall(0x43B99A, INLINE_HOOK(void, __fastcall, BSAnimGroupSequence* anim, void*, NiFixedString* name)
	{
		// we want to force every animation to have lowercase names so we can look them up case insensitive in NiTPointerMap
		sv::stack_string<0x400> newName("%s", name->data);
		newName.to_lower();
		anim->m_kName = newName.c_str();
	}));

	// AnimData::PlayAnimGroup
	// used to rig GetCurrentAmmoRounds command to add 1 to the result if we are about to play a looping reload anim
	WriteRelCall(0x8BAD2C, INLINE_HOOK(void, __fastcall, AnimData *animData, void*, UInt16 groupID, int flags, int queuedState, eAnimSequence sequenceID)
	{
		g_globals.isInLoopingReloadPlayAnim = true;
		ThisStdCall(0x494740, animData, groupID, flags, queuedState, sequenceID);
		g_globals.isInLoopingReloadPlayAnim = false;
	}));

	const auto writeAttackLoopToAimHooks = []
	{
		const static auto shouldApplyAttackLoopToAim = []
		{
			const auto* animData = g_thePlayer->GetAnimData();
			const auto* baseProcess = g_thePlayer->baseProcess;
			const auto* attackSequence = animData->animSequence[kSequence_Weapon];
			const auto* ammoInfo = baseProcess->GetAmmoInfo();
			const auto hasInterrupt = [&]  { return attackSequence && attackSequence->m_spTextKeys->FindFirstByName("interruptLoop"); };
			return ammoInfo && ammoInfo->count == 0 && hasInterrupt();
		};
		
		// bananasuicide wants no interrupt when depressing aim key on last shot
		// Plays Aim directly instead of AttackLoop when ammo is depleted
		// Actor::AimWeapon
		WriteRelCall(0x9420BD, INLINE_HOOK(void, __fastcall, PlayerCharacter* player, void*, bool isAiming, bool a2, bool a3)
		{
			ThisStdCall(0x8BB650, player, isAiming, a2, a3);

			auto* animData = g_thePlayer->GetAnimData();

			if (!shouldApplyAttackLoopToAim())
				return;

			const auto groupId = GetNearestGroupID(animData, kAnimGroup_Aim);
			GameFuncs::PlayAnimGroup(animData, groupId, 1, -1, -1);
		}));

		// Plays AimIS directly instead of AttackLoopIS when ammo is depleted and aim is pressed
		// Actor::AimWeapon
		WriteRelCall(0x94202C, INLINE_HOOK(void, __fastcall, PlayerCharacter* player, void*, bool isAiming, bool a2, bool a3)
		{
			ThisStdCall(0x8BB650, player, isAiming, a2, a3);

			auto* animData = g_thePlayer->GetAnimData();

			if (!shouldApplyAttackLoopToAim())
				return;

			const auto groupId = GetNearestGroupID(animData, kAnimGroup_AimIS);
			GameFuncs::PlayAnimGroup(animData, groupId, 1, -1, -1);
		}));
	};
	writeAttackLoopToAimHooks();
	

	const auto writeInterruptLoopAllowReloadHooks = []
	{
		// AnimData::GetSequenceState1(animData, kSequence_Weapon)
		WriteRelCall(0x949720, INLINE_HOOK(UInt32, __fastcall, AnimData* animData3rd, void*, eAnimSequence sequenceId)
		{
			const auto defaultResult = animData3rd->sequenceState1[sequenceId];
			if (animData3rd->actor != g_thePlayer)
				return defaultResult;
			const auto* animData = g_thePlayer->GetAnimData();
			const auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo();
			const auto* attackSequence = animData->animSequence[kSequence_Weapon];
			if (!attackSequence || !attackSequence->m_spTextKeys->FindFirstByName("interruptLoop")
				|| attackSequence->m_eState != NiControllerSequence::ANIMATING
				|| !weaponInfo || !weaponInfo->weapon)
				return defaultResult;
			const auto attackTime = weaponInfo->GetShotTime();
			if (attackSequence->m_fLastScaledTime < attackTime)
				return defaultResult;
			return kSeqState_Unk3;
		}));
	};
	writeInterruptLoopAllowReloadHooks();

	// fix looping reloads in stewie tweak allow reload when attacking when attack is done when Aim is EaseIn and ReloadXStart is TransDest
	WriteRelCall(0x8956BE, INLINE_HOOK(float, __fastcall, AnimData* animData, void*, BSAnimGroupSequence* anim)
	{
		if (g_pluginSettings.fixReloadStartAllowReloadTweak && anim->m_eState == NiControllerSequence::TRANSDEST)
			return anim->m_fLastScaledTime;
		return ThisStdCall<float>(0x493800, animData, anim);
	}));


#if _DEBUG
	// BSSimpleList<void *>::IsEmpty(ListMasters)
	// stop game from not loading bsa files for mods with empty master list
	WriteRelCall(0x46375C, INLINE_HOOK(bool, __fastcall, BSSimpleList<ModInfo>* modList)
	{
		return false;
	}));
#endif

	static UInt32 uiScriptLocalsClearOptimizationsAddr;
	WriteRelCall(0x8C7F36, INLINE_HOOK(void, __cdecl)
	{
		g_isThreadCacheEnabled = true;
		ClearResultCaches();
		CdeclCall<void>(uiScriptLocalsClearOptimizationsAddr);
	}), &uiScriptLocalsClearOptimizationsAddr);

#if 0
	using ProcessManager = void;
	static UInt32 uiUpdateHighProcessAnimationsAddr;
	WriteRelCall(0x8C7DF3, INLINE_HOOK(void, __fastcall, ProcessManager* processManager)
	{
		g_isThreadCacheEnabled = true;
		g_aiLinearTask1Queue.RunAllAndClear();
		ThisStdCall(uiUpdateHighProcessAnimationsAddr, processManager);
	}), &uiUpdateHighProcessAnimationsAddr);
#endif
	AdditiveManager::WriteHooks();

	static UInt32 uiAnimDataUpdateControllersAddr;
	WriteRelCall(0x8885C9, INLINE_HOOK(void, __fastcall, AnimData* animData, void*, Actor* actor)
	{
		OnActorUpdateAnimation::Dispatch(actor);
		ThisStdCall(uiAnimDataUpdateControllersAddr, animData, actor);
	}), &uiAnimDataUpdateControllersAddr);

	static UInt32 uiGetAttackSpeedMultGetAnimAddr;
	WriteRelCall(0x645DE0, INLINE_HOOK(Animation*, __fastcall, PlayerCharacter *pc, void*, bool bFirstPerson)
	{
		return ThisStdCall<Animation*>(0x950A60, pc, !pc->isInThirdPerson);
	}), &uiGetAttackSpeedMultGetAnimAddr);

	static UInt32 uiGetAttackSpeedMultGetGroupAddr;
	WriteRelCall(0x645F51, INLINE_HOOK(uint32_t, __cdecl, uint32_t a1)
	{
		return CdeclCall<uint32_t>(0x5F2440, a1) & 0xFF;
	}), & uiGetAttackSpeedMultGetGroupAddr);

	static UInt32 uiGetAttackSpeedMultGetAnimAttackMultAddr;
	WriteRelCall(0x645EE0, INLINE_HOOK(double, __fastcall, TESObjectWEAP* weap, void*, bool hasMod)
	{
		if (GET_CALLER_VAR_LAMBDA(Actor*, 0x8) == PlayerCharacter::GetSingleton()) {
			return ThisStdCall<double>(0x646020, weap, false);
		}

		return ThisStdCall<double>(0x646020, weap, hasMod);
	}), &uiGetAttackSpeedMultGetAnimAttackMultAddr);
	
}

void WriteDelayedHooks()
{
	NiHooks::WriteDelayedHooks();
	AllowAttackKey::ApplyHooks();

	// hooking TESForm::GetFlags
	// Runs on empty clip when reloading
	// nvac hooks here so it has to be delayed
	static UInt32 uiTESFormGetFlagsAddr = 0x44DDC0;
	WriteRelCall(0x8A8C1B, INLINE_HOOK(UInt32, __fastcall, TESForm* form)
	{
		auto* actor = GET_CALLER_VAR_LAMBDA(Actor*, -0x1C);
		[[msvc::noinline_calls]] { OnReloadHandler::SetDidReload(actor, ReloadType::NonPartial); }
		return ThisStdCall<UInt32>(uiTESFormGetFlagsAddr, form);
	}), &uiTESFormGetFlagsAddr);

	// TESForm::GetFlags
	// Runs when player presses the reload control
	static UInt32 uiTESFormGetFlagsAddr2 = 0x44DDC0;
	WriteRelCall(0x9497A3, INLINE_HOOK(UInt32, __fastcall, TESForm* form)
	{
		auto* ammoInfo = g_thePlayer->baseProcess->GetAmmoInfo();
		if (ammoInfo)
		{
			const auto reloadType = ammoInfo->count == 0 ? ReloadType::NonPartial : ReloadType::Partial;
			OnReloadHandler::SetDidReload(g_thePlayer, reloadType);
		}
		return ThisStdCall<UInt32>(uiTESFormGetFlagsAddr2, form);
	}), &uiTESFormGetFlagsAddr2);

	if (auto* cmdGetCurrentAmmoRounds = const_cast<CommandInfo*>(g_cmdTable->GetByName("GetCurrentAmmoRounds")))
	{
		static auto* defaultExec = cmdGetCurrentAmmoRounds->execute;
		cmdGetCurrentAmmoRounds->execute = [](COMMAND_ARGS) -> bool
		{
			defaultExec(PASS_COMMAND_ARGS);
			if (g_globals.isInLoopingReloadPlayAnim && g_globals.isInConditionFunction)
				*result = *result + 1;
			return true;
		};
	}


#if _DEBUG && 1

#if 0
	// AnimData::ResetSequenceState
	WriteRelCall(0x494D86, INLINE_HOOK(void, __fastcall, AnimData* animData, void*, UInt32 sequenceId, float fEaseOut)
	{
		if (sequenceId == kSequence_Movement)
		{
#if 1
			auto* anim = animData->animSequence[sequenceId];
			if (anim)
			{
				fEaseOut = anim->GetEaseOutTime();
			}
			else
			{
				fEaseOut = 0.2f;
			}
#endif
		}
		ThisStdCall(0x496080, animData, sequenceId, fEaseOut);
	}));
	//SafeWriteBuf(0xA35093, "\xEB\x15\x90", 3);
#endif
	WriteRelCall(0x897712, INLINE_HOOK(NiControllerSequence::AnimState, __fastcall, BSAnimGroupSequence* anim)
	{
		// INACTIVE -> do not end movement
		// ANIMATING -> end movement
		auto* addrOfRetn = GetLambdaAddrOfRetnAddr(_AddressOfReturnAddress());
		if (HolsterUnholsterLocomotionFixes::IsTryingToEndEquip(addrOfRetn))
			return NiControllerSequence::INACTIVE;
		if (anim->animGroup->GetBaseGroupID() == kAnimGroup_JumpLand)
			return anim->m_fLastScaledTime >= anim->m_fEndKeyTime ? NiControllerSequence::ANIMATING : NiControllerSequence::INACTIVE;
		// stop game from not ending move anims immediately
		return NiControllerSequence::ANIMATING;
	}));


	// NiControllerManager::DeactivateSequence
	WriteRelCall(0x496208, INLINE_HOOK(bool, __fastcall, NiControllerManager* manager, void*, BSAnimGroupSequence* pkSequence, float fEaseOut)
	{
		if (pkSequence->animGroup->GetSequenceType() == kSequence_Movement && pkSequence->m_eState == NiControllerSequence::EASEIN && fEaseOut > 0.0f)
		{
			return pkSequence->DeactivateNoReset(fEaseOut);
		}
		return ThisStdCall<bool>(0x47B220, manager, pkSequence, fEaseOut);
	}));

	// NiControllerManager::CreateTempBlendSequence
	WriteRelCall(0xA2F817, INLINE_HOOK(NiControllerSequence*, __fastcall, NiControllerManager* pkManager, void*, NiControllerSequence* pkSequence, NiControllerSequence* pkTimeSyncSeq)
	{
		auto* result = ThisStdCall<NiControllerSequence*>(0xA2F170, pkManager, pkSequence, pkTimeSyncSeq);
		const static auto sTempBlendSequenceName = NiFixedString("__TempBlendSequence__");
		result->m_kName = sTempBlendSequenceName;
		return result;
	}));

	// TurnLeft/Right hook
	WriteRelCall(0x896C78, INLINE_HOOK(void, __fastcall)
	{
		auto* addrOfRetn = GetLambdaAddrOfRetnAddr(_AddressOfReturnAddress());
		const auto* actor = GET_CALLER_VAR_LAMBDA(Actor*, -0x10C);
		auto* groupId = GET_CALLER_VAR_PTR_LAMBDA(UInt16*, -0x40);
		const auto moveFlags = actor->actorMover ? actor->actorMover->GetMovementFlags() : 0;

		const bool turningLeft = (moveFlags & kMoveFlag_TurnLeft) != 0;
		const bool turningRight = (moveFlags & kMoveFlag_TurnRight) != 0;
		if (turningLeft || turningRight)
		{
			NiControllerSequence* moveSequence;
			[[msvc::noinline_calls]] {
				moveSequence = actor->baseProcess->GetAnimData()->controllerManager->FindSequence([](const NiControllerSequence* seq)
				{
					if (NOT_TYPE(seq, BSAnimGroupSequence))
						return false;
					TESAnimGroup* animGroup = static_cast<const BSAnimGroupSequence*>(seq)->animGroup;
					if (!animGroup)
						return false;
					return animGroup->IsBaseMovement();
				});
			}
			if (!moveSequence)
			{
				if (turningLeft)
				{
					*groupId = kAnimGroup_TurnLeft;
				}
				else if (turningRight)
				{
					*groupId = kAnimGroup_TurnRight;
				}
			}
		}
		*addrOfRetn = 0x896C9A;
	}));
	PatchMemoryNop(0x896C7C + 1, 2);

	// allow move in jumpland
	// bhkCharacterController::GetContextHkState
	WriteRelCall(0x8964E1, INLINE_HOOK(UInt32, __fastcall, bhkCharacterController* controller)
	{
		auto* actor = GET_CALLER_VAR_LAMBDA(Actor*, -0x10C);
		auto* animData = actor->baseProcess->GetAnimData();
		const auto moveGroupId = static_cast<AnimGroupID>(animData->groupIDs[kSequence_Movement]);

		const auto result = ThisStdCall<UInt32>(0x5C0880, controller);
		//constexpr auto kCharControllerState_Jumping = 1;
		//constexpr auto kCharControllerState_InAir = 2;
		//if (result == kCharControllerState_Jumping || result == kCharControllerState_InAir)
		//	return result;

		if (moveGroupId == kAnimGroup_JumpLand)
		{
			GameFuncs::Actor_SetAnimActionAndSequence(actor, Decoding::kAnimAction_None, nullptr);
		}
		
		return result;
	}));

#if 0
	// BSAnimGroupSequence::Deactivate when easing out
	WriteVirtualCall(0x494BCB, INLINE_HOOK(bool, __fastcall, BSAnimGroupSequence* sequence, void*, float fEaseOutTime, bool bTransition)
	{
		if (sequence->animGroup->GetSequenceType() == kSequence_Movement && sequence->m_eState == NiControllerSequence::EASEOUT)
		{
			return true;
		}
		return sequence->Deactivate(fEaseOutTime, bTransition);
	}));
#endif
#endif
	
}
