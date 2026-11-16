#include "hooks.h"

#include <GameUI.h>
#include <shared_mutex>
#include <span>
#include <unordered_set>

#include "accum_blend_fix.h"
#include "additive_anims.h"
#include "aim_pitch.h"
#include "aim_transitions.h"
#include "anim_fixes.h"
#include "file_animations.h"
#include "fire_direction.h"
#include "anim_switch.h"
#include "GameProcess.h"
#include "GameObjects.h"
#include "commands_animation.h"
#include "SafeWrite.h"
#include "nitypes.h"
#include "utility.h"
#include "main.h"
#include "nihooks.h"
#include "blend_fixes.h"
#include "blend_smoothing.h"
#include "jip_fixes.h"
#include "knvse_events.h"
#include "movement_blend_fixes.h"
#include "movement_transitions.h"
#include "npc_looping_reload_fix.h"
#include "pose_blend_fixes.h"
#include "respect_end_key.h"
#include "sequence_extradata.h"
#include "text_key_arguments.h"
#include "up_down_variant_fix.h"
#include "weapon_hand_blend.h"

bool g_startedAnimation = false;
BSAnimGroupSequence* g_lastLoopSequence = nullptr;

// Returns and removes the sequence queued by QueueNextAnim for the group, which plays in place of the group's next animation.
BSAnimGroupSequence* GetQueuedAnim(AnimData* animData, FullAnimGroupID animGroupId)
{
	auto* state = ActorOverrides::Peek(animData);
	if (!state || state->queued.empty())
		return nullptr;
	const auto iter = ra::find(state->queued, animGroupId, &ActorOverrides::Queued::groupId);
	if (iter == state->queued.end())
		return nullptr;
	// The controller manager still holds a reference to the sequence.
	BSAnimGroupSequence* anim = iter->sequence;
	state->queued.erase(iter);
	return anim;
}

void Apply3rdPersonRespectEndKeyEaseInFix(AnimData* animData, BSAnimGroupSequence* anim3rd);

