#include "weapon_fire.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
#include "commands_animation.h"
#include "game_types.h"
#include "main.h"
#include "settings.h"
#include "utility.h"

namespace WeaponFire
{
	namespace
	{
		constexpr UInt32 kQueuedAttackAnimOffset = 0x110; // Actor::uiQueuedAttackAnim, 0xFF without one
		constexpr UInt32 kForceFireWeaponOffset = 0x2F4; // HighProcess: Actor::HandlePostAnimationActions tries the queued attack again
		constexpr UInt32 kSetQueuedIdleFlagsSlot = 0x614 / 4; // BaseProcess vtable
		constexpr UInt32 kSetSustainedFireSlot = 0x6E0 / 4; // BaseProcess vtable
		constexpr UInt32 kGetSustainedFireSlot = 0x6DC / 4; // BaseProcess vtable
		// A burst begins when the actor has not fired for this many shot intervals
		constexpr float kBurstGapIntervals = 1.5f;
		// A rolled jam is dropped when no shot follows within this long (the Animation's time)
		constexpr float kJamLifetime = 3.0f;
		// The player's shot at the Hit key is given up when it has not come after this long (the Animation's time)
		constexpr float kFiringLifetime = 0.25f;

		// Actors with a shot held for a Hit key, by ref ID. Added where the actor's shots are fired (AI threads for
		// NPCs), handled in kMessage_MainGameLoop.
		std::mutex g_pendingMutex;
		std::vector<UInt32> g_pending;

		AnimData* GetAnimation(const Actor* actor)
		{
			return actor->baseProcess ? actor->baseProcess->GetAnimData() : nullptr;
		}

		// The time of the actor's 3rd person Animation
		float GetClock(const Actor* actor)
		{
			const auto* animData = GetAnimation(actor);
			return animData ? animData->timePassed : 0.0f;
		}

		// The weapon section's sequence on the skeleton whose keys act: the player's view, an NPC's own
		BSAnimGroupSequence* GetKeySequence(Actor* actor)
		{
			auto* player = PlayerCharacter::GetSingleton();
			auto* animData = actor == player ? player->GetAnimData() : GetAnimation(actor);
			return animData ? animData->animSequence[kSequence_Weapon] : nullptr;
		}

		bool IsPlaying(const BSAnimGroupSequence* sequence)
		{
			const auto state = sequence->m_eState;
			return state == NiControllerSequence::ANIMATING || state == NiControllerSequence::EASEIN || state == NiControllerSequence::TRANSDEST;
		}

		bool IsAttackLoop(const BSAnimGroupSequence* sequence)
		{
			if (!sequence->animGroup)
				return false;
			const auto group = sequence->animGroup->GetBaseGroupID();
			return group >= kAnimGroup_AttackLoop && group <= kAnimGroup_AttackLoopISDown;
		}

		const NiTextKey* FindKey(const BSAnimGroupSequence* sequence, const char* name)
		{
			if (!sequence->m_spTextKeys)
				return nullptr;
			for (const auto& key : sequence->m_spTextKeys->GetKeys())
			{
				if (const char* text = key.m_kText.CStr(); text && !_stricmp(text, name))
					return &key;
			}
			return nullptr;
		}

		const NiTextKey* FindHitKey(const BSAnimGroupSequence* sequence) { return FindKey(sequence, "Hit"); }

		// The time between shots, as MiddleHighProcess::ClearAutomaticShotsToFire sets the attack loop timer
		float GetShotInterval(const Actor* actor, TESObjectWEAP* weapon)
		{
			const auto* weaponInfo = actor->baseProcess->GetWeaponInfo();
			const bool rateOfFireMod = weaponInfo && weaponInfo->HasModEffect(TESObjectWEAP::kWeaponModEffect_IncreaseRateOfFire);
			const float rate = ThisStdCall<float>(0x821640, weapon) // TESObjectWEAP::GetFireRate
				* ThisStdCall<float>(0x646020, weapon, rateOfFireMod); // TESObjectWEAP::GetAnimAttackMult
			return rate > 0.0f ? 1.0f / rate : 0.0f;
		}

		constexpr UInt32 kAttackLoopTimeOffset = 0x1D4; // MiddleHighProcess::fAttackLoopTime: game time to the burst's next shot
		// While a shot is held the attack loop timer is held at this: the game asks for no shot (an attack loop with an
		// interruptLoop key would start over with each one) and does not end the attack loop (Actor::PickAnimations ends
		// it at 0)
		constexpr float kHeldTimer = 1000.0f;

