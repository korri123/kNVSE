#include "ammo_keys.h"

#include <algorithm>
#include <charconv>
#include <optional>

#include "GameExtraData.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "SafeWrite.h"
#include "commands_animation.h"
#include "game_types.h"
#include "hooks.h"
#include "main.h"
#include "npc_looping_reload_fix.h"
#include "reload_state.h"
#include "text_key_arguments.h"
#include "text_keys.h"
#include "utility.h"

namespace AmmoKeys
{
	namespace
	{
		std::optional<Key> ParseKey(const char* text, const NiControllerSequence& sequence)
		{
			for (const bool load : { true, false })
			{
				const auto arguments = TextKeys::GetArguments(text, load ? "loadAmmo" : "unloadAmmo");
				if (!arguments)
					continue;
				Key key{ .load = load };
				if (arguments->empty())
					return key;
				const auto* end = arguments->data() + arguments->size();
				if (const auto [ptr, ec] = std::from_chars(arguments->data(), end, key.rounds); ec != std::errc() || ptr != end || key.rounds <= 0)
				{
					ERROR_LOG(FormatString("%s: \"%s\" takes a number of rounds above 0, ignored", sequence.m_kName.CStr(), text));
					return std::nullopt;
				}
				return key;
			}
			return std::nullopt;
		}

		BaseProcess::AmmoInfo* GetClip(const Actor* actor)
		{
			auto* process = actor->baseProcess;
			auto* ammoInfo = process ? process->GetAmmoInfo() : nullptr;
			return ammoInfo && ammoInfo->ammo ? ammoInfo : nullptr;
		}

		const TESObjectWEAP* GetWeapon(const Actor* actor)
		{
			const auto* weaponInfo = actor->baseProcess ? actor->baseProcess->GetWeaponInfo() : nullptr;
			return weaponInfo ? weaponInfo->weapon : nullptr;
		}

		// The rounds the clip can hold now, as Actor::Reload fills it: its size, or the ammo carried if that is less and
		// the actor's weapon uses ammo (Actor::ShouldWeaponUseAmmo: not for NPCs with infinite ammo)
		SInt32 GetCapacity(Actor* actor, const BaseProcess::AmmoInfo& clip)
		{
			const auto* weaponInfo = actor->baseProcess->GetWeaponInfo();
			if (!weaponInfo || !weaponInfo->weapon)
				return 0;
			auto* weapon = weaponInfo->weapon;
			const bool hasClipMod = weaponInfo->HasModEffect(TESObjectWEAP::kWeaponModEffect_IncreaseClipCapacity);
			const auto clipRounds = ThisStdCall<SInt32>(0x4FE160, weapon, hasClipMod); // TESObjectWEAP::GetClipRounds
			const bool usesAmmo = ThisStdCall<bool>(0x8A8DD0, actor, weapon); // Actor::ShouldWeaponUseAmmo
			const bool thrown = ThisStdCall<bool>(0x4C0BF0, weapon); // TESObjectWEAP::IsThrownWeapon
			if (!usesAmmo && !thrown)
				return clipRounds;
			auto* inventory = CdeclCall<void*>(0x4BF220, actor); // InventoryChanges::GetInventoryChanges
			const auto carried = inventory ? ThisStdCall<SInt32>(0x4C8F30, inventory, clip.ammo) : 0; // InventoryChanges::GetObjectCount
			return (std::min)(clipRounds, carried);
		}

		void SetRounds(const Actor* actor, BaseProcess::AmmoInfo& clip, SInt32 rounds)
		{
			clip.count = rounds;
			// The weapon's ExtraAmmo, where it has one for this ammo (Stewie's Tweaks' bRememberWeaponAmmos keeps the clip
			// there for when the weapon is equipped again), follows the clip.
			const auto* weaponInfo = actor->baseProcess->GetWeaponInfo();
			auto* extraData = weaponInfo ? weaponInfo->GetExtraData() : nullptr;
			if (auto* extraAmmo = extraData ? static_cast<ExtraAmmo*>(extraData->GetByType(kExtraData_Ammo)) : nullptr; extraAmmo && extraAmmo->ammo == clip.ammo)
				extraAmmo->unk4 = rounds;
		}

		bool IsCurrent(const Actor* actor, const ReloadState::Fill& fill)
		{
			return fill.valid && GetWeapon(actor) == fill.weapon
				&& (fill.started || GetTickCount64() - fill.tick < ReloadState::Fill::kFillLifetimeMs);
		}