// UInt32 animGroupId, BSAnimGroupSequence** toMorph, UInt8* basePointer
BSAnimGroupSequence* __fastcall HandleAnimationChange(AnimData* animData, void*, BSAnimGroupSequence* destAnim, UInt16 animGroupId, eAnimSequence animSequencePass)
{
	const auto baseGroupId = AnimGroup::GetBaseGroupID(animGroupId);
	const auto sequenceType = GetSequenceType(baseGroupId);
	if (!animData || !animData->actor)
		return nullptr;
	
	TESAnimGroup* const destAnimGroup = destAnim ? destAnim->animGroup : nullptr;
	BSAnimGroupSequence* currentAnim = nullptr;
	if (destAnimGroup)
	{
		if (auto* groupInfo = destAnimGroup->GetGroupInfo())
			currentAnim = animData->animSequence[groupInfo->sequenceType];
	}

	BlendFixes::ApplyMissingUpDownAnims(animData);

	if (auto* queuedAnim = GetQueuedAnim(animData, animGroupId))
	{
		destAnim = queuedAnim;
		HandleExtraOperations(animData, queuedAnim);
	}
	else if (auto* baseSet = Overrides::Find(animGroupId & 0xFF, animData); baseSet && baseSet->matchBaseGroupId)
	{
		if (auto* newAnim = Overrides::Play(*baseSet, animGroupId, animData))
			destAnim = newAnim;
	}
	else if (auto* set = Overrides::Find(animGroupId, animData))
	{
		if (auto* newAnim = Overrides::Play(*set, animGroupId, animData))
			destAnim = newAnim;
	}
	else if (destAnim)
	{
		// allow non AnimGroupOverride anims to use custom text keys
		HandleExtraOperations(animData, destAnim);
	}
	if (destAnim)
		AnimFixes::AddNoBlendSmoothingKeys(destAnim);
	
	// Handle event interception
	bool bSkip = false;
	if (auto* interceptedAnim = InterceptPlayAnimGroup::Dispatch(animData, destAnim, bSkip);
	    interceptedAnim && interceptedAnim->animGroup)
	{
		destAnim = interceptedAnim;
		animGroupId = destAnim->animGroup->groupID;
	}
	else if (bSkip)
	{
		return destAnim;
	}

	if (g_pluginSettings.blendSmoothing && g_pluginSettings.fixSpineBlendBug)
	{
		auto* aimTransition = AimTransitions::Switch(animData, destAnim);
		if (aimTransition || BlendFixes::ApplyAimBlendFix(animData, destAnim) == BlendFixes::SKIP)
		{
			// An aim transition plays instead of the requested aim, under that aim's group.
			auto* played = aimTransition ? aimTransition : destAnim;
			const UInt16 playedGroupId = aimTransition ? animGroupId : destAnim->animGroup->groupID;
			if (auto* idle = animData->animSequence[kSequence_Idle]; idle && idle->m_eState == NiControllerSequence::ANIMATING)
				BlendFixes::FixConflictingPriorities(currentAnim, played, idle);
			JIPFixes::CallSetAnimGroupEvent(animData, static_cast<AnimGroupID>(playedGroupId));
			return played;
		}
		if (!destAnim && (sequenceType == kSequence_WeaponUp || sequenceType == kSequence_WeaponDown))
		{
			// fix broken 2ha aimup/aimdown anims in vanilla (assault carbine)
			// otherwise aimISUp/aimISDown gets stuck due to our hook that patches out this logic
			const auto defaultBlend = animData->GetBlendTime();
			animData->ClearGroup(kSequence_WeaponUp, defaultBlend);
			animData->ClearGroup(kSequence_WeaponDown, defaultBlend);
		}
	}

	if (currentAnim)
		BlendFixes::FixPrematureFirstPersonEnd(animData, currentAnim);
	Apply3rdPersonRespectEndKeyEaseInFix(animData, destAnim);

	if (g_pluginSettings.blendSmoothing && destAnimGroup &&
	    destAnimGroup->GetSequenceType() == kSequence_Idle)
	{
		BlendFixes::AddMissingMTIdleInterps(animData, destAnim);
	}

	BSAnimGroupSequence* result;

	if (g_pluginSettings.blendSmoothing && destAnimGroup && destAnimGroup->GetSequenceType() == kSequence_Movement)
	{
		// JIP's handlers already ran for the group when its start animation began.
		const bool bContinuesStart = MovementTransitions::ContinuesStart(animData, animGroupId);
		result = destAnim = MovementTransitions::PlayMovementAnim(animData, destAnim, animGroupId);
		// Runs after the section's group and sequence are written, like JIP's hook in StartGroup (0x494E19).
		if (!bContinuesStart)
			JIPFixes::CallSetAnimGroupEvent(animData, static_cast<AnimGroupID>(destAnimGroup->groupID));
	}
	else
	{
		result = animData->StartGroup(destAnim, animGroupId, animSequencePass);
	}

	if (destAnim && currentAnim)
	{
		if (auto* idle = animData->animSequence[kSequence_Idle]; idle && idle->m_eState == NiControllerSequence::ANIMATING)
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
	NiTextKeyExtraData* textKeys = anim1st->m_spTextKeys;
	if (!textKeys->FindFirstByName("respectEndKey") && !textKeys->FindFirstByName("respectTextKeys"))
		return;
	if (textKeys->FindFirstByName("noBlend"))
		animData->noBlend120 = true;
	else
		// Applied before StartGroup so the 3rd person sequence blends in with the 1st person blend times.
		RespectEndKey::Apply(anim3rd, anim1st);
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
	if (!g_pluginSettings.fixLoopingReloads)
		return defaultRet();
	groupID = groupID & 0xFF; // prevent swimming animation if crouched and reloading
	auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo();
	if (!weaponInfo)
		return defaultRet();
	const auto fullGroupId = ThisStdCall<UInt16>(0x897910, animData->actor, groupID, weaponInfo, false, animData);
	if (fullGroupId == 0xFF)
		return defaultRet();
	return fullGroupId;
}

// Hook for the next round of a looping reload in Actor::HandlePostAnimationActions (AnimData::PlayAnimGroup).
// - For the player, GetCurrentAmmoRounds returns one more than usual while the next round's anim starts, because the
//   player's round is loaded by the queued equip afterwards. NPCs already have theirs loaded (npc_looping_reload_fix.cpp).
// - The engine records the reload loop action together with the current weapon sequence, then plays the next round's
//   group, chosen from the current stance. Sneaking or standing up mid-reload selects the other variant (sneak2hrreloadz
//   vs 2hrreloadz), so the recorded sequence is the one that was cut off and the action is dropped once it fades out.
//   The action is therefore re-recorded with the sequence that actually plays.
void __fastcall ReloadLoopNextRoundHook(AnimData* animData, void*, UInt16 groupId, int flags, int queuedState, eAnimSequence sequenceId)
{
	auto* actor = GET_CALLER_VAR(Actor*, -0xA0);
	const auto* recorded = animData->GetCurrentSequence(kSequence_Weapon);
	const auto isPlayer = actor == g_thePlayer;
	if (isPlayer)
		g_globals.isInLoopingReloadPlayAnim = true;
	animData->PlayGroup(groupId, flags, queuedState, sequenceId);
	if (isPlayer)
		g_globals.isInLoopingReloadPlayAnim = false;
	auto* playing = animData->GetCurrentSequence(kSequence_Weapon);
	if (actor && playing && playing != recorded)
		actor->SetAnimAction(Decoding::kAnimAction_ReloadLoop, playing);
}

// The engine chooses the looping or non-looping reload path with TESObjectWEAP::IsReloadLoop (0x4938C0), which looks at
// the base reload anim only. It plays the mod reload anim when the weapon has an Increase Clip Capacity mod
// (TESObjectWEAP::GetReloadAnimGroup 0x51E2A0). When the two disagree, the looping path asks for a group that does not
// exist, or a looping mod anim plays once as a plain reload. These hooks pass the clip mod state to IsReloadLoop.
namespace ModReloadAnimFix
{
	bool HasClipMod(const BaseProcess::WeaponInfo* weaponInfo)
	{
		return weaponInfo && weaponInfo->HasModEffect(TESObjectWEAP::kWeaponModEffect_IncreaseClipCapacity);
	}

	bool IsLoopingReload(const TESObjectWEAP* weapon, bool hasClipMod)
	{
		return weapon && weapon->HasLoopingReloadAnim(hasClipMod);
	}

	// PlayerCharacter::ReloadAlt and Actor::Reload: uses their clip mod argument.
	bool __fastcall IsReloadLoopClipModArgHook(const TESObjectWEAP* weapon)
	{
		return IsLoopingReload(weapon, GET_CALLER_VAR(bool, 0x10));
	}

	// Animation::Update, when it starts the queued reload group and picks the anim action. Written in WriteDelayedHooks
	// because Stewie's Tweaks replaces this call in its NVSEPlugin_Load (IsReloadLoopNullCheck, handled by IsLoopingReload).
	bool __fastcall IsReloadLoopQueuedReloadHook(const TESObjectWEAP* weapon)
	{
		const auto* actor = GET_CALLER_VAR(const Actor*, -0x6C);
		return IsLoopingReload(weapon, HasClipMod(actor->baseProcess->GetWeaponInfo()));
	}

	// InventoryMenu::DoClick, when reloading after the player picked ammo.
	bool __fastcall IsReloadLoopPlayerHook(const TESObjectWEAP* weapon)
	{
		return IsLoopingReload(weapon, HasClipMod(g_thePlayer->baseProcess->GetWeaponInfo()));
	}

	// Actor::HandlePostAnimationActions after a round of a looping reload; the engine passes no clip mod there.
	template <int WeaponInfoOffset>
	UInt32 __fastcall GetReloadAnimGroupHook(const TESObjectWEAP* weapon, void*, bool)
	{
		const auto* weaponInfo = GET_CALLER_VAR(const BaseProcess::WeaponInfo*, WeaponInfoOffset);
		return ThisStdCall<UInt32>(0x51E2A0, weapon, HasClipMod(weaponInfo));
	}

	bool g_enabled = false;

	void ApplyHooks()
	{
		g_enabled = true;
		WriteRelCall(0x95D400, IsReloadLoopClipModArgHook);
		WriteRelCall(0x8A8716, IsReloadLoopClipModArgHook);
		WriteRelCall(0x78074C, IsReloadLoopPlayerHook);
		WriteRelCall(0x8BAC17, GetReloadAnimGroupHook<-0x1C>); // NPC ending the loop
		WriteRelCall(0x8BACF1, GetReloadAnimGroupHook<-0x3C>); // next round
	}

	void WriteDelayedHooks()
	{
		if (g_enabled)
			WriteRelCall(0x4913C4, IsReloadLoopQueuedReloadHook);
	}
}

bool __fastcall IsCustomAnimKey(const char* key)
{
	const static auto customKeys = {
		"noBlend", "respectEndKey", "Script:", "interruptLoop", "burstFire", "respectTextKeys", "SoundPath:",
		"blendToReloadLoop", "scriptLine:", "allowAttack", "noFix", "allowClampInLocomotion", "noBlendSmoothing",
		"allowFirstPerson"
	};
	// Keys with arguments, matched the way their features read them (TextKeys::GetArguments).
	const static auto argumentKeys = { "aimPitch", "weaponSpace", "spawn", "despawn", "blendCurve", "blendInCurve", "blendOutCurve" };
	return ra::any_of(customKeys, _L(const char* key2, StartsWith(key, key2)))
		|| ra::any_of(argumentKeys, _L(const char* name, TextKeys::GetArguments(key, name).has_value()));
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
	if (g_pluginSettings.fixBlendAnimMultipliers && mult > 1.0f)
	{
		*animBlend /= mult;
	}
	// hooked call
	ThisStdCall(0xA328B0, anim, fTime, bUpdateInterpolators);
}

// Returns the override animation the actor plays next for the group, loaded. Used to answer the engine's lookups of a
// group in the Animation's map.
bool LookupCustomAnim(FullAnimGroupID groupId, AnimSequenceBase** base, AnimData* animData)
{
	if (auto* set = Overrides::Find(groupId, animData))
	{
		if (const auto loaded = Overrides::LoadNext(*set, groupId, animData))
		{
			*base = loaded.base;
			return true;
		}
	}
	if (animData->actor == g_thePlayer && animData != g_thePlayer->firstPersonAnimData && groupId != GetActorRealAnimGroup(g_thePlayer, groupId))
	{
		// hack to allow pollCondition to work correctly in first person when groupid is decayed
		Overrides::Find(groupId, g_thePlayer->firstPersonAnimData);
	}
	return false;
}

BSAnimGroupSequence* GetAnimByFullGroupID(AnimData* animData, UInt16 groupId)
{
	if (auto* set = Overrides::Find(groupId, animData))
		if (const auto loaded = Overrides::LoadNext(*set, groupId, animData))
			return loaded.sequence;
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
bool __fastcall NonExistingAnimHook(NiTPointerMap<AnimSequenceBase*>* animMap, void*, UInt16 groupId, AnimSequenceBase** base)
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
bool __fastcall OverrideWithCustomAnimHook(NiTPointerMap<AnimSequenceBase*>* animMap, void*, UInt16 groupId, AnimSequenceBase** base)
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

// Animation::GroupLoaded: returns whether the Animation has the group, either as its own or as an override. The engine
// calls it every frame for every actor (Actor::UpdateAnimation, PickAnimations, ...) and only uses the yes/no answer, so
// the Animation's own map is checked first and overrides are not loaded here.
template <int AnimDataOffset>
bool __fastcall GroupLoadedHook(NiTPointerMap<AnimSequenceBase*>* animMap, void*, UInt16 groupId, AnimSequenceBase** base)
{
	auto* parentBasePtr = GetParentBasePtr(_AddressOfReturnAddress());
	auto* animData = *reinterpret_cast<AnimData**>(parentBasePtr + AnimDataOffset);

	if ((*base = animMap->Lookup(groupId)))
		return true;
	auto* set = Overrides::Find(groupId, animData);
	return set && Overrides::GetNextPath(*set, groupId, animData);
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
	// Names of the sequences with a blendToReloadLoop text key (pooled strings). Added wherever animations start, on any
	// thread that picks them, and queried in Animation::Update.
	std::unordered_set<std::string_view> g_reloadStartBlendFixes;
	std::shared_mutex g_reloadStartBlendFixesMutex;

	void Add(std::string_view sequenceName)
	{
		{
			std::shared_lock lock(g_reloadStartBlendFixesMutex);
			if (g_reloadStartBlendFixes.contains(sequenceName))
				return;
		}
		std::unique_lock lock(g_reloadStartBlendFixesMutex);
		g_reloadStartBlendFixes.insert(AddStringToPool(sequenceName));
	}

	bool Has(std::string_view sequenceName)
	{
		std::shared_lock lock(g_reloadStartBlendFixesMutex);
		return g_reloadStartBlendFixes.contains(sequenceName);
	}

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
		if (!Has(anim->m_kName.Str()))
		{
			if (IsPlayersOtherAnimData(animData) && !g_thePlayer->IsThirdPerson())
			{
				const auto seqType = GetSequenceType(anim->animGroup->GetBaseGroupID());
				auto* cur1stPersonAnim = g_thePlayer->firstPersonAnimData->animSequence[seqType];
				if (cur1stPersonAnim && Has(cur1stPersonAnim->m_kName.Str()))
					return newCondition();
			}
			return defaultCondition();
		}
		return newCondition();
	}

	void __fastcall NoAimInReloadLoopHook(AnimData* animData, void*, eAnimSequence sequenceId, bool bChar)
	{
		auto* addrOfRetn = static_cast<UInt32*>(_AddressOfReturnAddress());
		constexpr static std::array ids = { kAnimGroup_ReloadWStart, kAnimGroup_ReloadYStart, kAnimGroup_ReloadXStart, kAnimGroup_ReloadZStart };
		auto* anim = animData->animSequence[sequenceId];

		const auto dontEndSequence = _L(, *addrOfRetn = 0x492BF8 );
		const auto endSequence = _L(, ThisStdCall(0x4994F0, animData, sequenceId, bChar));
		if (!anim || !anim->animGroup || ra::find(ids, anim->animGroup->GetBaseGroupID()) == ids.end())
		{
			endSequence();
			return;
		}
		if (Has(anim->m_kName.Str()))
		{
			dontEndSequence();
			return;
		}
		if (IsPlayersOtherAnimData(animData) && !g_thePlayer->IsThirdPerson())
		{
			const auto seqType = anim->animGroup->GetSequenceType();
			auto* cur1stPersonAnim = g_thePlayer->firstPersonAnimData->animSequence[seqType];
			if (cur1stPersonAnim && Has(cur1stPersonAnim->m_kName.Str()))
			{
				dontEndSequence();
				return;
			}
		}
		endSequence();
	}
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

	// Stewie's Tweaks bFireWhileAiming replaces the TESAnimGroup::IsAimAnim call at 0x894375 in Actor::FiresWeapon.
	// Outside VATS the player can fire while the aim sequences still blend in, except with two-handle weapons that use
	// reload G (grenade launchers). Its hook reads the caller's frame, so it cannot be called from here.
	bool FireWhileAimingTweakApplies(const TESObjectWEAP* weapon)
	{
		constexpr UInt32 isAimAnimCall = 0x894375;
		constexpr UInt32 isAimAnim = 0x4937C0; // TESAnimGroup::IsAimAnim, the same test as TESAnimGroup::IsAim
		if (*reinterpret_cast<UInt8*>(isAimAnimCall) != 0xE8 || GetRelJumpAddr(isAimAnimCall) == isAimAnim)
			return false;
		return VATSCameraData::GetSingleton()->eMode == VATSCameraData::VATS_MODE_NONE && weapon
			&& !(weapon->eWeaponType == TESObjectWEAP::kWeapType_TwoHandHandle && weapon->reloadAnim == TESObjectWEAP::eReload_G);
	}

	// Returns false when Actor::FiresWeapon (0x893A40) treats the actor as out of ammo (it clicks instead of attacking).
	// Stewie's Tweaks replaces two jumps in that check with hooks that read its frame, so their rules are repeated here
	// when those jumps are in place:
	// - bClipSizeMatters (0x893C13): outside VATS the player needs a whole shot's ammo in the clip.
	// - god mode fix (0x893C37, always on): the player fires without ammo in god mode unless demigod mode is on.
	bool HasAmmoToFire(Actor* actor, TESObjectWEAP* weapon)
	{
		const auto isJumpAt = [](UInt32 address) { return *reinterpret_cast<UInt8*>(address) == 0xE9; };
		const auto isPlayer = actor == g_thePlayer;
		const auto* ammoInfo = actor->baseProcess->GetAmmoInfo();
		bool outOfAmmo = false;
		if (weapon->ammo.IsRockItLauncher())
			outOfAmmo = !isPlayer || !g_thePlayer->GetRockItAmmoCount();
		else if (weapon->GetEquippedAmmo(actor))
			outOfAmmo = !ammoInfo || ammoInfo->count < weapon->ammoUse;
		const auto clipSizeMatters = isJumpAt(0x893C13) && isPlayer
			&& VATSCameraData::GetSingleton()->eMode == VATSCameraData::VATS_MODE_NONE;
		if (outOfAmmo && ammoInfo && ammoInfo->count && !clipSizeMatters)
			outOfAmmo = false;
		if (outOfAmmo && isJumpAt(0x893C37) && isPlayer && IsGodMode() && !IsDemigodMode())
			outOfAmmo = false;
		return !outOfAmmo;
	}

	bool CanFireWeapon(Actor* actor)
	{
		auto* process = actor->baseProcess;
		if (!process)
			return false;
		const auto isPlayer = actor == g_thePlayer;

		// PlayerCharacter::Update only reads the attack control when these checks pass (0x941E64-0x9420F7).
		if (isPlayer)
		{
			if (Interface::IsInMenuMode())
				return false;
			if (g_thePlayer->disabledControlFlags & PlayerCharacter::kControlFlag_Fight)
				return false;
			if (g_thePlayer->grabType == PlayerCharacter::kGrabType_InventoryDrop || g_thePlayer->grabType == PlayerCharacter::kGrabType_Telekinesis)
				return false;
			if (actor->actorMover && actor->actorMover->GetMovementFlags() & kMoveFlag_Swimming)
				return false;
		}
		if (actor->IsKnockedOut() || actor->IsParalyzed() || actor->lifeState != 0
			|| process->GetCurrentAnimAction() == kAnimAction_Stagger || process->GetSitSleepState() != 0)
			return false;

		// Actor::FiresWeapon checks, including the calls patched by kNVSE (allowAttack, interruptLoop) and Stewie's Tweaks.
		auto* weaponInfo = process->GetWeaponInfo();
		auto* weapon = weaponInfo ? weaponInfo->weapon : nullptr;
		if (weapon && !HasAmmoToFire(actor, weapon))
			return false;
		const auto animAction = ReloadAnimActionHook<&g_reloadAnimActionAddr1>(actor);
		if (animAction == kAnimAction_Reload || animAction == kAnimAction_ReloadLoop)
			return false;
		if (!actor->IsWeaponDrawn())
			return false;
		if (!AllowAttackHook(reinterpret_cast<Decoding::HighProcess*>(process)))
			return false;
		auto* animData = isPlayer ? g_thePlayer->GetAnimData() : process->GetAnimData();
		if (!animData)
			return false;
		auto* sequence = animData->GetCurrentSequence(kSequence_Weapon);
		if (!sequence || !sequence->animGroup)
			return false;
		auto* group = sequence->animGroup.data;
		if (group->IsAim() && !(isPlayer && FireWhileAimingTweakApplies(weapon)))
		{
			// An aim transition (AimTransitions) counts as still easing in.
			if (sequence->m_eState != NiControllerSequence::ANIMATING || AimTransitions::HoldsFire(sequence))
				return false;
			for (const auto sequenceId : { kSequence_WeaponUp, kSequence_WeaponDown })
			{
				const auto* pitchSequence = animData->GetCurrentSequence(sequenceId);
				if (pitchSequence && pitchSequence->m_eState != NiControllerSequence::ANIMATING)
					return false;
			}
			if (!isPlayer && actor->typeID == kFormType_Character)
			{
				const auto isSneakGroup = group->GetMoveType() == kAnimMoveType_Sneaking;
				if (isSneakGroup != actor->IsSneaking())
					return false;
			}
		}
		else if (group->IsAttack())
		{
			switch (group->GetGroupInfo()->actionType)
			{
			case kAnimGroupActionType_Loop:
			case kAnimGroupActionType_AttackSpin:
				break;
			case kAnimGroupActionType_Attack: // The next attack starts once the group action reaches AGA_ATTACK_STOP (3).
				if (animData->sequenceState1[kSequence_Weapon] != kSeqState_Unk3)
					return false;
				break;
			default:
				return false;
			}
		}
		return true;
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
			animData->BlendOut(sequenceId, bChar);
		}));
	}
