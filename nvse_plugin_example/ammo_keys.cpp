#include "ammo_keys.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <unordered_map>

#include "GameExtraData.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "SafeWrite.h"
#include "commands_animation.h"
#include "game_types.h"
#include "hooks.h"
#include "main.h"
#include "text_key_arguments.h"
#include "utility.h"

namespace AmmoKeys
{
	namespace
	{
		struct ParsedKeys
		{
			TimedExecution<Key> keys;
			bool loads = false;
		};

		struct FixedStringHash
		{
			size_t operator()(const NiFixedString& string) const { return std::hash<const char*>()(string.CStr()); }
		};

		// By KF path, which every sequence made from the KF has as its name. Nodes are never erased, so pointers to the
		// values stay valid.
		std::mutex g_parsedMutex;
		std::unordered_map<NiFixedString, std::optional<ParsedKeys>, FixedStringHash> g_parsed;

		std::optional<Key> ParseKey(const char* text, const BSAnimGroupSequence& sequence)
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

		BaseProcess::AmmoInfo* GetClip()
		{
			auto* process = g_thePlayer->baseProcess;
			auto* ammoInfo = process ? process->GetAmmoInfo() : nullptr;
			return ammoInfo && ammoInfo->ammo ? ammoInfo : nullptr;
		}

		// The rounds the clip can hold now: its size, or the ammo carried if that is less
		SInt32 GetCapacity(const BaseProcess::AmmoInfo& clip)
		{
			const auto* weaponInfo = g_thePlayer->baseProcess->GetWeaponInfo();
			if (!weaponInfo || !weaponInfo->weapon)
				return 0;
			const bool hasClipMod = weaponInfo->HasModEffect(TESObjectWEAP::kWeaponModEffect_IncreaseClipCapacity);
			const auto clipRounds = ThisStdCall<SInt32>(0x4FE160, weaponInfo->weapon, hasClipMod); // TESObjectWEAP::GetClipRounds
			auto* inventory = CdeclCall<void*>(0x4BF220, g_thePlayer); // InventoryChanges::GetInventoryChanges
			const auto carried = inventory ? ThisStdCall<SInt32>(0x4C8F30, inventory, clip.ammo) : 0; // InventoryChanges::GetObjectCount
			return (std::min)(clipRounds, carried);
		}

		void SetRounds(BaseProcess::AmmoInfo& clip, SInt32 rounds)
		{
			clip.count = rounds;
			// The weapon's ExtraAmmo, where it has one for this ammo (Stewie's Tweaks' bRememberWeaponAmmos keeps the clip
			// there for when the weapon is equipped again), follows the clip.
			const auto* weaponInfo = g_thePlayer->baseProcess->GetWeaponInfo();
			auto* extraData = weaponInfo ? weaponInfo->GetExtraData() : nullptr;
			if (auto* extraAmmo = extraData ? static_cast<ExtraAmmo*>(extraData->GetByType(kExtraData_Ammo)) : nullptr; extraAmmo && extraAmmo->ammo == clip.ammo)
				extraAmmo->unk4 = rounds;
		}

		// The clip fill of the reload that PlayerCharacter::Reload started last
		struct Fill
		{
			bool valid = false;
			TESForm* ammo = nullptr;
			const TESObjectWEAP* weapon = nullptr;
			// The rounds of this ammo in the clip before the reload (0 when it changes the ammo type)
			SInt32 before = 0;
			// The rounds the engine filled the clip with, once its queued equip has done so; -1 before
			SInt32 filled = -1;
			// The clip keeps its rounds instead of the fill
			bool held = false;
			// A reload sequence has started for it
			bool started = false;
			ULONGLONG tick = 0;
		};
		Fill g_fill;

		// Rounds loaded by keys since the player's weapon section started its sequence or the engine last loaded a round
		// of a looping reload
		SInt32 g_keyLoads = 0;
		// Attacking ended a looping reload, which loaded the round in the hand: the rest of this round's animation (on the
		// skeleton of the view) plays without loading more, until it starts again
		const BSAnimGroupSequence* g_loopEndedRound = nullptr;

		constexpr ULONGLONG kFillLifetimeMs = 5000;