		// The NPC is reloading with its clip held: the reload KF loads it by key
		bool IsHolding(const Actor* actor, const ReloadState& state)
		{
			return IsCurrent(actor, state.fill) && state.fill.held;
		}

		bool Loads(const BSAnimGroupSequence* sequence)
		{
			const auto* keys = sequence ? TextKeys::Find(*sequence) : nullptr;
			return keys && std::ranges::any_of(keys->ammoKeys.items, [](const auto& item) { return item.first.load; });
		}

		// The skeleton whose keys act for the actor: the player's view, an NPC's own
		AnimData* GetKeyAnimData(Actor* actor)
		{
			auto* player = PlayerCharacter::GetSingleton();
			if (actor == player)
				return player->GetAnimData();
			return actor->baseProcess ? actor->baseProcess->GetAnimData() : nullptr;
		}

		// The reload sequence the reload plays: the one started already (a reload of a clip that isn't empty plays
		// before the fill), else the one the group plays next (the reload after the last shot starts when the attack
		// ends). Sets started if it has started.
		BSAnimGroupSequence* GetReloadSequence(Actor* actor, const TESObjectWEAP* weapon, bool hasClipMod, bool& started)
		{
			auto* animData = GetKeyAnimData(actor);
			if (!animData)
				return nullptr;
			const auto group = static_cast<AnimGroupID>(ThisStdCall<UInt32>(0x51E2A0, weapon, hasClipMod) & 0xFF); // TESObjectWEAP::GetReloadAnimGroup
			auto* current = animData->animSequence[kSequence_Weapon];
			if (current && current->animGroup && current->animGroup->GetBaseGroupID() == group
				&& current->m_eState != NiControllerSequence::INACTIVE && current->m_eState != NiControllerSequence::EASEOUT)
			{
				started = true;
				return current;
			}
			return GetAnimByGroupID(animData, group);
		}

		// Records the fill of a reload that plays an animation. Returns the state if the fill is held.
		ReloadState* StartFill(Actor* actor, TESForm* ammo, const TESObjectWEAP* weapon, bool hasClipMod, SInt32 before, SInt32 filled, bool holdable)
		{
			auto* state = ReloadState::Get(actor);
			if (!state)
				return nullptr;
			bool started = false;
			const auto* sequence = GetReloadSequence(actor, weapon, hasClipMod, started);
			state->fill = ReloadState::Fill{
				.valid = true,
				.ammo = ammo,
				.weapon = weapon,
				.before = before,
				.filled = filled,
				.held = holdable && Loads(sequence),
				.holdable = holdable,
				.started = started,
				.tick = GetTickCount64(),
			};
			return state->fill.held ? state : nullptr;
		}

		// PlayerCharacter::Reload queueing the equip of the ammo that fills the clip
		UInt32 g_reloadEquip = 0x88C650; // Actor::QueueEquipObject
		void __fastcall PlayerReloadFillHook(PlayerCharacter* player, void*, TESForm* ammo, SInt32 rounds, ExtraDataList* extraData,
			UInt32 applyEnchantment, UInt32 noUnequip, UInt32 noMessage)
		{
			const auto* weapon = GET_CALLER_VAR(const TESObjectWEAP*, 0x8);
			const auto reloadType = GET_CALLER_VAR(UInt32, 0xC);
			const bool hasClipMod = GET_CALLER_VAR(bool, 0x10);
			// Types 1 (empty clip) and 2 (reload key) play the reload animation; 0 fills the clip at once
			if (weapon && (reloadType == 1 || reloadType == 2))
			{
				const auto* clip = GetClip(player);
				StartFill(player, ammo, weapon, hasClipMod, clip && clip->ammo == ammo ? static_cast<SInt32>(clip->count) : 0, -1, true);
			}
			ThisStdCall(g_reloadEquip, player, ammo, rounds, extraData, applyEnchantment, noUnequip, noMessage);
		}

