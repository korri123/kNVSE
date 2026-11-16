#pragma once

#include <span>
#include <vector>

#include "anim_overrides.h"
#include "GameProcess.h"
#include "kf_loader.h"
#include "NiNodes.h"
#include "transition_anims.h"

// Per-Animation (one actor skeleton; the player has two) state for the override animations. It lives in the
// kAnimationExtraData of the node that the Animation's controller manager animates. It needs no lock because only one
// thread at a time picks an actor's animations:
//  - the main thread for the player and for scripts (which run before the AI threads start),
//  - AI linear task thread 1 for other actors,
//  - the thread that loads an actor's 3D, before anything else sees the actor.
struct ActorOverrides
{
	// An override KF as loaded for this Animation: its sequence (the KF's own, or a clone if another Animation already
	// uses that) and the AnimSequenceSingle that answers the engine's lookups. Destroying the AnimSequenceSingle
	// releases its references to the sequence and the KF model, as for the engine's own (~AnimSequenceSingle).
	struct Loaded
	{
		const char* path;
		NiPointer<BSAnimGroupSequence> sequence;
		AnimSequenceBase* base;
	};
	// The file of a multi-file set that plays next. Kept until it has played, so that the engine's questions about the
	// group between two plays (is it loaded, its speed, its keys) are answered for that file.
	struct Variant
	{
		SavedAnims* set;
		const AnimPath* next;
		UInt32 nextIndex;
		// How often the set has played (for ordered sets, the file's turn).
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
	// The actor's ID, recorded when the Animation was first seen. The actor of a deleted reference is freed before its
	// Animation, so it cannot be read from the Animation later.
	UInt32 actorId = 0;
	std::vector<Loaded> loaded;
	// KFs that did not load for this skeleton. Not tried again, and skipped when a set's file is chosen.
	std::vector<const char*> failed;
	// The sets that may apply to the actor, per group asked about (Overrides::CollectCandidates). Valid for one
	// generation of the registry and one identity of the actor (the weapon changes).
	UInt32 generation = 0;
	Overrides::Identity identity;
	std::vector<UInt16> memoGroups;
	std::vector<Memo> memos;
	std::vector<SavedAnims*> candidates;
	std::vector<Variant> variants;
	// The variant that the actor's aim (attack, throw) played last. Its up and down groups play the same one.
	UInt32 tripleIndex = 0;
	// Sets with pollCondition that were tried for the group playing in its section (Overrides::UpdatePolled).
	std::vector<Polled> polled;
	std::vector<Polled> polledScratch;
	bool polledListed = false;
	// Sequences queued by QueueNextAnim.
	std::vector<Queued> queued;
	// The KFs of the sets of the player's weapon, loaded ahead of their first play.
	KFLoader::Preload preload;

	~ActorOverrides() { Release(); }

	// The Animation's state, created on first use. Null if the Animation has no controller manager attached to a node.
	static ActorOverrides* Get(AnimData* animData);
	// Like Get, but null if nothing has created the state yet.
	static ActorOverrides* Peek(AnimData* animData);

	// Releases everything. Called when the Animation is deleted (Overrides::OnAnimDataDelete) or its node is.
	void Release();
	// Like Release, but first removes the loaded sequences from the controller manager (kNVSEReset).
	void Unload();

	Loaded* FindLoaded(const char* path);
	bool HasFailed(const char* path) const;
	// Drops what was resolved for another generation of the registry or another identity of the actor, and starts
	// reading the KFs of the player's weapon ahead of time.
	void Refresh(Actor* actor, bool firstPerson);
	std::span<SavedAnims* const> GetCandidates(Actor* actor, bool firstPerson, UInt16 groupId);
	void Track(SavedAnims& set, UInt16 groupId);
	void UpdatePolled();
};

namespace Overrides
{
	// Returns the override set that the group's animation comes from for the actor, or null: the first of the candidate
	// sets (Overrides::CollectCandidates) whose conditions pass. With a transition, the first set that has a start or
	// stop animation of that kind for the group (TransitionAnims). Sets with only start and stop animations are skipped
	// for the group's own animation, so they do not hide the walk of a set below them.
	// On the background 3D-loading threads, sets with a script condition are skipped, because scripts only run where the
	// engine itself runs conditions.
	SavedAnims* Find(UInt16 groupId, AnimData* animData, TransitionKind transition = TransitionKind{});

	// What Find decides about a candidate set.
	enum class Verdict : UInt8
	{
		Plays,
		Disabled, // by a script or by the user (SavedAnims::IsEnabled)
		FolderCondition, // the actor does not match the set's subfolder (male, female, mod1, hurt, ...)
		NoTransition, // no start or stop animation of the requested kind, or only start and stop animations
		OtherIdle, // a special idle is playing and the set has no file with that idle's file name
		LoaderThread, // has a script condition, which is not run on a loader thread
		ConditionFailed, // the condition did not compile
		ConditionFalse,
		NoFiles, // contains only an additive animation
	};
	struct Candidate
	{
		SavedAnims* set;
		Verdict verdict;
	};
	// Lists every set that may apply to the actor's group, in the order Find tries them, with Find's verdict on each.
	// The first set that plays is the one Find returns. Runs the conditions. Main thread.
	std::vector<Candidate> Explain(UInt16 groupId, AnimData* animData, TransitionKind transition = TransitionKind{});

	struct LoadedAnim
	{
		BSAnimGroupSequence* sequence = nullptr;
		AnimSequenceBase* base = nullptr;

		explicit operator bool() const { return sequence != nullptr; }
	};
	// Loads the KF as a sequence of the Animation's controller manager on first use. path is pooled.
	LoadedAnim Load(std::string_view path, AnimData* animData);
	// Loads one file of the set, preparing it if it is a start or stop animation.
	LoadedAnim Load(SavedAnims& set, const AnimPath& animPath, AnimData* animData);
	// Returns the file of the set that the actor plays next for the group (random or ordered variants, the reload's
	// kind, the special idle that is playing), or null if none can play.
	const AnimPath* GetNextPath(SavedAnims& set, UInt16 groupId, AnimData* animData);
	// Loads that file. The engine's questions about the group are answered from it.
	LoadedAnim LoadNext(SavedAnims& set, UInt16 groupId, AnimData* animData);
	// Prepares that file for playing: starts its text key handling and its additive animation. The next play picks
	// a file again.
	BSAnimGroupSequence* Play(SavedAnims& set, UInt16 groupId, AnimData* animData);
	// Called for sequences started by kNVSE itself rather than through Play (BlendFixes' cross fades to another variant
	// of a group, which take it from LoadNext). If the sequence is an override it counts as played, so the next play
	// picks again and the up and down groups started after it follow its variant.
	void Played(AnimData* animData, const BSAnimGroupSequence* sequence);

	// Called from Animation::~Animation.
	void OnAnimDataDelete(AnimData* animData);
	// Plays the groups whose polled sets' conditions changed. Main thread, before the AI threads start.
	void UpdatePolled();
	// kNVSEReset: every Animation releases its override sequences and its references to sets. Main thread.
	void UnloadAll();
	// Checks the player's weapon every frame so that its KFs are read while it is still holstered (or the Pip-Boy is
	// open) rather than on the first frame of the draw. Main thread.
	void UpdatePlayerPreload();

	void WriteHooks();
}