		float* GetAttackLoopTimer(BaseProcess* process)
		{
			return process->processLevel <= 1 ? reinterpret_cast<float*>(reinterpret_cast<UInt8*>(process) + kAttackLoopTimeOffset) : nullptr;
		}

		bool IsTimerHeld(BaseProcess* process)
		{
			const auto* timer = GetAttackLoopTimer(process);
			return timer && *timer >= kHeldTimer / 2;
		}

		// The sequence reached its Hit key: its time passed the key, or a looping sequence went around
		bool IsHitReached(const FireState::OpenBolt& bolt, const BSAnimGroupSequence* sequence)
		{
			const float time = sequence->m_fLastScaledTime;
			if (time >= bolt.hitTime)
				return true;
			return sequence->m_eCycleType == NiControllerSequence::LOOP && bolt.lastTime > -NI_INFINITY && time < bolt.lastTime - 0.01f;
		}

		void SetForceFireWeapon(BaseProcess* process, bool force)
		{
			if (process->processLevel == 0)
				*(reinterpret_cast<UInt8*>(process) + kForceFireWeaponOffset) = force;
		}

		// The shot held back for the Hit key: fired by Actor::HandlePostAnimationActions in the actor's next update,
		// one shot, and the next one a shot interval later
		void QueueShot(Actor* actor)
		{
			auto* process = actor->baseProcess;
			ThisStdCall(0x8D8380, process, 1); // MiddleHighProcess::SetShotsToFire
			ThisStdCall((*reinterpret_cast<UInt32**>(process))[kSetQueuedIdleFlagsSlot], process, kIdleFlag_FireWeapon);
			SetForceFireWeapon(process, false);
			process->ResetAttackLoopTimer(true);
		}

		// Fires the held shot. Returns whether the actor stays pending: PlayerCharacter::Update drops the queued idle
		// flags before it reads the attack control, so the player's shot is asked for there instead (the attack loop
		// timer runs out at once while attack is held; ReleasedPlayerAttack queues it when attack was released).
		bool FireHeldShot(Actor* actor, FireState& state)
		{
			if (actor != PlayerCharacter::GetSingleton())
			{
				state.openBolt = {};
				QueueShot(actor);
				return false;
			}
			state.openBolt.firing = true;
			state.openBolt.firingClock = GetClock(actor);
			if (auto* timer = GetAttackLoopTimer(actor->baseProcess))
				*timer = 0.0001f;
			return true;
		}

		// The shot is no longer held (the burst has begun) and the timer counts again
		void EndHold(Actor* actor, FireState& state)
		{
			state.openBolt = {};
			state.lastShotTime = GetClock(actor);
			if (IsTimerHeld(actor->baseProcess))
				actor->baseProcess->ResetAttackLoopTimer(true);
		}

		void AddPending(const Actor* actor)
		{
			std::unique_lock lock(g_pendingMutex);
			if (std::ranges::find(g_pending, actor->refID) == g_pending.end())
				g_pending.push_back(actor->refID);
		}

		// Holds the actor's shots for the Hit key of this attack loop sequence, the shot owed (fired at the key).
		// restarted: the attack was just started over, so the sequence's time is still the one before.
		void BeginHold(Actor* actor, FireState& state, const BSAnimGroupSequence* sequence, const NiTextKey& hit, float* timer, bool owed, bool restarted = false)
		{
			state.openBolt = {
				.sequence = sequence,
				.hitTime = hit.m_fTime,
				.lastTime = restarted ? -NI_INFINITY : sequence->m_fLastScaledTime,
				.owed = owed,
				.startClock = GetClock(actor),
				.gameTimer = owed ? 0.0f : *timer,
			};
			*timer = kHeldTimer;
			AddPending(actor);
		}

