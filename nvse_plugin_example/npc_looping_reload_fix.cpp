#include "npc_looping_reload_fix.h"

#include <algorithm>

#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "NiNodes.h"
#include "SafeWrite.h"
#include "game_types.h"
#include "main.h"
#include "reload_state.h"
#include "utility.h"

namespace NPCLoopingReloadFix
{
	namespace
	{
		// Actor::Reload's reload type: 0 fills the clip at once (equipping, loading), 1 reloads an empty clip (after
		// the last shot), 2 reloads now if the clip isn't full
		constexpr UInt32 kReloadType_Instant = 0;

		constexpr UInt32 kLoopStartOffset = kAnimGroup_ReloadW - kAnimGroup_ReloadWStart;

		bool g_enabled = false;
		bool g_modReloadAnimsFixed = false;

		// Returns true if Actor::Reload plays a looping reload for this NPC: the engine's looping path (Actor::Reload
		// 0x8A8716, with bFixModReloadAnims the reload anim that plays) with a looping anim, and the actor has its
		// start anim. Otherwise nothing plays and the clip is filled at once.
		bool PlaysLoopingReload(Actor* actor, const TESObjectWEAP* weapon, bool hasClipMod)
		{
			if (actor == PlayerCharacter::GetSingleton() || !weapon || !weapon->HasLoopingReloadAnim(hasClipMod))
				return false;
			if (!g_modReloadAnimsFixed && !ThisStdCall<bool>(0x4938C0, weapon)) // TESObjectWEAP::IsReloadLoop
				return false;
			const auto startGroup = ThisStdCall<UInt32>(0x51E2A0, weapon, hasClipMod) - kLoopStartOffset;
			const auto fullGroupId = ThisStdCall<UInt16>(0x897910, actor, startGroup, nullptr, false, nullptr); // Actor::GetAnimGroup
			return (fullGroupId & 0xFF) == startGroup;
		}

		// Actor::Reload's frame
		Actor* GetReloadingActor(void* addressOfReturnAddress) { return GetVariableOfCallerStack<Actor*>(addressOfReturnAddress, -0x58); }
		bool GetReloadClipMod(void* addressOfReturnAddress) { return GetVariableOfCallerStack<bool>(addressOfReturnAddress, 0x10); }

		// Actor::Reload, reload type 2: the group it plays at once. ReloadX for a loop (the player plays ReloadXStart)
		UInt32 __fastcall ReloadNowGroupHook(const TESObjectWEAP* weapon, void*, bool hasClipMod)
		{
			const auto group = ThisStdCall<UInt32>(0x51E2A0, weapon, hasClipMod); // TESObjectWEAP::GetReloadAnimGroup
			return PlaysLoopingReload(GetReloadingActor(_AddressOfReturnAddress()), weapon, hasClipMod) ? group - kLoopStartOffset : group;
		}

		// Actor::Reload playing the reload at once (type 2, or type 1 with an automatic weapon) records it as a plain
		// reload, which never continues to the loop. Records it as a looping reload for NPCs instead.
		template <int GroupOffset>
		void __fastcall ReloadNowActionHook(Actor* actor, void*, SInt32 action, BSAnimGroupSequence* sequence)
		{
			const auto group = GET_CALLER_VAR(UInt32, GroupOffset);
			if (actor != PlayerCharacter::GetSingleton() && group >= kAnimGroup_ReloadWStart && group <= kAnimGroup_ReloadZStart)
				action = Decoding::kAnimAction_ReloadLoop;
			ThisStdCall(0x8A73E0, actor, action, sequence); // Actor::SetAnimAction
		}

		thread_local SInt32 t_ammoCarried = 0;

		// Actor::HandlePostAnimationActions after a round: the ammo the loop may load. An NPC loads up to the reload's
		// target (the whole clip, as Actor::Reload fills it, for a weapon that uses no ammo). The target is dropped with
		// the last round.
		SInt32 __fastcall RoundAmmoAvailableHook(void* inventoryChanges, void*, TESForm* ammoForm)
		{
			const auto carried = ThisStdCall<SInt32>(0x4C8F30, inventoryChanges, ammoForm); // InventoryChanges::GetObjectCount
			t_ammoCarried = carried;
			auto* actor = GET_CALLER_VAR(Actor*, -0xA0);
			if (actor == PlayerCharacter::GetSingleton())
				return carried;
			auto* state = ReloadState::Peek(actor);
			if (!state || !state->loopTarget)
				return carried;
			const auto* ammo = GET_CALLER_VAR(BaseProcess::AmmoInfo*, -0x38);
			const auto clipRounds = GET_CALLER_VAR(SInt32, -0x34);
			const auto available = state->loopTarget;
			// The rounds after this round. Rounds that ammo keys loaded during it take the place of its load (AmmoKeys
			// makes the engine's count getter, called next, return one less).
			const SInt32 nextRounds = (ammo ? static_cast<SInt32>(ammo->count) : 0) + (state->keyLoads > 0 ? 0 : 1);
			if (nextRounds >= available || nextRounds >= clipRounds)
				state->loopTarget = 0;
			return available;
		}