		// Actor::EquipObject making the clip of the equipped ammo (the player's queued equip, or at once in menus)
		UInt32 g_clipConstructor = 0x4BC550; // ItemChange::ItemChange
		BaseProcess::AmmoInfo* __fastcall ClipEquipHook(BaseProcess::AmmoInfo* clip, void*, TESForm* ammo, SInt32 rounds)
		{
			clip = ThisStdCall<BaseProcess::AmmoInfo*>(g_clipConstructor, clip, ammo, rounds);
			auto* player = PlayerCharacter::GetSingleton();
			if (GET_CALLER_VAR(Actor*, -0x80) != player || !clip)
				return clip;
			auto* state = ReloadState::Peek(player);
			if (!state || !IsCurrent(player, state->fill) || state->fill.filled >= 0 || ammo != state->fill.ammo)
				return clip;
			state->fill.filled = clip->count;
			if (state->fill.held)
				clip->count = (std::min)(state->fill.before, static_cast<SInt32>(clip->count));
			return clip;
		}

		// Actor::Reload's frame (NPCs: the player reloads in PlayerCharacter::Reload)
		Actor* GetReloadingActor(void* addressOfReturnAddress) { return GetVariableOfCallerStack<Actor*>(addressOfReturnAddress, -0x58); }

		// Actor::Reload filling an NPC's clip (BSTasklet::SetData on the current ammo), before the reload plays (type 2)
		// or as it is queued (type 1). A looping reload keeps the rounds (NPCLoopingReloadFix); so does a reload whose KF
		// loads them by key.
		void __fastcall ActorReloadFillHook(BaseProcess::AmmoInfo* clip, void*, SInt32 rounds)
		{
			auto* retAddr = _AddressOfReturnAddress();
			auto* actor = GetReloadingActor(retAddr);
			const auto* weapon = GetVariableOfCallerStack<const TESObjectWEAP*>(retAddr, 0x8);
			const auto reloadType = GetVariableOfCallerStack<UInt32>(retAddr, 0xC);
			const bool hasClipMod = GetVariableOfCallerStack<bool>(retAddr, 0x10);
			const SInt32 before = clip->count;
			const SInt32 filled = rounds;
			ThisStdCall(0x6ECD40, clip, NPCLoopingReloadFix::OnClipFill(actor, weapon, reloadType, hasClipMod, *clip, rounds)); // BSTasklet::SetData
			if (actor == PlayerCharacter::GetSingleton() || !weapon || reloadType == 0)
				return;
			const bool holdable = !weapon->HasLoopingReloadAnim(hasClipMod);
			if (StartFill(actor, clip->ammo, weapon, hasClipMod, before, filled, holdable))
				SetRounds(actor, *clip, (std::min)(before, filled));
		}

		// Actor::Reload, reload type 2: does the clip need a reload? A reload in progress whose rounds come later
		// (an NPC's looping reload, a held fill) counts as full, because the AI asks every update while in cover and
		// every call would start it over.
		UInt32 __fastcall ReloadNowClipRoundsHook(const TESObjectWEAP* weapon, void*, bool hasClipMod)
		{
			auto* retAddr = _AddressOfReturnAddress();
			auto* actor = GetReloadingActor(retAddr);
			if (actor->IsAnimActionReload())
			{
				const auto* state = ReloadState::Peek(actor);
				if (NPCLoopingReloadFix::PlaysLoop(actor, weapon, GetVariableOfCallerStack<bool>(retAddr, 0x10))
					|| (state && state->fill.started && IsHolding(actor, *state)))
					return GET_CALLER_VAR(BaseProcess::AmmoInfo*, -0x18)->count;
			}
			return ThisStdCall<UInt32>(0x4FE160, weapon, hasClipMod); // TESObjectWEAP::GetClipRounds
		}

		// Actor::UseAmmo after the last round reloaded. An NPC's clip is empty while a looping reload or a held fill
		// loads it, which is not running out of ammo (unequipping the weapon, CombatController::Disarmed, the follower's
		// out of ammo line). Delayed because NVAC replaces this call.
		UInt32 g_useAmmoRoundsGetter = 0x726070;
		SInt32 __fastcall UseAmmoReloadedRoundsHook(BaseProcess::AmmoInfo* ammo)
		{
			const auto rounds = ThisStdCall<SInt32>(g_useAmmoRoundsGetter, ammo);
			auto* actor = GET_CALLER_VAR(Actor*, -0x1C);
			if (rounds == 0 && actor != PlayerCharacter::GetSingleton())
			{
				if (const auto* state = ReloadState::Peek(actor); state && (state->loopTarget > 0 || IsHolding(actor, *state)))
					*static_cast<UInt32*>(_AddressOfReturnAddress()) = 0x8A8DBF;
			}
			return rounds;
		}