		// Whether the game's shot now is held back for the Hit key of the burst's attack loop. burstStart: the player
		// started an attack whose shot this is.
		bool HoldsShot(Actor* actor, FireState& state, TESObjectWEAP* weapon, bool burstStart)
		{
			auto& bolt = state.openBolt;
			if (bolt.sequence)
			{
				const auto* sequence = GetKeySequence(actor);
				if (!bolt.firing && sequence == bolt.sequence && IsPlaying(sequence) && !IsHitReached(bolt, sequence))
					return true;
				// The shot asked for, reached (Update would fire it in the next frame) or cut: this shot fires
				EndHold(actor, state);
				return false;
			}
			if (!weapon->IsAutomatic())
				return false;
			if (actor != PlayerCharacter::GetSingleton())
				burstStart = GetClock(actor) - state.lastShotTime > kBurstGapIntervals * GetShotInterval(actor, weapon);
			if (!burstStart)
				return false;
			auto* timer = GetAttackLoopTimer(actor->baseProcess);
			const auto* sequence = GetKeySequence(actor);
			if (!timer || !sequence || !IsAttackLoop(sequence) || !IsPlaying(sequence))
				return false;
			const auto* hit = FindHitKey(sequence);
			if (!hit || sequence->m_fLastScaledTime >= hit->m_fTime)
				return false;
			BeginHold(actor, state, sequence, *hit, timer, false);
			return true;
		}

		// Whether the actor's shot is still held (or, the player's, asked for)
		bool UpdateHeldShot(Actor* actor, FireState& state)
		{
			auto& bolt = state.openBolt;
			if (bolt.firing)
			{
				// The player's shot was asked for and none came (the attack ended): given up
				if (GetClock(actor) - bolt.firingClock < kFiringLifetime)
					return true;
				bolt = {};
				state.queuedShots = 0;
				return false;
			}
			const auto* sequence = GetKeySequence(actor);
			if (sequence != bolt.sequence || !IsPlaying(sequence))
			{
				// Cut: attack released with no shot owed (the game cleared the timer), a reload, a jam, ... The other
				// attack loop of an aim change goes on with the burst, so its shot fires now.
				if (bolt.owed && sequence && IsAttackLoop(sequence) && IsPlaying(sequence))
				{
					state.lastShotTime = GetClock(actor);
					return FireHeldShot(actor, state);
				}
				bolt = {};
				state.queuedShots = 0;
				if (IsTimerHeld(actor->baseProcess))
					actor->baseProcess->ResetAttackLoopTimer(false);
				return false;
			}
			if (!IsHitReached(bolt, sequence))
			{
				bolt.lastTime = sequence->m_fLastScaledTime;
				return true;
			}
			// The game's own first shot of the burst would come this much later: it fires then
			const float gameShotIn = bolt.gameTimer - (GetClock(actor) - bolt.startClock);
			state.lastShotTime = GetClock(actor);
			if (bolt.owed || gameShotIn <= 0.0f)
				return FireHeldShot(actor, state);
			bolt = {};
			if (auto* timer = GetAttackLoopTimer(actor->baseProcess))
				*timer = gameShotIn;
			return false;
		}

		UInt32 g_firesWeapon = 0x893A40; // Actor::FiresWeapon

		// The next of the player's queued shots: the attack starts over, as the press would have started it, and its
		// shot is held for the Hit key. Waits for the held shot and a jam. Whether the actor stays pending.
		bool StartQueuedShot(Actor* actor, FireState& state)
		{
			if (state.openBolt.sequence || state.jam.attack)
				return true;
			const auto* clip = actor->baseProcess->GetAmmoInfo();
			if (actor != PlayerCharacter::GetSingleton() || !clip || clip->count <= 0)
			{
				state.queuedShots = 0;
				return false;
			}
			--state.queuedShots;
			if (!ThisStdCall<bool>(g_firesWeapon, actor, state.attackGroup))
			{
				state.queuedShots = 0;
				return false;
			}
			auto* timer = GetAttackLoopTimer(actor->baseProcess);
			auto* sequence = GetKeySequence(actor);
			const auto* hit = sequence && IsAttackLoop(sequence) && IsPlaying(sequence) ? FindHitKey(sequence) : nullptr;
			if (!timer || !hit)
			{
				// Not held: the game fires it
				state.queuedShots = 0;
				return false;
			}
			// Actor::FiresWeapon starts the attack loop only if it isn't playing (the press of attack restarted it):
			// it starts over here, on both skeletons
			sequence->ResetSequence();
			if (auto* animData3rd = GetAnimation(actor); animData3rd && animData3rd->animSequence[kSequence_Weapon] != sequence)
			{
				if (auto* other = animData3rd->animSequence[kSequence_Weapon]; other && IsAttackLoop(other) && IsPlaying(other))
					other->ResetSequence();
			}
			BeginHold(actor, state, sequence, *hit, timer, true, true);
			return true;
		}

