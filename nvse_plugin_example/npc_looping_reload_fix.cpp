#include "npc_looping_reload_fix.h"

#include <algorithm>

#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "NiNodes.h"
#include "SafeWrite.h"
#include "game_types.h"
#include "main.h"
#include "utility.h"

namespace NPCLoopingReloadFix
{
	namespace
	{
		// The rounds the clip holds once the running reload is done: what Actor::Reload would have filled at the start
		class ReloadTarget : public PluginFormExtraData
		{
		public:
			SInt32 rounds = 0;

			static const NiFixedString& GetDataName()
			{
				static const NiFixedString name("kNVSE_NPCLoopingReload");
				return name;
			}

			const NiFixedString& GetName() const override { return GetDataName(); }

			static NiPointer<ReloadTarget> Get(const Actor* actor)
			{
				return static_cast<ReloadTarget*>(PluginFormExtraData::Get(g_nvseDataInterface, actor, GetDataName()));
			}

			static void Set(Actor* actor, SInt32 rounds)
			{
				NiPointer target = Get(actor);
				if (!target)
				{
					target = new (New<ReloadTarget>()) ReloadTarget();
					PluginFormExtraData::Add(g_nvseDataInterface, actor, target);
				}
				target->rounds = rounds;
			}

			static void Remove(Actor* actor)
			{
				PluginFormExtraData::Remove(g_nvseDataInterface, actor, GetDataName());
			}
		};

		// Actor::Reload's reload type: 0 fills the clip at once (equipping, loading), 1 reloads an empty clip (after the
		// last shot), 2 reloads now if the clip isn't full
		constexpr UInt32 kReloadType_Instant = 0;

		constexpr UInt32 kLoopStartOffset = kAnimGroup_ReloadW - kAnimGroup_ReloadWStart;

		bool g_enabled = false;
		bool g_modReloadAnimsFixed = false;

		// Whether Actor::Reload plays a looping reload for this NPC: the engine's looping path (Actor::Reload 0x8A8716,
		// with bFixModReloadAnims the reload anim that plays) with a looping anim, and the actor has its start anim.
		// Without it nothing plays and the clip is filled at once, as before.
		bool PlaysLoopingReload(Actor* actor, const TESObjectWEAP* weapon, bool hasClipMod)
		{
			if (actor == g_thePlayer || !weapon || !weapon->HasLoopingReloadAnim(hasClipMod))
				return false;
			if (!g_modReloadAnimsFixed && !ThisStdCall<bool>(0x4938C0, weapon)) // TESObjectWEAP::IsReloadLoop
				return false;
			const auto startGroup = ThisStdCall<UInt32>(0x51E2A0, weapon, hasClipMod) - kLoopStartOffset;
			const auto fullGroupId = ThisStdCall<UInt16>(0x897910, actor, startGroup, nullptr, false, nullptr); // Actor::GetAnimGroup
			return (fullGroupId & 0xFF) == startGroup;
		}

		// Actor::Reload's frame
		Actor* GetReloadingActor(void* addressOfReturnAddress) { return GetVariableOfCallerStack<Actor*>(addressOfReturnAddress, -0x58); }
		const TESObjectWEAP* GetReloadWeapon(void* addressOfReturnAddress) { return GetVariableOfCallerStack<const TESObjectWEAP*>(addressOfReturnAddress, 0x8); }
		UInt32 GetReloadType(void* addressOfReturnAddress) { return GetVariableOfCallerStack<UInt32>(addressOfReturnAddress, 0xC); }
		bool GetReloadClipMod(void* addressOfReturnAddress) { return GetVariableOfCallerStack<bool>(addressOfReturnAddress, 0x10); }