		bool IsCurrent(const Fill& fill)
		{
			const auto* weaponInfo = g_thePlayer->baseProcess ? g_thePlayer->baseProcess->GetWeaponInfo() : nullptr;
			return fill.valid && GetTickCount64() - fill.tick < kFillLifetimeMs && weaponInfo && weaponInfo->weapon == fill.weapon;
		}

		bool Loads(const BSAnimGroupSequence* sequence)
		{
			if (!sequence)
				return false;
			Find(*sequence);
			std::unique_lock lock(g_parsedMutex);
			const auto iter = g_parsed.find(sequence->m_kName);
			return iter != g_parsed.end() && iter->second && iter->second->loads;
		}

		// The reload sequence the reload plays in the player's view: the one started already (a reload of a clip that
		// isn't empty plays before the fill), else the one the group plays next (the reload after the last shot starts
		// when the attack ends). Sets started if it has started.
		BSAnimGroupSequence* GetReloadSequence(const TESObjectWEAP* weapon, bool hasClipMod, bool& started)
		{
			auto* animData = g_thePlayer->GetAnimData();
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

		// PlayerCharacter::Reload queueing the equip of the ammo that fills the clip
		UInt32 g_reloadEquip = 0x88C650; // Actor::QueueEquipObject
		void __fastcall ReloadFillHook(PlayerCharacter* player, void*, TESForm* ammo, SInt32 rounds, ExtraDataList* extraData,
			UInt32 applyEnchantment, UInt32 noUnequip, UInt32 noMessage)
		{
			const auto* weapon = GET_CALLER_VAR(const TESObjectWEAP*, 0x8);
			const auto reloadType = GET_CALLER_VAR(UInt32, 0xC);
			const bool hasClipMod = GET_CALLER_VAR(bool, 0x10);
			// Types 1 (empty clip) and 2 (reload key) play the reload animation; 0 fills the clip at once
			if (weapon && (reloadType == 1 || reloadType == 2))
			{
				const auto* clip = GetClip();
				bool started = false;
				const auto* sequence = GetReloadSequence(weapon, hasClipMod, started);
				g_fill = Fill{
					.valid = true,
					.ammo = ammo,
					.weapon = weapon,
					.before = clip && clip->ammo == ammo ? static_cast<SInt32>(clip->count) : 0,
					.held = Loads(sequence),
					.started = started,
					.tick = GetTickCount64(),
				};
			}
			ThisStdCall(g_reloadEquip, player, ammo, rounds, extraData, applyEnchantment, noUnequip, noMessage);
		}

		// Actor::EquipObject making the clip of the equipped ammo (in Actor::Reload's queued equip, or at once in menus)
		UInt32 g_clipConstructor = 0x4BC550; // ItemChange::ItemChange
		BaseProcess::AmmoInfo* __fastcall ClipEquipHook(BaseProcess::AmmoInfo* clip, void*, TESForm* ammo, SInt32 rounds)
		{
			clip = ThisStdCall<BaseProcess::AmmoInfo*>(g_clipConstructor, clip, ammo, rounds);
			if (GET_CALLER_VAR(Actor*, -0x80) != g_thePlayer || !clip || !IsCurrent(g_fill) || g_fill.filled >= 0 || ammo != g_fill.ammo)
				return clip;
			g_fill.filled = clip->count;
			if (g_fill.held)
				clip->count = std::min<SInt32>(g_fill.before, clip->count);
			return clip;
		}

		// The engine loading a round of a looping reload: Actor::HandlePostAnimationActions when a round ends (the next
		// round starts), PlayerCharacter::CheckUserInputAttacks when attacking ends the loop (the round in the hand goes
		// in). Both read the rounds in the clip and add one. One less if keys loaded rounds since the round started, so it
		// loads nothing more and the loop goes on or ends by the clip as it is.
		template <int ActorOffset, bool EndsLoop>
		struct RoundLoad
		{
			inline static UInt32 roundsGetter = 0x726070;

			static SInt32 __fastcall Hook(BaseProcess::AmmoInfo* clip)
			{
				const auto rounds = ThisStdCall<SInt32>(roundsGetter, clip);
				if (GET_CALLER_VAR(Actor*, ActorOffset) != g_thePlayer)
					return rounds;
				if (EndsLoop)
				{
					const auto* animData = g_thePlayer->GetAnimData();
					g_loopEndedRound = animData ? animData->animSequence[kSequence_Weapon] : nullptr;
				}
				const auto keyLoads = std::exchange(g_keyLoads, 0);
				return keyLoads > 0 && rounds > 0 ? rounds - 1 : rounds;
			}
		};
		using NextRoundLoad = RoundLoad<-0xA0, false>;
		using AttackRoundLoad = RoundLoad<-0x9C, true>;
	}