		// Plays the jam the attack rolled, as Actor::FiresWeapon does in its place, and ends the burst
		void StartJam(Actor* actor, const TESObjectWEAP* weapon)
		{
			auto* process = actor->baseProcess;
			auto* animData = GetAnimation(actor);
			const auto* clip = process->GetAmmoInfo();
			if (!animData || (clip && clip->count == 0)) // a reload follows
				return;
			const UInt32 group = (ThisStdCall<UInt32>(0x51E2A0, weapon, false) & 0xFF) + 0x17; // TESObjectWEAP::GetReloadAnimGroup: Jam<reload>
			const auto fullGroup = ThisStdCall<UInt16>(0x897910, actor, group, 0, false, nullptr); // Actor::GetAnimGroup
			if (CdeclCall<UInt32>(0x5F2440, fullGroup) != group) // TESAnimGroup::GetType: no such animation
				return;
			ThisStdCall(0x8B28C0, actor, group, animData); // Actor::StartWeaponGroup
			actor->SetAnimAction(Decoding::kAnimAction_Reload, animData->animSequence[kSequence_Weapon]);
			if (actor == PlayerCharacter::GetSingleton())
				actor->StartAnimOn1stPerson(fullGroup, Actor::kActionFlag_ImmediateStart);
			// The burst ends: no more shots, and the next attack is the first of a burst again (rolls a jam; the game
			// clears this when attack is released during an attack group, not during the jam)
			process->ResetAttackLoopTimer(false);
			ThisStdCall((*reinterpret_cast<UInt32**>(process))[kSetSustainedFireSlot], process, false);
			SetForceFireWeapon(process, false);
			*reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(actor) + kQueuedAttackAnimOffset) = 0xFF;
		}

		// Plays the jam waiting for the end of the attack, if the actor still has the weapon
		void PlayPendingJam(Actor* actor, FireState& state)
		{
			const auto* weapon = state.jam.weapon;
			state.jam = {};
			state.queuedShots = 0;
			const auto* weaponInfo = actor->baseProcess->GetWeaponInfo();
			if (weaponInfo && weaponInfo->weapon == weapon)
				StartJam(actor, weapon);
			else if (IsTimerHeld(actor->baseProcess))
				actor->baseProcess->ResetAttackLoopTimer(false);
		}

		// The shot of an attack that rolled a jam was fired: the jam plays once the attack has ended
		void WaitForAttackEnd(Actor* actor, FireState& state, TESObjectWEAP* weapon)
		{
			const auto* clip = actor->baseProcess->GetAmmoInfo();
			if (clip && clip->count == 0) // a reload follows
				return;
			const auto* attack = GetKeySequence(actor);
			if (!attack || !attack->animGroup || !attack->animGroup->IsAttack() || !IsPlaying(attack))
			{
				StartJam(actor, weapon);
				return;
			}
			state.jam = {
				.weapon = weapon,
				.attack = attack,
				.lastTime = attack->m_fLastScaledTime,
				.shotClock = GetClock(actor),
			};
			state.queuedShots = 0;
			// No more shots, and the attack loop is not ended (Actor::PickAnimations ends it when the timer is 0)
			if (auto* timer = GetAttackLoopTimer(actor->baseProcess); timer && IsAttackLoop(attack))
				*timer = kHeldTimer;
			AddPending(actor);
		}

		// Whether the jam still waits for the attack to end
		bool UpdatePendingJam(Actor* actor, FireState& state)
		{
			auto& jam = state.jam;
			const auto* sequence = GetKeySequence(actor);
			const bool ended = sequence != jam.attack || !IsPlaying(sequence)
				|| sequence->m_fLastScaledTime >= sequence->m_fEndKeyTime
				|| (sequence->m_eCycleType == NiControllerSequence::LOOP && sequence->m_fLastScaledTime < jam.lastTime - 0.01f)
				|| GetClock(actor) - jam.shotClock > kJamLifetime;
			if (!ended)
			{
				jam.lastTime = sequence->m_fLastScaledTime;
				return true;
			}
			PlayPendingJam(actor, state);
			return false;
		}

		UInt32 g_ejectShellCasing = 0x524DB0; // TESObjectWEAP::EjectShellCasing

		// The Animation time of the skeleton whose keys act
		float GetKeyClock(Actor* actor)
		{
			auto* player = PlayerCharacter::GetSingleton();
			auto* animData = actor == player ? player->GetAnimData() : GetAnimation(actor);
			return animData ? animData->timePassed : 0.0f;
		}