		// Actor::Reload, reload type 2: does the clip need a reload? A looping reload in progress counts as full: the AI
		// asks every update while in cover, and every call would start it over.
		UInt32 __fastcall ReloadNowClipRoundsHook(const TESObjectWEAP* weapon, void*, bool hasClipMod)
		{
			auto* retAddr = _AddressOfReturnAddress();
			auto* actor = GetReloadingActor(retAddr);
			if (actor->IsAnimActionReload() && PlaysLoopingReload(actor, weapon, GetReloadClipMod(retAddr)))
				return GET_CALLER_VAR(BaseProcess::AmmoInfo*, -0x18)->count;
			return ThisStdCall<UInt32>(0x4FE160, weapon, hasClipMod); // TESObjectWEAP::GetClipRounds
		}

		// Actor::Reload, reload type 2: the group it plays at once, ReloadX for a loop; the player plays ReloadXStart
		UInt32 __fastcall ReloadNowGroupHook(const TESObjectWEAP* weapon, void*, bool hasClipMod)
		{
			const auto group = ThisStdCall<UInt32>(0x51E2A0, weapon, hasClipMod); // TESObjectWEAP::GetReloadAnimGroup
			return PlaysLoopingReload(GetReloadingActor(_AddressOfReturnAddress()), weapon, hasClipMod) ? group - kLoopStartOffset : group;
		}

		// Actor::Reload playing the reload at once (type 2, or type 1 with an automatic weapon) records it as a plain
		// reload, which never continues to the loop
		template <int GroupOffset>
		void __fastcall ReloadNowActionHook(Actor* actor, void*, SInt32 action, BSAnimGroupSequence* sequence)
		{
			const auto group = GET_CALLER_VAR(UInt32, GroupOffset);
			if (actor != g_thePlayer && group >= kAnimGroup_ReloadWStart && group <= kAnimGroup_ReloadZStart)
				action = Decoding::kAnimAction_ReloadLoop;
			ThisStdCall(0x8A73E0, actor, action, sequence); // Actor::SetAnimAction
		}

		// Actor::Reload filling the clip (BSTasklet::SetData on the current ammo): a looping reload keeps the rounds and
		// remembers what it loads up to
		void __fastcall ClipFillHook(BaseProcess::AmmoInfo* ammo, void*, SInt32 rounds)
		{
			auto* retAddr = _AddressOfReturnAddress();
			auto* actor = GetReloadingActor(retAddr);
			if (GetReloadType(retAddr) != kReloadType_Instant && PlaysLoopingReload(actor, GetReloadWeapon(retAddr), GetReloadClipMod(retAddr)))
			{
				ReloadTarget::Set(actor, rounds);
				rounds = std::min<SInt32>(ammo->count, rounds);
			}
			ThisStdCall(0x6ECD40, ammo, rounds);
		}

		// Actor::UseAmmo after the last round reloaded: the clip is empty while a looping reload loads it, which is not
		// running out of ammo (unequipping it, CombatController::Disarmed, the follower's out of ammo line). Delayed:
		// NVAC replaces this call.
		UInt32 g_useAmmoRoundsGetter = 0x726070;
		SInt32 __fastcall UseAmmoReloadedRoundsHook(BaseProcess::AmmoInfo* ammo)
		{
			const auto rounds = ThisStdCall<SInt32>(g_useAmmoRoundsGetter, ammo);
			auto* actor = GET_CALLER_VAR(Actor*, -0x1C);
			if (rounds == 0 && actor != g_thePlayer && ReloadTarget::Get(actor))
				*static_cast<UInt32*>(_AddressOfReturnAddress()) = 0x8A8DBF;
			return rounds;
		}

		thread_local SInt32 t_ammoCarried = 0;

		// Actor::HandlePostAnimationActions after a round: the ammo the loop may load. An NPC loads up to the reload's
		// target (an NPC whose weapon uses no ammo the whole clip, as Actor::Reload fills it), dropped with the last round.
		SInt32 __fastcall RoundAmmoAvailableHook(void* inventoryChanges, void*, TESForm* ammoForm)
		{
			const auto carried = ThisStdCall<SInt32>(0x4C8F30, inventoryChanges, ammoForm); // InventoryChanges::GetObjectCount
			t_ammoCarried = carried;
			auto* actor = GET_CALLER_VAR(Actor*, -0xA0);
			if (actor == g_thePlayer)
				return carried;
			const NiPointer target = ReloadTarget::Get(actor);
			if (!target)
				return carried;
			const auto* ammo = GET_CALLER_VAR(BaseProcess::AmmoInfo*, -0x38);
			const auto clipRounds = GET_CALLER_VAR(SInt32, -0x34);
			const auto available = target->rounds;
			const SInt32 nextRounds = ammo ? static_cast<SInt32>(ammo->count) + 1 : 1;
			if (nextRounds >= available || nextRounds >= clipRounds)
				ReloadTarget::Remove(actor);
			return available;
		}

