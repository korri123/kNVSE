#pragma once

#include "NiNodes.h"

struct AnimData;
class Actor;
class TESObjectWEAP;

// What kNVSE keeps about an actor's shots (WeaponFire): kAnimationExtraData::fire on the root of the actor's 3rd person
// Animation (the player's too), like ReloadState. Written where the actor's shots are handled (Actor::FiresWeapon,
// Actor::StartAttack, Actor::HandlePostAnimationActions: the thread that updates the actor) and in
// kMessage_MainGameLoop, before those threads start.
struct FireState
{
	// The Animation the state belongs to: a node that gets another Animation starts over
	const AnimData* animData = nullptr;

	// The actor's last shot, in the time of its 3rd person Animation (AnimData::timePassed)
	float lastShotTime = -NI_INFINITY;

	// The first shot of a burst waiting for the Hit key of the attack loop. The sequence of the skeleton whose keys
	// act (the player's view) is compared by address only.
	struct OpenBolt
	{
		const BSAnimGroupSequence* sequence = nullptr;
		float hitTime = 0.0f;
		float lastTime = 0.0f;
		// The game fired before the Hit key: the shot it held back is fired there
		bool owed = false;
		// When the hold began (the Animation's time), and the game's attack loop timer then (its time to the burst's
		// first shot without a shot at the start)
		float startClock = 0.0f;
		float gameTimer = 0.0f;
		// The player's held shot is asked for (WeaponFire::FireHeldShot), since when
		bool firing = false;
		float firingClock = 0.0f;
	};
	OpenBolt openBolt;
	// The player's attack presses while a shot was held: each fires after the attack loop has played to its Hit key
	// again, one after the other. The attack group the game started them with (Actor::FiresWeapon's argument).
	UInt8 queuedShots = 0;
	UInt32 attackGroup = 0;
	// The player started an attack (Actor::FiresWeapon without sustained fire) whose shot has not come yet: the shot
	// begins a burst
	bool attackStarted = false;

	// bJamAfterShot: the attack rolled a jam (weapon, when), which plays after its shot once the attack animation has
	// ended (attack, its time at the last update, when it fired)
	struct Jam
	{
		const TESObjectWEAP* weapon = nullptr;
		float rollTime = 0.0f;
		const BSAnimGroupSequence* attack = nullptr;
		float lastTime = 0.0f;
		float shotClock = 0.0f;
	};
	Jam jam;

	// Shell casings of an automatic weapon's shots, ejected when the attack sequence's time since the shot reaches its
	// Eject key: the time each is due (the key skeleton's Animation time, AnimData::timePassed)
	struct Eject
	{
		static constexpr UInt8 kMaxPending = 8;
		const TESObjectWEAP* weapon = nullptr;
		float due[kMaxPending] = {};
		UInt8 count = 0;
	};
	Eject eject;

	// The actor's state, made if it has none; nullptr without a 3rd person Animation
	static FireState* Get(Actor* actor);
	// The actor's state if it has one
	static FireState* Peek(const Actor* actor);
};

// Shots of automatic weapons, their shell casings and jams.
//
// Open bolt: the game fires an automatic weapon when its attack starts (Actor::FiresWeapon queues the shot once the
// attack sequence is ANIMATING) and then by its fire rate (the attack loop timer, MiddleHighProcess::fAttackLoopTime);
// the Hit key of an attack loop did nothing. An attack loop (AttackLoop, AttackLoopIS and their up/down variants) with a
// Hit key now holds back the first shot of a burst until the animation reaches that key, and the next shots follow at
// the fire rate from there. A shot is never earlier than without the key: the game's shots before it are merged into
// the one at the key, and if the game had not fired yet it fires as usual. Released before the key, the shot still
// fires at it (the attack loop goes on until then). The player's bursts begin with each press of attack; attack pressed
// again while a shot is held is one more shot, which plays the attack loop to its Hit key again after the held one.
// An NPC's burst begins when it has not fired for 1.5 times the weapon's shot interval. The player: the keys of the
// view's skeleton; NPCs: theirs.
//
// Eject keys: TESObjectWEAP::Fire ejects an automatic weapon's shell casing with the shot (the engine acts on an
// attack's Eject key only for weapons that are not automatic). An attack sequence with an Eject key now ejects each
// shot's casing once the sequence has played from the shot to the key (around the loop for a key before the shot).
//
// Jam after the shot (bJamAfterShot): the game rolls a jam (fWeaponConditionJam1-10 by weapon condition) when an attack
// starts, in Actor::FiresWeapon and, for NPCs, Actor::StartAttack, and plays the jam animation instead of the attack,
// so a worn weapon could jam several times without firing. With the setting the attack fires and plays to its end (an
// attack loop: to the end of its cycle, no more shots), then the jam plays and ends the burst; attack pressed before
// that plays the jam at once. A shot that empties the clip does not jam (the reload follows).
namespace WeaponFire
{
	// Every frame outside menus, from kMessage_MainGameLoop: fires held back shots whose Hit key was reached, starts
	// queued shots, ejects casings, plays jams whose attack has ended
	void Update();
	// PlayerCharacter::Update with attack released and the attack loop timer running (where it would end the attack
	// loop): whether a held shot or a jam waiting for the loop's end takes over, keeping the attack loop playing
	bool ReleasedPlayerAttack();
	// From WriteDelayedHooks
	void WriteHooks();
}