		// The loadAmmo keys of the round the player holds that haven't acted yet: attacking ended the loop, the round goes
		// in. Loads what they would have (the sum of their rounds; all if one fills the clip).
		void LoadPendingRound(Actor* actor, const BSAnimGroupSequence* round)
		{
			Key pending{ .load = true };
			bool any = false;
			{
				std::unique_lock lock(g_animTimeMutex);
				const auto iter = g_timeTrackedAnims.find(const_cast<BSAnimGroupSequence*>(round));
				if (iter == g_timeTrackedAnims.end() || !iter->second->ammoKeysAct || !iter->second->ammoKeys.Exists())
					return;
				const auto& context = iter->second->ammoKeys;
				const auto& items = context.execution->items;
				for (size_t i = context.index; i < items.size(); ++i)
				{
					const Key& key = items[i].first;
					if (!key.load)
						continue;
					if (!key.rounds || (any && !pending.rounds))
						pending.rounds = 0;
					else
						pending.rounds += key.rounds;
					any = true;
				}
			}
			if (any)
				Fire(actor, pending, nullptr);
		}

		// The engine loading a round of a looping reload: Actor::HandlePostAnimationActions when a round ends (the next
		// round starts), PlayerCharacter::CheckUserInputAttacks when attacking ends the player's loop (the round in the
		// hand goes in). Both read the rounds in the clip and add one. One less if keys loaded rounds since the round
		// started, so it loads nothing more and the loop goes on or ends by the clip as it is.
		template <int ActorOffset, bool EndsLoop>
		struct RoundLoad
		{
			inline static UInt32 roundsGetter = 0x726070;

			static SInt32 __fastcall Hook(BaseProcess::AmmoInfo* clip)
			{
				auto* actor = GET_CALLER_VAR(Actor*, ActorOffset);
				auto* state = EndsLoop ? ReloadState::Get(actor) : ReloadState::Peek(actor);
				if (EndsLoop && state)
				{
					auto* animData = GetKeyAnimData(actor);
					const auto* round = animData ? animData->animSequence[kSequence_Weapon] : nullptr;
					if (round && state->keyLoads == 0)
						LoadPendingRound(actor, round);
					state->loopEndedRound = round;
				}
				const auto rounds = ThisStdCall<SInt32>(roundsGetter, clip);
				const auto keyLoads = state ? std::exchange(state->keyLoads, 0) : 0;
				const bool byKeys = keyLoads > 0 && rounds > 0;
				if (state)
					state->roundLoadedByKeys = byKeys;
				return byKeys ? rounds - 1 : rounds;
			}
		};
		using NextRoundLoad = RoundLoad<-0xA0, false>;
		using AttackRoundLoad = RoundLoad<-0x9C, true>;

		// HUDMainMenu::UpdateAmmoLabel reading the rounds in the player's clip. Prevent Reload Cancelling (Stewie's
		// Tweaks with PreventReloadCancelling.dll, or the DLL alone) replaces this call: it shows the rounds from before
		// a reload until 70 % of it. With a held fill the keys decide when the clip changes, so the counter shows the
		// clip as it is: an unloadAmmo key earlier than 70 % showed only at 70 %, for the frames before a loadAmmo key
		// after it (a flicker).
		UInt32 g_hudRoundsGetter = 0x726070;
		SInt32 __fastcall HUDRoundsHook(BaseProcess::AmmoInfo* clip)
		{
			auto* player = PlayerCharacter::GetSingleton();
			if (const auto* state = ReloadState::Peek(player); state && IsHolding(player, *state))
				return static_cast<SInt32>(clip->count);
			return ThisStdCall<SInt32>(g_hudRoundsGetter, clip);
		}
	}

	TimedExecution<Key> Parse(const NiControllerSequence& sequence)
	{
		return TimedExecution<Key>(sequence.m_spTextKeys->GetKeys(), [&](const char* text, Key& key)
		{
			const auto parsedKey = ParseKey(text, sequence);
			if (parsedKey)
				key = *parsedKey;
			return parsedKey.has_value();
		});
	}