		// Whether casings are still due
		bool UpdateEject(Actor* actor, FireState& state)
		{
			auto& eject = state.eject;
			const auto* weaponInfo = actor->baseProcess->GetWeaponInfo();
			auto* weapon = weaponInfo ? weaponInfo->weapon : nullptr;
			const float clock = GetKeyClock(actor);
			UInt8 kept = 0;
			for (UInt8 i = 0; i < eject.count; ++i)
			{
				// The weapon was put away or changed (or the clock started over, a new Animation): dropped
				if (!weapon || weapon != eject.weapon || clock < eject.due[i] - 10.0f)
					continue;
				if (clock >= eject.due[i])
					ThisStdCall(g_ejectShellCasing, weapon, actor);
				else
					eject.due[kept++] = eject.due[i];
			}
			eject.count = kept;
			return kept > 0;
		}

		// TESObjectWEAP::Fire ejecting an automatic weapon's casing with the shot: an attack sequence with an Eject key
		// ejects it when the sequence has played from the shot to the key
		void __fastcall EjectHook(TESObjectWEAP* weapon, void*, TESObjectREFR* ref)
		{
			auto* actor = DYNAMIC_CAST(ref, TESForm, Actor);
			const auto* sequence = actor ? GetKeySequence(actor) : nullptr;
			const NiTextKey* key = sequence && sequence->animGroup && sequence->animGroup->IsAttack() && IsPlaying(sequence)
				&& !FindKey(sequence, "burstFire") ? FindKey(sequence, "Eject") : nullptr;
			auto* state = key ? FireState::Get(actor) : nullptr;
			if (!state)
			{
				ThisStdCall(g_ejectShellCasing, weapon, ref);
				return;
			}
			// Sequence time from the shot to the key: around the loop for a key the loop has passed
			float delay = key->m_fTime - sequence->m_fLastScaledTime;
			if (delay < 0.0f && sequence->m_eCycleType == NiControllerSequence::LOOP)
				delay += sequence->m_fEndKeyTime - sequence->m_fBeginKeyTime;
			auto& eject = state->eject;
			if (delay <= 0.0f || eject.count == FireState::Eject::kMaxPending)
			{
				ThisStdCall(g_ejectShellCasing, weapon, ref);
				return;
			}
			auto* player = PlayerCharacter::GetSingleton();
			auto* animData = actor == player ? player->GetAnimData() : GetAnimation(actor);
			const float mult = animData ? GetAnimMult(animData, static_cast<UInt8>(sequence->animGroup->groupID)) : 1.0f;
			const float rate = sequence->m_fFrequency * (mult > 0.0f ? mult : 1.0f);
			if (eject.weapon != weapon)
				eject.count = 0;
			eject.weapon = weapon;
			eject.due[eject.count++] = GetKeyClock(actor) + delay / (rate > 0.0f ? rate : 1.0f);
			AddPending(actor);
		}

		// Whether the actor needs updates: a held shot, queued shots, a jam waiting, casings waiting
		bool UpdateActor(UInt32 refID)
		{
			auto* actor = DYNAMIC_CAST(LookupFormByRefID(refID), TESForm, Actor);
			if (!actor || actor->IsDeleted() || actor->IsDead(true) || !actor->baseProcess)
				return false;
			auto* state = FireState::Peek(actor);
			if (!state)
				return false;
			bool keep = false;
			if (state->eject.count)
				keep |= UpdateEject(actor, *state);
			if (state->openBolt.sequence)
				keep |= UpdateHeldShot(actor, *state);
			if (state->jam.attack)
				keep |= UpdatePendingJam(actor, *state);
			if (state->queuedShots)
				keep |= StartQueuedShot(actor, *state);
			return keep;
		}

		// Actor::HandlePostAnimationActions firing the weapon (kIdleFlag_FireWeapon)
		UInt32 g_fire = 0x523150; // TESObjectWEAP::Fire
		void __fastcall FireHook(TESObjectWEAP* weapon, void*, Actor* actor)
		{
			auto* state = FireState::Get(actor);
			const bool burstStart = state && std::exchange(state->attackStarted, false);
			if (state && HoldsShot(actor, *state, weapon, burstStart))
			{
				state->openBolt.owed = true;
				return;
			}
			ThisStdCall(g_fire, weapon, actor);
			if (!state)
				return;
			state->lastShotTime = GetClock(actor);
			if (!state->jam.weapon || state->jam.attack)
				return;
			const bool due = state->jam.weapon == weapon && state->lastShotTime - state->jam.rollTime < kJamLifetime;
			state->jam = {};
			if (due && g_pluginSettings.jamAfterShot)
				WaitForAttackEnd(actor, *state, weapon);
		}