#endif
}

thread_local PluginGlobalData g_globals;

thread_local bool g_isThreadCacheEnabled = false;

SynchronizedQueue g_mainThreadQueue;


#if _DEBUG
static HHOOK hHook = nullptr;
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;

            HWND hForeground = GetForegroundWindow();
            DWORD targetThreadId = GetWindowThreadProcessId(hForeground, NULL);
            HKL hKL = GetKeyboardLayout(targetThreadId);

            BYTE keyState[256];
            if (GetKeyboardState(keyState)) {
                keyState[VK_SHIFT] = static_cast<BYTE>(GetKeyState(VK_SHIFT));
                keyState[VK_MENU] = static_cast<BYTE>(GetKeyState(VK_MENU)); // Alt
                keyState[VK_CONTROL] = static_cast<BYTE>(GetKeyState(VK_CONTROL));
            }

            WCHAR buffer[2] = {};
            
            int result = ToUnicodeEx(
                pKey->vkCode,
                pKey->scanCode,
                keyState,
                buffer,
                2,
                0,
                hKL
            );

            if (result == -1) {
                if (buffer[0] == 0x00B0) {
                    return 1; 
                }
            }
        }
    }
    return CallNextHookEx(hHook, nCode, wParam, lParam);
}
#endif

static char* stristr(const char* haystack, const char* needle) 
{
	// case insensitive strstr
	if (!*needle) return const_cast<char*>(haystack);

	const auto needleLen = strlen(needle);
	const auto haystackLen = strlen(haystack);
    
	if (needleLen > haystackLen) return nullptr;
    
	const char* end = haystack + (haystackLen - needleLen + 1);
    
	while (haystack < end) 
	{
		if (_strnicmp(haystack, needle, needleLen) == 0)
			return const_cast<char*>(haystack);
		haystack++;
	}
	return nullptr;
}

