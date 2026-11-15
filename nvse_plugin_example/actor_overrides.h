#pragma once

#include <span>
#include <vector>

#include "anim_overrides.h"
#include "GameProcess.h"
#include "kf_loader.h"
#include "NiNodes.h"
#include "transition_anims.h"

// What kNVSE keeps of the override animations of one Animation (an actor's skeleton; the player has two), inside the
// kAnimationExtraData on the node its controller manager animates. Like the rest of that data it needs no lock: one
// thread at a time picks an actor's animations (the main thread for the player and for scripts, which run before the AI
// threads start; AI linear task thread 1 for other actors; the thread that loads an actor's 3D before anything else
// sees it).
// It replaced maps shared by every actor behind two locks: the loaded sequences by (path, Animation), a cache of each
// thread's lookups thrown away every frame, the order counters and the polled sets.
struct ActorOverrides
{
	// An override KF as loaded for this Animation: its sequence (the KF's own, or a clone when another Animation has
	// that) and the AnimSequenceSingle the engine's lookups are answered with. Destroying that gives back its reference
	// to the sequence and the use of the KF model, as it does for the engine's own (~AnimSequenceSingle).
	struct Loaded
	{
		const char* path;
		NiPointer<BSAnimGroupSequence> sequence;
		AnimSequenceBase* base;
	};
	// The file of a set with several that plays next, kept until it has played: what the engine asks about a group
	// between two plays (is it loaded, its speed, its keys) is answered for that file
	struct Variant
	{
		SavedAnims* set;
		const AnimPath* next;
		UInt32 nextIndex;
		// how often the set has played (ordered sets: the file's turn)
		UInt32 played;
	};
	struct Polled
	{
		SavedAnims* set;
		UInt16 groupId;
	};
	struct Queued
	{
		UInt16 groupId;
		NiPointer<BSAnimGroupSequence> sequence;
	};
	struct Memo
	{
		UInt32 begin;
		UInt32 count;
	};

	AnimData* animData = nullptr;
	// the actor's, from when the Animation was first seen: the actor of a deleted reference is freed before its Animation
	UInt32 actorId = 0;
	std::vector<Loaded> loaded;
	// KFs that did not load for this skeleton: not tried again, and passed over when a set's file is chosen
	std::vector<const char*> failed;
	// The sets that may apply to the actor per group asked about (Overrides::CollectCandidates), valid for one
	// generation of the registry and one identity of the actor (its weapon changes)
	UInt32 generation = 0;
	Overrides::Identity identity;
	std::vector<UInt16> memoGroups;
	std::vector<Memo> memos;
	std::vector<SavedAnims*> candidates;
	std::vector<Variant> variants;
	// The variant the actor's aim (attack, throw) played last: its up and down groups play the same one
	UInt32 tripleIndex = 0;
	// Sets with pollCondition that were tried for the group playing in its section (Overrides::UpdatePolled)
	std::vector<Polled> polled;
	std::vector<Polled> polledScratch;
	bool polledListed = false;
	// QueueNextAnim
	std::vector<Queued> queued;
	// The KFs of the sets of the player's weapon, loaded ahead of their first play
	KFLoader::Preload preload;

	~ActorOverrides() { Release(); }

	// The Animation's state, made on first use. Null without a controller manager attached to a node
	static ActorOverrides* Get(AnimData* animData);
	// Null also when nothing asked for it yet
	static ActorOverrides* Peek(AnimData* animData);

	// Gives back everything: the Animation is going (Overrides::OnAnimDataDelete), or its node is
	void Release();
	// Takes the loaded sequences out of the controller manager first (kNVSEReset)
	void Unload();

	Loaded* FindLoaded(const char* path);
	bool HasFailed(const char* path) const;
	// Drops what was resolved for another generation of the registry or another identity of the actor, and reads the
	// KFs of the player's weapon ahead
	void Refresh(Actor* actor, bool firstPerson);
	std::span<SavedAnims* const> GetCandidates(Actor* actor, bool firstPerson, UInt16 groupId);
	void Track(SavedAnims& set, UInt16 groupId);
	void UpdatePolled();
};

namespace Overrides
{
	// The override set the group's animation comes from for the actor, or null: the first of the sets registered for it
	// (Overrides::CollectCandidates) whose conditions pass. With a transition, the first that has such a start or stop
	// animation of the group (TransitionAnims). A set with only start and stop animations is passed over for the
	// group's own animation, so it does not hide the walk of a set below it.
	// On the threads that load 3D in the background, sets with a script condition are passed over: scripts only run
	// where the engine runs conditions itself
	SavedAnims* Find(UInt16 groupId, AnimData* animData, TransitionKind transition = TransitionKind{});

	// What Find makes of a candidate set
	enum class Verdict : UInt8
	{
		Plays,
		Disabled, // by a script or by the user (SavedAnims::IsEnabled)
		FolderCondition, // the actor is not of the set's subfolder (male, female, mod1, hurt, ...)
		NoTransition, // has no start or stop animation of the kind asked for, or only those
		OtherIdle, // a special idle plays whose file name it has no file of
		LoaderThread, // has a script condition, which is not run on a loader thread
		ConditionFailed, // its condition did not compile
		ConditionFalse,
		NoFiles, // only an additive animation
	};
	struct Candidate
	{
		SavedAnims* set;
		Verdict verdict;
	};
	// Every set that may apply to the actor's group in the order Find tries them, with what Find makes of each: the
	// first that plays is the one it returns. Runs their conditions. Main thread
	std::vector<Candidate> Explain(UInt16 groupId, AnimData* animData, TransitionKind transition = TransitionKind{});

	struct LoadedAnim
	{
		BSAnimGroupSequence* sequence = nullptr;
		AnimSequenceBase* base = nullptr;

		explicit operator bool() const { return sequence != nullptr; }
	};
	// The KF as a sequence of the Animation's controller manager, loaded on first use. path: pooled
	LoadedAnim Load(std::string_view path, AnimData* animData);
	// One file of the set, prepared when it is a start or stop animation
	LoadedAnim Load(SavedAnims& set, const AnimPath& animPath, AnimData* animData);
	// The file of the set that the actor plays next for the group (random or ordered variants, the reload's kind, the
	// special idle that is playing), or null when none can play
	const AnimPath* GetNextPath(SavedAnims& set, UInt16 groupId, AnimData* animData);
	// That file loaded: what the engine's questions about a group are answered with
	LoadedAnim LoadNext(SavedAnims& set, UInt16 groupId, AnimData* animData);
	// That file about to play: its text key handling starts, its additive animation plays, and the next play picks
	// anew
	BSAnimGroupSequence* Play(SavedAnims& set, UInt16 groupId, AnimData* animData);
	// The sequence was started by kNVSE itself, not through Play (BlendFixes' cross fades to another variant of a
	// group, which take it from LoadNext): if it is an override it counts as played, so the next play picks anew and
	// the up and down groups started after it follow its variant
	void Played(AnimData* animData, const BSAnimGroupSequence* sequence);

	// Animation::~Animation
	void OnAnimDataDelete(AnimData* animData);
	// Plays the groups whose polled sets' conditions changed. Main thread, before the AI threads start
	void UpdatePolled();
	// kNVSEReset: every Animation gives up its override sequences and what it kept of the sets. Main thread
	void UnloadAll();
	// The player's weapon is looked at every frame, so that its KFs are read while it is still in the holster (or the
	// Pip-Boy still open) and not from the first frame of the draw. Main thread
	void UpdatePlayerPreload();

	void WriteHooks();
}