		// PlayerCharacter::CheckUserInputAttacks starting an attack or firing the next shot of a burst
		bool __fastcall PlayerFiresWeaponHook(Actor* player, void*, UInt32 attackGroup)
		{
			auto* process = player->baseProcess;
			auto* state = FireState::Get(player);
			const bool sustained = ThisStdCall<bool>((*reinterpret_cast<UInt32**>(process))[kGetSustainedFireSlot], process);
			if (state && !sustained)
			{
				// Attack pressed while the jam waits for the attack to end: it plays now, instead of this attack
				if (state->jam.attack)
				{
					PlayPendingJam(player, *state);
					return false;
				}
				// Attack pressed while a shot is held for the Hit key: one more shot, after it. The press reset the
				// attack loop timer before this call (PlayerCharacter::CheckUserInputAttacks, call at 0x948ED6) and
				// asked for its attack to be tried again: the shot stays held.
				if (state->openBolt.sequence)
				{
					if (state->queuedShots < 255)
						++state->queuedShots;
					state->attackGroup = attackGroup;
					if (auto* timer = GetAttackLoopTimer(process); timer && !state->openBolt.firing)
						*timer = kHeldTimer;
					SetForceFireWeapon(process, false);
					return true;
				}
				state->attackStarted = true;
				state->attackGroup = attackGroup;
			}
			const bool attacks = ThisStdCall<bool>(g_firesWeapon, player, attackGroup);
			if (state && !attacks)
				state->attackStarted = false;
			return attacks;
		}

		// RandomBool rolling the jam of an attack that starts (Actor::FiresWeapon, Actor::StartAttack)
		template <int ActorOffset>
		bool __cdecl JamRollHook(float chance)
		{
			const bool jams = CdeclCall<bool>(0x4DFF00, chance); // RandomBool
			if (!g_pluginSettings.jamAfterShot)
				return jams;
			auto* actor = GET_CALLER_VAR(Actor*, ActorOffset);
			const auto* weaponInfo = actor->baseProcess ? actor->baseProcess->GetWeaponInfo() : nullptr;
			auto* state = FireState::Get(actor);
			if (!state || !weaponInfo || !weaponInfo->weapon)
				return jams;
			// A jam waits for its attack to end and another attack starts (an NPC's): it jams instead, as the game does
			if (state->jam.attack)
			{
				state->jam = {};
				if (IsTimerHeld(actor->baseProcess))
					actor->baseProcess->ResetAttackLoopTimer(false);
				return true;
			}
			if (!jams)
				return false;
			state->jam = { .weapon = weaponInfo->weapon, .rollTime = GetClock(actor) };
			return false;
		}
	}

	void Update()
	{
		std::unique_lock lock(g_pendingMutex);
		// UpdateActor may add the actor again (AddPending): work on a copy
		auto pending = std::move(g_pending);
		g_pending.clear();
		lock.unlock();
		std::vector<UInt32> keep;
		for (const UInt32 refID : pending)
		{
			if (UpdateActor(refID))
				keep.push_back(refID);
		}
		lock.lock();
		for (const UInt32 refID : keep)
		{
			if (std::ranges::find(g_pending, refID) == g_pending.end())
				g_pending.push_back(refID);
		}
	}

	bool ReleasedPlayerAttack()
	{
		auto* player = PlayerCharacter::GetSingleton();
		auto* state = FireState::Peek(player);
		if (!state)
			return false;
		// The attack loop plays to the end of its cycle before the jam
		if (state->jam.attack)
			return IsAttackLoop(state->jam.attack);
		// The attack loop goes on into the next queued shot (which starts it over)
		if (!state->openBolt.sequence)
			return state->queuedShots > 0;
		if (state->openBolt.firing)
		{
			QueueShot(player);
			return true;
		}
		return state->openBolt.owed;
	}

	void WriteHooks()
	{
		WriteRelCall(0x8BADE9, FireHook, &g_fire);
		WriteRelCall(0x524A93, EjectHook, &g_ejectShellCasing);
		// NVSE's DisablePlayerControls (attacking) replaces this call: it is called through
		WriteRelCall(0x949CF1, PlayerFiresWeaponHook, &g_firesWeapon);
		WriteRelCall(0x893FEE, JamRollHook<-0xB4>);
		WriteRelCall(0x89388C, JamRollHook<-0x38>);
	}
}