void ApplyHooks()
{
	
#if _DEBUG
	// stop ° in console
	hHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
#endif
	Settings::Load();
	const auto& conf = g_pluginSettings;

	//WriteRelJump(0x4949D0, AnimationHook);
	
	WriteRelCall(0x494989, HandleAnimationChange);
	WriteRelCall(0x495E2A, HandleAnimationChange);
	WriteRelCall(0x4956FF, HandleAnimationChange);
	WriteRelCall(0x4973B4, HandleAnimationChange);

	WriteRelJump(0x5F444F, KeyStringCrashFixHook);
	WriteRelCall(0x941E4C, DecreaseAttackTimer);

	WriteRelCall(0x492CF6, LoopingReloadPauseFix::NoAimInReloadLoopHook);

	// WriteRelJump(0x4951D7, FixSpineBlendBug);

	if (conf.blendSmoothing && conf.fixSpineBlendBug)
		BlendFixes::ApplyAimBlendHooks();
	BlendFixes::ApplyHooks();
	AnimSwitch::WriteHooks();
	Eases::WriteHooks();
	if (conf.blendSmoothing && conf.fixBlendFromPose)
		PoseBlendFixes::ApplyHooks();
	if (conf.blendSmoothing)
	{
		MovementTransitions::WriteHooks();
		if (conf.fixSpineBlendBug)
			AimTransitions::WriteHooks();
		AimPitch::WriteHooks();
	}
	
	// These check their setting each time they run, because the menu can switch them on and off.
	WriteRelCall(0x8BABCA, LoopingReloadFixHook);
	WriteRelCall(0x948CEC, LoopingReloadFixHook); // part of cancelling a looping reload after shooting
	AccumBlendFix::WriteHooks();
	FireDirection::WriteHooks();
	WriteRelCall(0x4951D2, FixBlendMult);

	if (conf.fixModReloadAnims)
		ModReloadAnimFix::ApplyHooks();

	if (conf.fixNPCLoopingReloads)
		NPCLoopingReloadFix::ApplyHooks(conf.fixModReloadAnims);

	if (conf.fixAimUpDownVariants)
		UpDownVariantFix::WriteHooks();

	if (conf.fixIdleStrafing)
		FixIdleAnimStrafe();

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
	// 0x493DC0 (Animation::InitGroupSpeed) is hooked in Overrides::WriteHooks
	// WriteRelCall(0x493115, NonExistingAnimHook<-0x18C>);
	// WriteRelCall(0x490626, NonExistingAnimHook<-0x90>);
	// WriteRelCall(0x49022F, NonExistingAnimHook<-0x54>);
	// WriteRelCall(0x490066, NonExistingAnimHook<-0x54>);
	/* experimental end */

	// Animation::GroupLoaded
	WriteRelCall(0x49472B, GroupLoadedHook<-0x8>);
	//  AnimData::GetAnimGroupForID
	WriteRelCall(0x49651B, OverrideWithCustomAnimHook<-0x8>);
#endif
	Overrides::WriteHooks();
	
	// AnimData destructor
	WriteRelCall(0x48FB82, INLINE_HOOK(void, __fastcall, AnimData* animData)
	{
		HandleOnAnimDataDelete(animData);

		// hooked call
		ThisStdCall(0x48FF50, animData);
		// Runs after the idles are freed, so anything that still looks up an animation has its state, and before the
		// sequences leave the controller manager.
		Overrides::OnAnimDataDelete(animData);
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
	// Fixes respectEndKey not applying to a: keys. A player 3rd person sequence that takes its times from a 1st person
	// animation (RespectEndKey) also takes its a: keys from that animation.
	WriteRelCall(0x495E6C, INLINE_HOOK(NiTextKeyExtraData*, __fastcall, BSAnimGroupSequence* sequence)
	{
		if (const auto* anim1st = RespectEndKey::TimesSource(sequence))
			return anim1st->m_spTextKeys;
		return sequence->m_spTextKeys;
	}));

	// NiControllerSequence destructor
	WriteRelCall(0xA35649, INLINE_HOOK(void, __fastcall, NiControllerSequence* anim)
	{
		// hooked call
		*reinterpret_cast<UInt32*>(anim) = kVtbl_NiControllerSequence;
		auto* retnAddr = GetLambdaAddrOfRetnAddr(_AddressOfReturnAddress());
		// Sequence extra data is released when the sequence leaves its manager (SequenceExtraDatas::WriteHooks).
		// EraseTimeTrackedAnim(anim); we do store a smart pointer to the anim in g_timeTrackedAnims
		// Deactivating also clears the blend items' pointers to this sequence (kBlendInterpItem::sequence).
		if (anim->m_eState != NiControllerSequence::INACTIVE)
			anim->Deactivate(0.0f, false);
		*retnAddr = 0xA3565E;
	}));
	SafeWrite8(0xA35649 + 5, 0x90); // nop byte 6
	
#if 0
	// impossible as targets will contain deleted nodes
	static UInt32 uiNiMultiTargetTransformControllerDestroy = 0xA2FB70;
	WriteRelCall(0xA30283, INLINE_HOOK(void, __fastcall, NiMultiTargetTransformController* pController)
	{
		kBlendInterpolatorExtraData::EraseController(pController);
		ThisStdCall(uiNiMultiTargetTransformControllerDestroy, pController);
	}), &uiNiMultiTargetTransformControllerDestroy);