	TimedExecution<Key>* Find(const BSAnimGroupSequence& sequence)
	{
		std::unique_lock lock(g_parsedMutex);
		auto [iter, inserted] = g_parsed.try_emplace(sequence.m_kName);
		if (inserted && sequence.m_spTextKeys)
		{
			ParsedKeys parsed;
			parsed.keys = TimedExecution<Key>(sequence.m_spTextKeys->GetKeys(), [&](const char* text, Key& key)
			{
				const auto parsedKey = ParseKey(text, sequence);
				if (parsedKey)
					key = *parsedKey;
				return parsedKey.has_value();
			});
			parsed.loads = std::ranges::any_of(parsed.keys.items, [](const auto& item) { return item.first.load; });
			if (!parsed.keys.items.empty())
				iter->second = std::move(parsed);
		}
		return iter->second ? &iter->second->keys : nullptr;
	}

	bool OnSequenceStart(AnimData* animData, BSAnimGroupSequence* sequence)
	{
		if (animData->actor != g_thePlayer || IsPlayersOtherAnimData(animData) || !sequence->animGroup)
			return false;
		const auto group = sequence->animGroup->GetBaseGroupID();
		if (GetSequenceType(group) != kSequence_Weapon)
			return true;
		g_keyLoads = 0;
		if (sequence == g_loopEndedRound)
			g_loopEndedRound = nullptr;
		if (!sequence->animGroup->IsReload() || !IsCurrent(g_fill) || g_fill.started)
			return true;
		// The reload of the last fill starts (a reload after the last shot waits for the attack to end). The fill was
		// held by the sequence the group was going to play; this one decides.
		g_fill.started = true;
		const bool held = Loads(sequence);
		if (held == g_fill.held)
			return true;
		g_fill.held = held;
		if (g_fill.filled < 0)
			return true;
		if (auto* clip = GetClip(); clip && clip->ammo == g_fill.ammo)
			SetRounds(*clip, held ? (std::min)(g_fill.before, g_fill.filled) : g_fill.filled);
		return true;
	}

	void Fire(const Key& key, const BSAnimGroupSequence* sequence)
	{
		auto* clip = GetClip();
		if (!clip || (key.load && sequence == g_loopEndedRound))
			return;
		// Keys reached before the engine's queued equip has filled the clip change what it fills it with
		const bool beforeFill = IsCurrent(g_fill) && g_fill.held && g_fill.filled < 0 && g_fill.ammo == clip->ammo;
		const SInt32 current = beforeFill ? g_fill.before : static_cast<SInt32>(clip->count);
		SInt32 rounds;
		if (key.load)
		{
			const auto capacity = GetCapacity(*clip);
			rounds = (std::max)(current, key.rounds ? (std::min)(capacity, current + key.rounds) : capacity);
			g_keyLoads += rounds - current;
		}
		else
			rounds = key.rounds ? (std::max<SInt32>)(0, current - key.rounds) : 0;
		if (beforeFill)
			g_fill.before = rounds;
		SetRounds(*clip, rounds);
	}

	void Reset()
	{
		g_fill = {};
		g_keyLoads = 0;
		g_loopEndedRound = nullptr;
	}

	void WriteDelayedHooks()
	{
		// Stewie's Tweaks detours the first (Prevent Reload Cancelling, bRememberWeaponAmmos) and replaces the second
		// (bRememberWeaponAmmos) in its NVSEPlugin_Load; NVAC inlines the getters of the last two. These go over them and
		// call them.
		WriteRelCall(0x95DCAC, ReloadFillHook, &g_reloadEquip);
		WriteRelCall(0x88CEDA, ClipEquipHook, &g_clipConstructor);
		WriteRelCall(0x8BAB1E, NextRoundLoad::Hook, &NextRoundLoad::roundsGetter);
		WriteRelCall(0x948C52, AttackRoundLoad::Hook, &AttackRoundLoad::roundsGetter);
	}
}
