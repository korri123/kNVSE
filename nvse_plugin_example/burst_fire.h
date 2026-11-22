#pragma once

#include "timed_execution.h"

struct AnimData;
class Actor;

// burstFire text key: an attack animation with it fires a shot at each of its Hit keys and ejects a casing at each of
// its Eject keys. The engine acts on the first of each (TESAnimGroup::keyTimes), kNVSE on the others, as timed keys of
// the play (AnimTime::burstKeys): while the sequence is the weapon section's current one, on the skeleton of the
// player's view (NPCs: their own). A shot after the last Eject key ejects its own casing. No shot is fired once the clip
// has run out or a reload has started.
namespace BurstFire
{
	struct Key
	{
		// A Hit key, else an Eject key
		bool shot = false;
		// A shot that ejects its own casing (no Eject key after it)
		bool casing = false;
	};

	// One play's burst
	struct State
	{
		// The rounds in the clip at the start or the last shot key: more now is the clip refilled (the game fills it a few
		// frames after the shot that emptied it, while the attack still plays)
		UInt32 clip = 0;
		bool stopped = false;
	};

	// The burst keys among the sequence's text keys: none without a burstFire key (TextKeys::Table reads them once per KF)
	TimedExecution<Key> Parse(const NiControllerSequence& sequence);
	// A play of an attack with burst keys starts, or starts over
	void Start(Actor* actor, State& state);
	// The play reached a burst key
	void Fire(Actor* actor, AnimData* animData, const BSAnimGroupSequence* sequence, const Key& key, State& state);
}