#endif

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
		if (const auto* anim1st = animData == g_thePlayer->baseProcess->animData ? RespectEndKey::TimesSource(anim) : nullptr)
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
			const static NiFixedString key = "fDestFrame";
			auto* destFrameKey = seq->m_spTextKeys->FindFirstByName(key);
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
		[[msvc::noinline_calls]] {
			const auto result = InterceptStopSequence::Dispatch(animData->actor, sequenceId);
			if (result && *result)
			{
				// jump to end of function
				*addressOfReturn = 0x49979C;
				return static_cast<eAnimSequence>(0);
			}
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
			AnimFixes::FixShortGaitBlend(anim, fileName);
		}
		return &anim->m_kName;
	}));

#if 1
	// BSAnimGroupSequence::SetSequenceName
	WriteRelCall(0x43B99A, INLINE_HOOK(void, __fastcall, BSAnimGroupSequence* anim, void*, NiFixedString* name)
	{
		// we want to force every animations with path names to have lowercase names so we can look them up case insensitive in NiTPointerMap
		sv::stack_string<0x400> newName("%s", name->data);
		if (newName.ends_with_ci(".kf"))
			newName.to_lower();
		anim->m_kName = newName.c_str();
	}));
#endif
	for (const auto addr : { 0x8B79EC, 0x8B7A05, 0x8B7A1E, 0x8B7A37 })
	{
		// replace case-sensitive anim name strstr's with case-insensitive
		WriteRelCall(addr, stristr);
	}

	// NiControllerManager::GetSequenceByName
	WriteRelCall(0x47A53C, INLINE_HOOK(bool, __fastcall, NiTMapBase<const char*, NiControllerSequence*>* map, void*, const char* name, NiControllerSequence** out)
	{
		sv::stack_string<0x400> newName("%s", name);
		if (newName.ends_with_ci(".kf"))
		{
			newName.to_lower();
			return ThisStdCall<bool>(0x853130, map, newName.c_str(), out);
		}
		return ThisStdCall<bool>(0x853130, map, name, out);
	}));

	WriteRelCall(0x8BAD2C, ReloadLoopNextRoundHook);


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
			if (groupId != 0xFFFF)
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
			if (groupId != 0xFFFF)
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
	
	AdditiveManager::WriteHooks();

	static UInt32 uiAnimDataUpdateControllersAddr;
	WriteRelCall(0x8885C9, INLINE_HOOK(void, __fastcall, AnimData* animData, void*, Actor* actor)
	{
		OnActorUpdateAnimation::Dispatch(actor);
		AimPitch::Update(animData, actor);
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
	}), &uiGetAttackSpeedMultGetGroupAddr);

	static UInt32 uiGetAttackSpeedMultGetAnimAttackMultAddr;
	WriteRelCall(0x645EE0, INLINE_HOOK(double, __fastcall, TESObjectWEAP* weap, void*, bool hasMod)
	{
		if (GET_CALLER_VAR_LAMBDA(Actor*, 0x8) == PlayerCharacter::GetSingleton()) {
			return ThisStdCall<double>(0x646020, weap, false);
		}

		return ThisStdCall<double>(0x646020, weap, hasMod);
	}), &uiGetAttackSpeedMultGetAnimAttackMultAddr);

	// NiControllerManager::DeactivateSequence in Animation::ClearGroup (Animation at [ebp-8], section at [ebp-4]).
	WriteRelCall(0x496208, INLINE_HOOK(bool, __fastcall, NiControllerManager* manager, void*, BSAnimGroupSequence* pkSequence, float fEaseOut)
	{
		auto* animData = GET_CALLER_VAR_LAMBDA(AnimData*, -0x8);
		const auto sequenceType = GET_CALLER_VAR_LAMBDA(eAnimSequence, -0x4);
		bool result;
		if (g_pluginSettings.blendSmoothing && g_pluginSettings.fixBlendFromPose)
			PoseBlendFixes::RecordHardCut(animData, pkSequence, fEaseOut);
		const auto animSwitch = AnimSwitch::Current(animData);
		// A variant that BlendFixes cross faded in (TransitionToVariant) can be ended by the engine while it is still
		// easing in, for example an unequip that ends during a stance switch. Deactivate would ease it out from full
		// weight and the pose would jump toward the variant's stance, so the switch eases it out from its current weight.
		if (pkSequence->m_eState == NiControllerSequence::EASEIN && fEaseOut > 0.0f
			&& ((g_pluginSettings.blendSmoothing && pkSequence->animGroup->GetSequenceType() == kSequence_Movement)
				|| BlendFixes::ContinuesVariant(*pkSequence)))
		{
			result = animSwitch.EaseOut(pkSequence, fEaseOut);
		}
		else
		{
			result = ThisStdCall<bool>(0x47B220, manager, pkSequence, fEaseOut);
			animSwitch.Add(pkSequence);
		}
		if (sequenceType == kSequence_Movement) [[msvc::noinline_calls]]
		{
			MovementBlendFixes::TrackLastEaseOut(animData);
		}
		return result;
	}));

	// Same call in Animation::ClearGroup, for the section's linked sequence.
	WriteRelCall(0x4961DE, INLINE_HOOK(bool, __fastcall, NiControllerManager* manager, void*, NiControllerSequence* pkSequence, float fEaseOut)
	{
		auto* animData = GET_CALLER_VAR_LAMBDA(AnimData*, -0x8);
		if (g_pluginSettings.blendSmoothing && g_pluginSettings.fixBlendFromPose)
			PoseBlendFixes::RecordHardCut(animData, pkSequence, fEaseOut);
		const bool result = ThisStdCall<bool>(0x47B220, manager, pkSequence, fEaseOut);
		AnimSwitch::Current(animData).Add(pkSequence);
		return result;
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

		if (!g_pluginSettings.blendSmoothing)
		{
			const bool turningLeft = (moveFlags & kMoveFlag_TurnLeft) != 0;
			const bool turningRight = (moveFlags & kMoveFlag_TurnRight) != 0;
			if (turningLeft)
			{
				*groupId = kAnimGroup_TurnLeft;
			}
			else if (turningRight)
			{
				*groupId = kAnimGroup_TurnRight;
			}
			*addrOfRetn = 0x896C9A;
			return;
		}

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

	static UInt32 uiNiRTTIDynamicCastAddr = 0x653270;
	WriteRelCall(0x49918B, INLINE_HOOK(NiTimeController*, __cdecl, NiRTTI* rtti, NiTimeController* controller)
	{
		if (g_pluginSettings.fixDeactivateControllerManagers)
		{
			if (auto* manager = NI_DYNAMIC_CAST(NiControllerManager, controller))
			{
				manager->DeactivateAll();

				// clear dangling pointers
				for (auto& sequence : manager->sequences)
				{
					if (!sequence)
						continue;
					for (auto& block: sequence->GetControlledBlocks())
					{
						if (block.m_spInterpCtlr && block.m_spInterpCtlr->GetType() == NiMultiTargetTransformController::ms_RTTI)
							block.m_pkBlendInterp = nullptr;
					}
				}
				SequenceExtraDatas::MarkStoreTargets(manager);
				return nullptr;
			}
		}
		return CdeclCall<NiTimeController*>(uiNiRTTIDynamicCastAddr, rtti, controller);
	}), &uiNiRTTIDynamicCastAddr);
	
	// prevent i.e. hunting rifle sound playing twice
	WriteRelJump(0x4EEC60, INLINE_HOOK(float, __fastcall, BSAnimGroupSequence* sequence, void*, float fTime)
	{
		if (sequence->m_eState == NiControllerSequence::TRANSDEST)
		{
			if (sequence->m_fDestFrame != -NI_INFINITY)
				return sequence->m_fDestFrame - 0.0001f;
			return 0.0f;
		}
		if (sequence->m_eState <= NiControllerSequence::INACTIVE || sequence->m_eState >= NiControllerSequence::EASEOUT)
			return 0.0f;
		return sequence->ComputeScaledTime(fTime + sequence->m_fOffset, true);
	}));
	
