#include "burst_fire.h"

#include <algorithm>

#include "GameObjects.h"
#include "GameProcess.h"
#include "game_types.h"
#include "main.h"
#include "utility.h"

namespace BurstFire
{
	namespace
	{
		enum class KeyType { None, Hit, Eject };

		KeyType GetKeyType(const NiTextKey& key)
		{
			const char* text = key.m_kText.CStr();
			if (!text)
				return KeyType::None;
			if (!_stricmp(text, "Hit"))
				return KeyType::Hit;
			if (!_stricmp(text, "Eject"))
				return KeyType::Eject;
			return KeyType::None;
		}

		// Whether the clip the burst fires from ran out: empty, being reloaded, or full again
		bool IsClipSpent(Actor* actor, State& state)
		{
			const auto* clip = actor->baseProcess->GetAmmoInfo();
			if (!clip || IsGodMode())
				return false;
			if (clip->count == 0 || clip->count > state.clip || actor->IsAnimActionReload())
				return true;
			state.clip = clip->count;
			return false;
		}
	}

	TimedExecution<Key> Parse(const NiControllerSequence& sequence)
	{
		const auto keys = sequence.m_spTextKeys->GetKeys();
		if (std::ranges::none_of(keys, [](const NiTextKey& key) { return key.m_kText.CStr() && !_stricmp(key.m_kText.CStr(), "burstFire"); }))
			return {};
		// The first of each (in the order the KF lists them) is the engine's
		float lastEject = -NI_INFINITY;
		bool firstEject = true;
		for (const auto& key : keys)
		{
			if (GetKeyType(key) == KeyType::Eject && !std::exchange(firstEject, false))
				lastEject = (std::max)(lastEject, key.m_fTime);
		}
		std::vector<std::pair<Key, float>> items;
		bool firstHit = true;
		firstEject = true;
		for (const auto& key : keys)
		{
			const auto type = GetKeyType(key);
			if (type == KeyType::None || std::exchange(type == KeyType::Hit ? firstHit : firstEject, false))
				continue;
			const bool shot = type == KeyType::Hit;
			items.emplace_back(Key{ .shot = shot, .casing = shot && key.m_fTime > lastEject }, key.m_fTime);
		}
		return TimedExecution<Key>(std::move(items));
	}

	void Start(Actor* actor, State& state)
	{
		const auto* clip = actor->baseProcess ? actor->baseProcess->GetAmmoInfo() : nullptr;
		state = { .clip = clip ? clip->count : 0 };
	}

	void Fire(Actor* actor, AnimData* animData, const BSAnimGroupSequence* sequence, const Key& key, State& state)
	{
		// The attack was cut, or the player's other skeleton plays along: its keys pass without acting
		if (animData->animSequence[kSequence_Weapon] != sequence || IsPlayersOtherAnimData(animData))
			return;
		if (key.shot)
		{
			state.stopped = state.stopped || IsClipSpent(actor, state);
			if (state.stopped)
				return;
			actor->FireWeapon();
			if (!key.casing)
				return;
		}
		actor->EjectFromWeapon(actor->GetWeaponForm());
	}
}
