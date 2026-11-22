#include "reload_kinds.h"

#include <utility>

#include "GameObjects.h"
#include "GameProcess.h"
#include "SafeWrite.h"
#include "game_types.h"
#include "utility.h"

namespace ReloadKinds
{
	namespace
	{
		// Records the kind of a reload. Reload types (Actor::Reload / PlayerCharacter::Reload): 0 fills the clip at once,
		// 1 reloads an empty clip (after the last shot), 2 reloads now if the clip isn't full. 1 and 2 play the reload.
		void OnReload(Actor* actor, const TESForm* ammo, UInt32 reloadType)
		{
			auto* state = ReloadState::Get(actor);
			if (!state)
				return;
			const auto* swappedAmmo = std::exchange(state->swappedAmmo, nullptr);
			if (reloadType != 1 && reloadType != 2)
				return;
			const auto* clip = actor->baseProcess->GetAmmoInfo();
			if (swappedAmmo && swappedAmmo == ammo)
				state->kind = ReloadKind::AmmoSwap;
			else if (clip && clip->ammo == ammo && clip->count > 0)
				state->kind = ReloadKind::Partial;
			else
				state->kind = ReloadKind::Full;
		}

		// PlayerCharacter::Reload and ReloadLoopingWeapon (PlayerCharacter::ReloadAlt calls one by the weapon) counting
		// the ammo they load (the equipped ammo, or the one of the weapon's hotkey slot), before anything changes the
		// clip
		template <int Site>
		struct PlayerReload
		{
			inline static UInt32 objectCount = 0x4C8F30; // InventoryChanges::GetObjectCount

			static SInt32 __fastcall Hook(void* inventoryChanges, void*, TESForm* ammo)
			{
				OnReload(g_thePlayer, ammo, GET_CALLER_VAR(UInt32, 0xC));
				return ThisStdCall<SInt32>(objectCount, inventoryChanges, ammo);
			}
		};
		using PlayerPlainReload = PlayerReload<0>;
		using PlayerLoopingReload = PlayerReload<1>;

		// Actor::Reload looking the equipped ammo up in the NPC's inventory, before anything changes the clip
		UInt32 g_actorAmmoItem = 0x4D0650; // InventoryChanges::GetInventoryItem
		void* __fastcall ActorReloadAmmoItemHook(void* inventoryChanges, void*, TESForm* ammo, UInt32 arg)
		{
			OnReload(GET_CALLER_VAR(Actor*, -0x58), ammo, GET_CALLER_VAR(UInt32, 0xC));
			return ThisStdCall<void*>(g_actorAmmoItem, inventoryChanges, ammo, arg);
		}

		// An ammo swap emptying the new clip it made, right before it calls Reload
		template <int Site>
		struct AmmoSwap
		{
			inline static UInt32 setRounds = 0x6ECD40; // BSTasklet::SetData

			static void __fastcall Hook(BaseProcess::AmmoInfo* clip, void*, SInt32 rounds)
			{
				ThisStdCall(setRounds, clip, rounds);
				if (auto* state = ReloadState::Get(g_thePlayer))
					state->swappedAmmo = clip->ammo;
			}
		};
		using SelectDefaultAmmoSwap = AmmoSwap<0>;
		using InventoryMenuSwap = AmmoSwap<1>;
	}

	ReloadKind Get(const Actor* actor)
	{
		const auto* state = ReloadState::Peek(actor);
		return state ? state->kind : ReloadKind::Full;
	}

	void WriteHooks()
	{
		WriteRelCall(0x95D992, PlayerPlainReload::Hook, &PlayerPlainReload::objectCount);
		WriteRelCall(0x95D57C, PlayerLoopingReload::Hook, &PlayerLoopingReload::objectCount);
		WriteRelCall(0x8A84D0, ActorReloadAmmoItemHook, &g_actorAmmoItem);
		// PlayerCharacter::SelectDefaultAmmo (the next ammo type the player carries)
		WriteRelCall(0x9465FF, SelectDefaultAmmoSwap::Hook, &SelectDefaultAmmoSwap::setRounds);
		// InventoryMenu::DoClick on an ammo
		WriteRelCall(0x780723, InventoryMenuSwap::Hook, &InventoryMenuSwap::setRounds);
	}
}