#if 0
	// HighProcess::SetQueuedIdleFlag
	// prevent race condition
	WriteRelJump(0x903180, INLINE_HOOK(void, __fastcall, Decoding::HighProcess* process, void*, UInt32 bFlag)
	{
		if (!process->animData || !process->animData->nSceneRoot || !process->animData->controllerManager)
			return;
		process->queuedIdleFlags |= bFlag;
	}));
#endif
	// Actor::HandlePostAnimationActions
	WriteRelCall(0x978269, INLINE_HOOK(void, __fastcall, Actor* actor)
	{
		if (!actor->Get3D())
			return;
		ThisStdCall(0x8BA600, actor);
	}));

	BlendSmoothing::WriteHooks();
	SequenceExtraDatas::WriteHooks();
	
	JIPFixes::Init();
	
	// fix extra data loading
	WriteRelCall(0x9F2ABB, INLINE_HOOK(void, __fastcall, PatrolActorPackageData* obj)
	{
		if (!obj->pActor)
			return;
		ThisStdCall<void*>(0x5D43C0, obj);
	}));
	
}

namespace
{
	// NiControllerSequence::GetState of a section's current sequence in Animation::Update (Animation at [ebp-0x18C], time
	// step at [ebp-0x5C]). The result decides whether the sequence's clock runs at the group's speed. The engine calls it
	// once per update for each section that has a group and sequence and is not held (Animation::cSkipUpdate), after the
	// step is added to the Animation's time. Features that run their own clocks do so here; otherwise it returns the
	// answer of the BlendFixes hook that this one replaces.
	NiControllerSequence::AnimState __fastcall SectionClockHook(NiControllerSequence* sequence)
	{
		auto* animData = GET_CALLER_VAR(AnimData*, -0x18C);
		const float fStep = GET_CALLER_VAR(float, -0x5C);
		if (MovementTransitions::RunClock(animData, sequence, fStep))
			return NiControllerSequence::ANIMATING;
		AimTransitions::RunClock(animData, sequence, fStep);
		return BlendFixes::ContinuedVariantState(animData, sequence);
	}
}