	bool OnSequenceStart(AnimData* animData, BSAnimGroupSequence* sequence)
	{
		auto* actor = animData->actor;
		if (!actor || !sequence->animGroup || (actor == PlayerCharacter::GetSingleton() && IsPlayersOtherAnimData(animData)))
			return false;
		if (GetSequenceType(sequence->animGroup->GetBaseGroupID()) != kSequence_Weapon)
			return true;
		auto* state = ReloadState::Peek(actor);
		if (!state)
			return true;
		state->keyLoads = 0;
		if (sequence == state->loopEndedRound)
			state->loopEndedRound = nullptr;
		auto& fill = state->fill;
		if (!IsCurrent(actor, fill))
			return true;
		const bool isReload = sequence->animGroup->IsReload();
		if (fill.started)
		{
			// The reload has ended
			if (!isReload)
				fill.valid = false;
			return true;
		}
		if (!isReload)
			return true;
		// The reload of the last fill starts (a reload after the last shot waits for the attack to end). The fill was
		// held by the sequence the group was going to play; this one decides.
		fill.started = true;
		const bool held = fill.holdable && Loads(sequence);
		if (held == fill.held)
			return true;
		fill.held = held;
		if (fill.filled < 0)
			return true;
		if (auto* clip = GetClip(actor); clip && clip->ammo == fill.ammo)
			SetRounds(actor, *clip, held ? (std::min)(fill.before, fill.filled) : fill.filled);
		return true;
	}

	void Fire(Actor* actor, const Key& key, const BSAnimGroupSequence* sequence)
	{
		auto* clip = GetClip(actor);
		auto* state = clip ? ReloadState::Get(actor) : nullptr;
		if (!state || (key.load && sequence && sequence == state->loopEndedRound))
			return;
		auto& fill = state->fill;
		// Keys reached before the player's queued equip has filled the clip change what it fills it with
		const bool beforeFill = IsCurrent(actor, fill) && fill.held && fill.filled < 0 && fill.ammo == clip->ammo;
		const SInt32 current = beforeFill ? fill.before : static_cast<SInt32>(clip->count);
		SInt32 rounds;
		if (key.load)
		{
			const auto capacity = GetCapacity(actor, *clip);
			rounds = (std::max)(current, key.rounds ? (std::min)(capacity, current + key.rounds) : capacity);
			state->keyLoads += rounds - current;
		}
		else
			rounds = key.rounds ? (std::max<SInt32>)(0, current - key.rounds) : 0;
		if (beforeFill)
			fill.before = rounds;
		SetRounds(actor, *clip, rounds);
	}

	void Reset()
	{
		auto* player = PlayerCharacter::GetSingleton();
		if (auto* state = player ? ReloadState::Peek(player) : nullptr)
			*state = ReloadState{ .animData = state->animData };
	}

	bool EngineLoadsRound(const Actor* actor)
	{
		const auto* state = ReloadState::Peek(actor);
		return !state || !state->roundLoadedByKeys;
	}

	void WriteDelayedHooks()
	{
		// Stewie's Tweaks detours the first (Prevent Reload Cancelling, bRememberWeaponAmmos) and replaces the second
		// (bRememberWeaponAmmos) in its NVSEPlugin_Load; NVAC inlines the getters of the last three. These go over them
		// and call them.
		WriteRelCall(0x95DCAC, PlayerReloadFillHook, &g_reloadEquip);
		WriteRelCall(0x88CEDA, ClipEquipHook, &g_clipConstructor);
		WriteRelCall(0x8A85D5, ReloadNowClipRoundsHook);
		WriteRelCall(0x8A86F4, ActorReloadFillHook);
		WriteRelCall(0x8BAB1E, NextRoundLoad::Hook, &NextRoundLoad::roundsGetter);
		WriteRelCall(0x948C52, AttackRoundLoad::Hook, &AttackRoundLoad::roundsGetter);
		WriteRelCall(0x8A8C50, UseAmmoReloadedRoundsHook, &g_useAmmoRoundsGetter);
	}

	void WriteLateHooks()
	{
		// Only where something replaced the getter call (Prevent Reload Cancelling, written from Stewie's Tweaks'
		// deferred init in the main loop). NVAC inlines the getter at this site otherwise.
		constexpr UInt32 kHudRoundsCall = 0x77244F;
		if (*reinterpret_cast<UInt8*>(kHudRoundsCall) != 0xE8 || GetRelJumpAddr(kHudRoundsCall) == 0x726070)
			return;
		WriteRelCall(kHudRoundsCall, HUDRoundsHook, &g_hudRoundsGetter);
	}
}