		// Actor::HandlePostAnimationActions loading a round with QueueEquipObject, which doesn't change an NPC's clip.
		// Delayed because Stewie's Tweaks replaces this call (RememberWeaponAmmos) and its hook must be called first.
		UInt32 g_loadRoundEquip = 0x88C650;
		void __fastcall LoadRoundHook(Actor* actor, void*, TESForm* ammoForm, SInt32 rounds, ExtraDataList* xData, UInt32 applyEnchantment, UInt32 noUnequip, UInt32 noMessage)
		{
			if (actor == PlayerCharacter::GetSingleton() || rounds <= t_ammoCarried)
				ThisStdCall(g_loadRoundEquip, actor, ammoForm, rounds, xData, applyEnchantment, noUnequip, noMessage);
			auto* ammo = GET_CALLER_VAR(BaseProcess::AmmoInfo*, -0x38);
			if (actor != PlayerCharacter::GetSingleton() && ammo && rounds <= GET_CALLER_VAR(SInt32, -0x34))
				ammo->count = rounds;
		}

		// Actor::HandlePostAnimationActions after the last round plays the idle in the weapon section. For an NPC it
		// would then play the round's animation again from its start as a plain reload, showing one round more. This
		// plays the last round's animation to its end instead, as a plain reload.
		void __fastcall LoopEndIdleHook(AnimData* animData, void*, UInt16 groupId, int flags, int loopCount, eAnimSequence sequenceId)
		{
			auto* actor = GET_CALLER_VAR(Actor*, -0xA0);
			auto* round = animData->GetCurrentSequence(kSequence_Weapon);
			if (actor != PlayerCharacter::GetSingleton() && round && round->animGroup)
			{
				const auto roundGroup = round->animGroup->GetBaseGroupID();
				if (roundGroup >= kAnimGroup_ReloadW && roundGroup <= kAnimGroup_ReloadZ)
				{
					actor->SetAnimAction(Decoding::kAnimAction_Reload, round);
					*static_cast<UInt32*>(_AddressOfReturnAddress()) = 0x8BACD2; // Skips the replay
					return;
				}
			}
			animData->PlayGroup(groupId, flags, loopCount, sequenceId);
		}
	}

	SInt32 OnClipFill(Actor* actor, const TESObjectWEAP* weapon, UInt32 reloadType, bool hasClipMod, const BaseProcess::AmmoInfo& clip, SInt32 rounds)
	{
		if (!g_enabled || reloadType == kReloadType_Instant || !PlaysLoopingReload(actor, weapon, hasClipMod))
			return rounds;
		auto* state = ReloadState::Get(actor);
		if (!state)
			return rounds;
		state->loopTarget = rounds;
		return (std::min)(static_cast<SInt32>(clip.count), rounds);
	}

	bool PlaysLoop(Actor* actor, const TESObjectWEAP* weapon, bool hasClipMod)
	{
		return g_enabled && PlaysLoopingReload(actor, weapon, hasClipMod);
	}

	void ApplyHooks(bool modReloadAnimsFixed)
	{
		g_enabled = true;
		g_modReloadAnimsFixed = modReloadAnimsFixed;
		// 0x8A85D5 (Actor::Reload type 2: does the clip need a reload?) and the clip fill 0x8A86F4 are AmmoKeys' hooks,
		// which ask PlaysLoop and OnClipFill
		WriteRelCall(0x8A85FD, ReloadNowGroupHook);
		WriteRelCall(0x8A8650, ReloadNowActionHook<-0x30>);
		WriteRelCall(0x8A879D, ReloadNowActionHook<-0x3C>);
		WriteRelCall(0x8BAB0D, RoundAmmoAvailableHook);
		WriteRelCall(0x8BABFB, LoopEndIdleHook);
	}

	void WriteDelayedHooks()
	{
		if (!g_enabled)
			return;
		WriteRelCall(0x8BAB52, LoadRoundHook, &g_loadRoundEquip);
	}
}