void WriteDelayedHooks()
{
#if _DEBUG
	SafeWriteBuf(0x5719ED, "\x83\x7D\xF8\x00\x74\x16", 5);
#endif
	NiHooks::WriteDelayedHooks();
	AllowAttackKey::ApplyHooks();
	ModReloadAnimFix::WriteDelayedHooks();
	NPCLoopingReloadFix::WriteDelayedHooks();
	BlendFixes::WriteDelayedHooks();
	RespectEndKey::WriteDelayedHooks();
	if (g_pluginSettings.blendSmoothing)
	{
		MovementTransitions::WriteDelayedHooks();
		if (g_pluginSettings.fixSpineBlendBug)
			AimTransitions::WriteDelayedHooks();
		// Animation::Update, over the hook written by BlendFixes::WriteDelayedHooks
		WriteRelCall(0x491B1B, SectionClockHook);
	}

	// The two hooks below are written late because NVAC replaces these getter calls with the getter's body once the
	// plugins have loaded (it patches every remaining call to BSAnimGroupSequence::GetCycleType and GetState). A hook
	// written at load time would be overwritten.

	// BSAnimGroupSequence::GetCycleType
	// Lets CLAMP sequences be used in locomotion (start and stop animations play once).
	WriteRelCall(0x4909BC, INLINE_HOOK(UInt32, __fastcall, BSAnimGroupSequence* anim)
	{
		const static NiFixedString key = "allowClampInLocomotion";
		if (anim->m_spTextKeys->FindFirstByName(key) ||
			(anim->animGroup && TransitionAnims::GetPathKind(anim->m_kName.Str(), anim->animGroup->groupID) != TransitionKind::None))
			return NiControllerSequence::kCycle_Loop;
		return anim->m_eCycleType;
	}));

	// NiControllerSequence::GetState of the movement sequence in Actor::PickAnimations. The result decides whether the
	// movement section is blended out because the group picked in this update ([ebp-0x40], base group) belongs to
	// another section.
	WriteRelCall(0x897712, INLINE_HOOK(NiControllerSequence::AnimState, __fastcall, BSAnimGroupSequence* anim)
	{
		// INACTIVE keeps the movement, ANIMATING ends it.
		// Equip and Unequip are picked on the update they start, and the next update picks the movement group again from
		// the move flags. Ending the movement there would restart it on every draw and holster while moving. Their legs
		// are below the movement's priority anyway.
		const auto pickedGroup = GET_CALLER_VAR_LAMBDA(UInt32, -0x40) & 0xFF;
		if (pickedGroup == kAnimGroup_Equip || pickedGroup == kAnimGroup_Unequip)
			return NiControllerSequence::INACTIVE;
		if (anim->animGroup->GetBaseGroupID() == kAnimGroup_JumpLand)
			return anim->m_fLastScaledTime >= anim->m_fEndKeyTime ? NiControllerSequence::ANIMATING : NiControllerSequence::INACTIVE;
		// A stop animation plays to its End key, where Animation::Update blends the section out.
		if (MovementTransitions::Of(anim) == TransitionKind::Stop) [[msvc::noinline_calls]]
			return NiControllerSequence::INACTIVE;
		// End movement animations immediately.
		return NiControllerSequence::ANIMATING;
	}));

	// Actor::PickAnimations picks the jump loop of an airborne actor only while its anim action is None, Attack,
	// Attack_Follow_Through or Wait_For_Lower_Body_Anim (switch at 0x89676B, indexed by action + 1 through a byte table).
	// Attack_Eject is not in that list. The update that starts with it picks the weapon's group, which blends the
	// movement section out (0x897735), and the jump loop is not picked again until the fall time has passed. Without a
	// jump animation in the movement section, Actor::UpdateRagdollAnimState runs foot IK in the air and pulls the pelvis
	// toward the ground. The patch below gives Attack_Eject the same case as Attack.
	constexpr UInt32 kJumpPickActionCases = 0x897818;
	SafeWrite8(kJumpPickActionCases + Decoding::kAnimAction_Attack_Eject + 1,
		*reinterpret_cast<UInt8*>(kJumpPickActionCases + Decoding::kAnimAction_Attack + 1));

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
	
	
#if _DEBUG
	WriteRelCall(0x86F96A, INLINE_HOOK(bool, __cdecl)
	{
		if (InterfaceManager::IsMenuInStack(Console))
			return false;
		return CdeclCall<bool>(0x709BC0);
	}));	
#endif
}