		// Actor::HandlePostAnimationActions loading a round with QueueEquipObject, which doesn't change an NPC's clip.
		// Delayed: Stewie's Tweaks replaces this call (RememberWeaponAmmos), its hook is called first.
		UInt32 g_loadRoundEquip = 0x88C650;
		void __fastcall LoadRoundHook(Actor* actor, void*, TESForm* ammoForm, SInt32 rounds, ExtraDataList* xData, UInt32 applyEnchantment, UInt32 noUnequip, UInt32 noMessage)
		{
			if (actor == g_thePlayer || rounds <= t_ammoCarried)
				ThisStdCall(g_loadRoundEquip, actor, ammoForm, rounds, xData, applyEnchantment, noUnequip, noMessage);
			auto* ammo = GET_CALLER_VAR(BaseProcess::AmmoInfo*, -0x38);
			if (actor != g_thePlayer && ammo && rounds <= GET_CALLER_VAR(SInt32, -0x34))
				ammo->count = rounds;
		}

		// Actor::HandlePostAnimationActions after the last round plays the idle in the weapon section, then for an NPC
		// the round's animation again from its start as a plain reload: a round more on screen (1.7 s with the Cowboy
		// Repeater). The NPC plays the last round's animation to its end instead, as a plain reload.
		void __fastcall LoopEndIdleHook(AnimData* animData, void*, UInt16 groupId, int flags, int loopCount, eAnimSequence sequenceId)
		{
			auto* actor = GET_CALLER_VAR(Actor*, -0xA0);
			auto* round = animData->GetCurrentSequence(kSequence_Weapon);
			if (actor != g_thePlayer && round && round->animGroup)
			{
				const auto roundGroup = round->animGroup->GetBaseGroupID();
				if (roundGroup >= kAnimGroup_ReloadW && roundGroup <= kAnimGroup_ReloadZ)
				{
					actor->SetAnimAction(Decoding::kAnimAction_Reload, round);
					*static_cast<UInt32*>(_AddressOfReturnAddress()) = 0x8BACD2; // skip the replay
					return;
				}
			}
			animData->PlayGroup(groupId, flags, loopCount, sequenceId);
		}
	}

	void ApplyHooks(bool modReloadAnimsFixed)
	{
		if (!PluginFormExtraData::IsAvailable(g_nvseDataInterface))
		{
			ERROR_LOG("bFixNPCLoopingReloads needs xNVSE 6.4.9 or newer (PluginFormExtraData), not applied");
			return;
		}
		g_enabled = true;
		g_modReloadAnimsFixed = modReloadAnimsFixed;
		WriteRelCall(0x8A85D5, ReloadNowClipRoundsHook);
		WriteRelCall(0x8A85FD, ReloadNowGroupHook);
		WriteRelCall(0x8A8650, ReloadNowActionHook<-0x30>);
		WriteRelCall(0x8A879D, ReloadNowActionHook<-0x3C>);
		WriteRelCall(0x8A86F4, ClipFillHook);
		WriteRelCall(0x8BAB0D, RoundAmmoAvailableHook);
		WriteRelCall(0x8BABFB, LoopEndIdleHook);
	}

	void WriteDelayedHooks()
	{
		if (!g_enabled)
			return;
		WriteRelCall(0x8A8C50, UseAmmoReloadedRoundsHook, &g_useAmmoRoundsGetter);
		WriteRelCall(0x8BAB52, LoadRoundHook, &g_loadRoundEquip);
	}
}
