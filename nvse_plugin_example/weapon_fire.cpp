#include "weapon_fire.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "GameForms.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
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
		constexpr UInt32 kStartAnimOn1stPersonSlot = 0x4B0 / 4; // PlayerCharacter vtable
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
			auto* animData = actor == g_thePlayer ? g_thePlayer->GetAnimData() : GetAnimation(actor);
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

		const NiTextKey* FindHitKey(const BSAnimGroupSequence* sequence)
		{
			if (!sequence->m_spTextKeys)
				return nullptr;
			for (const auto& key : sequence->m_spTextKeys->GetKeys())
			{
				if (const char* text = key.m_kText.CStr(); text && !_stricmp(text, "Hit"))
					return &key;
			}
			return nullptr;
		}

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
			if (actor != g_thePlayer)
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

		// Whether the game's shot now is held back for the Hit key of the burst's attack loop
		bool HoldsShot(Actor* actor, FireState& state, TESObjectWEAP* weapon)
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
			if (GetClock(actor) - state.lastShotTime <= kBurstGapIntervals * GetShotInterval(actor, weapon))
				return false;
			auto* timer = GetAttackLoopTimer(actor->baseProcess);
			const auto* sequence = GetKeySequence(actor);
			if (!timer || !sequence || !IsAttackLoop(sequence) || !IsPlaying(sequence))
				return false;
			const auto* hit = FindHitKey(sequence);
			if (!hit || sequence->m_fLastScaledTime >= hit->m_fTime)
				return false;
			bolt = {
				.sequence = sequence,
				.hitTime = hit->m_fTime,
				.lastTime = sequence->m_fLastScaledTime,
				.startClock = GetClock(actor),
				.gameTimer = *timer,
			};
			*timer = kHeldTimer;
			std::unique_lock lock(g_pendingMutex);
			if (std::ranges::find(g_pending, actor->refID) == g_pending.end())
				g_pending.push_back(actor->refID);
			return true;
		}

		// Whether the actor's shot is still held (or, the player's, asked for)
		bool UpdateHeldShot(UInt32 refID)
		{
			auto* actor = DYNAMIC_CAST(LookupFormByRefID(refID), TESForm, Actor);
			if (!actor || actor->IsDeleted() || actor->IsDead(true) || !actor->baseProcess)
				return false;
			auto* state = FireState::Peek(actor);
			if (!state || !state->openBolt.sequence)
				return false;
			auto& bolt = state->openBolt;
			if (bolt.firing)
			{
				// The player's shot was asked for and none came (the attack ended): given up
				if (GetClock(actor) - bolt.firingClock < kFiringLifetime)
					return true;
				bolt = {};
				return false;
			}
			const auto* sequence = GetKeySequence(actor);
			if (sequence != bolt.sequence || !IsPlaying(sequence))
			{
				// Cut: attack released with no shot owed (the game cleared the timer), a reload, a jam, ... The other
				// attack loop of an aim change goes on with the burst, so its shot fires now.
				if (bolt.owed && sequence && IsAttackLoop(sequence) && IsPlaying(sequence))
				{
					state->lastShotTime = GetClock(actor);
					return FireHeldShot(actor, *state);
				}
				bolt = {};
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
			state->lastShotTime = GetClock(actor);
			if (bolt.owed || gameShotIn <= 0.0f)
				return FireHeldShot(actor, *state);
			bolt = {};
			if (auto* timer = GetAttackLoopTimer(actor->baseProcess))
				*timer = gameShotIn;
			return false;
		}

		// Plays the jam the attack rolled, as Actor::FiresWeapon does in its place, and ends the burst
		void StartJam(Actor* actor, TESObjectWEAP* weapon)
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
			ThisStdCall((*reinterpret_cast<UInt32**>(actor))[kStartAnimOn1stPersonSlot], actor, static_cast<UInt32>(fullGroup), 1);
			// The burst ends: no more shots, and the next attack is the first of a burst again (rolls a jam; the game
			// clears this when attack is released during an attack group, not during the jam)
			process->ResetAttackLoopTimer(false);
			ThisStdCall((*reinterpret_cast<UInt32**>(process))[kSetSustainedFireSlot], process, false);
			SetForceFireWeapon(process, false);
			*reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(actor) + kQueuedAttackAnimOffset) = 0xFF;
		}

		// Actor::HandlePostAnimationActions firing the weapon (kIdleFlag_FireWeapon)
		UInt32 g_fire = 0x523150; // TESObjectWEAP::Fire
		void __fastcall FireHook(TESObjectWEAP* weapon, void*, Actor* actor)
		{
			auto* state = FireState::Get(actor);
			if (state && HoldsShot(actor, *state, weapon))
			{
				state->openBolt.owed = true;
				return;
			}
			ThisStdCall(g_fire, weapon, actor);
			if (!state)
				return;
			state->lastShotTime = GetClock(actor);
			if (!state->jamWeapon)
				return;
			const bool due = state->jamWeapon == weapon && state->lastShotTime - state->jamRollTime < kJamLifetime;
			state->jamWeapon = nullptr;
			if (due && g_pluginSettings.jamAfterShot)
				StartJam(actor, weapon);
		}

		// RandomBool rolling the jam of an attack that starts (Actor::FiresWeapon, Actor::StartAttack)
		template <int ActorOffset>
		bool __cdecl JamRollHook(float chance)
		{
			const bool jams = CdeclCall<bool>(0x4DFF00, chance); // RandomBool
			if (!jams || !g_pluginSettings.jamAfterShot)
				return jams;
			auto* actor = GET_CALLER_VAR(Actor*, ActorOffset);
			const auto* weaponInfo = actor->baseProcess ? actor->baseProcess->GetWeaponInfo() : nullptr;
			auto* state = FireState::Get(actor);
			if (!state || !weaponInfo || !weaponInfo->weapon)
				return jams;
			state->jamWeapon = weaponInfo->weapon;
			state->jamRollTime = GetClock(actor);
			return false;
		}
	}

	void Update()
	{
		std::unique_lock lock(g_pendingMutex);
		std::erase_if(g_pending, [](UInt32 refID) { return !UpdateHeldShot(refID); });
	}

	bool ReleasedPlayerAttack()
	{
		auto* state = FireState::Peek(g_thePlayer);
		if (!state || !state->openBolt.sequence)
			return false;
		if (state->openBolt.firing)
		{
			QueueShot(g_thePlayer);
			return true;
		}
		return state->openBolt.owed;
	}

	void WriteHooks()
	{
		WriteRelCall(0x8BADE9, FireHook, &g_fire);
		WriteRelCall(0x893FEE, JamRollHook<-0xB4>);
		WriteRelCall(0x89388C, JamRollHook<-0x38>);
	}
}
