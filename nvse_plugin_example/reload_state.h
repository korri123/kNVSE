#pragma once

#include "NiNodes.h"

struct AnimData;
class Actor;
class TESForm;
class TESObjectWEAP;

// What kNVSE keeps about an actor's reload (AmmoKeys, NPCLoopingReloadFix): kAnimationExtraData::reload on the root of
// the actor's 3rd person Animation (the player's too, whichever view plays). It goes with the Animation: without one
// nothing reloads. No locks: an actor's reload runs where its animations update (AI threads for NPCs, Actor::Reload and
// Actor::HandlePostAnimationActions), and the ammo keys act on the main thread in kMessage_MainGameLoop, before those
// threads start.
struct ReloadState
{
	// The Animation the state belongs to: a node that gets another Animation starts over
	const AnimData* animData = nullptr;

	// The clip fill of the reload that Actor::Reload / PlayerCharacter::Reload started last
	struct Fill
	{
		bool valid = false;
		TESForm* ammo = nullptr;
		const TESObjectWEAP* weapon = nullptr;
		// The rounds of this ammo in the clip before the reload (0 when it changes the ammo type)
		SInt32 before = 0;
		// The rounds the engine filled the clip with: at once for NPCs, once the queued equip has done so for the
		// player (-1 before)
		SInt32 filled = -1;
		// The clip keeps its rounds instead of the fill: the reload KF loads them by key (AmmoKeys)
		bool held = false;
		// The fill may be held: not an NPC's looping reload, whose rounds the loop loads, nor a round of one played as a
		// plain reload (bFixNPCLoopingReloads off), which loads them all at once
		bool holdable = false;
		// A reload sequence has started for it. It stays current until a sequence of another group starts in the weapon
		// section; one that hasn't started, for kFillLifetimeMs
		bool started = false;
		ULONGLONG tick = 0;

		static constexpr ULONGLONG kFillLifetimeMs = 5000;
	};
	Fill fill;

	// An NPC's looping reload (NPCLoopingReloadFix): the rounds the clip holds once it is done, what Actor::Reload would
	// have filled; 0 without one
	SInt32 loopTarget = 0;

	// Rounds loaded by ammo keys since the actor's weapon section started its sequence or the engine last loaded a round
	// of a looping reload (AmmoKeys)
	SInt32 keyLoads = 0;

	// Attacking ended the player's looping reload, which loaded the round in the hand: the rest of this round's
	// animation (on the skeleton of the view) loads nothing more, until it starts again. Compared by address only
	const BSAnimGroupSequence* loopEndedRound = nullptr;

	// The actor's state, made if it has none; nullptr without a 3rd person Animation
	static ReloadState* Get(Actor* actor);
	// The actor's state if it has one
	static ReloadState* Peek(const Actor* actor);
};
