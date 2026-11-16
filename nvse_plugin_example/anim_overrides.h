#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "GameForms.h"
#include "LambdaVariableContext.h"

class Actor;
class BSAnimGroupSequence;

// One file of an override set
struct AnimPath
{
	// relative to Data\Meshes, lower case, pooled (NiGlobalStringTable): a sequence loaded from it has the same pointer
	// as its name
	std::string_view path;
	bool partialReload = false;
	// _start / _stop: a start or stop animation of a walk or run (MovementTransitions) or of aiming down the sights
	// (AimTransitions), never picked as the group's animation
	bool isStartAnim = false;
	bool isStopAnim = false;
	bool isAmmoSwap = false;
};

// The subfolder of _male / _1stperson a file is in
enum class FolderCondition : UInt8
{
	None, Male, Female, Mod1, Mod2, Mod3, Hurt, Human
};

// Where sets were registered from, as the user knows them: what the kNVSE menu lists, and disables and orders as one
// (OverridePrefs keeps that by name)
struct OverrideOrigin
{
	enum class Kind : UInt8
	{
		ModFolder, // AnimGroupOverride\<mod>, AnimGroupOverride\<mod>\<form ID>
		Json, // a folder a JSON file lists
		Script, // a script command (SetActorAnimationPath, ...)
		Plugin, // another plugin through kNVSE's exports
	};
	Kind kind;
	// what identifies it: the folder relative to AnimGroupOverride (a mod's, a form's in it, a JSON entry's), or
	// "script: <mod of the script>" / "plugin"
	std::string name;
	// the JSON file that listed the folder first, and that entry's priority
	std::string jsonFile;
	int jsonPriority = 0;
	// disabled by the user. Set under the registry's lock, read without it
	std::atomic<bool> userDisabled = false;
	// its place in the user's default order of origins (all equal while the user has not ordered them): a stack is
	// ordered by its form's order of origins (SavedAnims::formRank), then by this, then by the order of registration.
	// Under the registry's lock
	int rank = 0;
};

// An override set: the files registered together for one anim group of one identifier (a form, a mod, or everyone) that
// play under the same conditions: random or ordered variants, and with a reload its partial and ammo swap versions.
// Complete when the registry publishes it and not changed afterwards, but for the atomics (disabled or not, the
// condition once it is compiled, the files the user disabled): the threads that pick animations read sets without a
// lock, and keep pointers to them (they live until Overrides::Clear).
struct SavedAnims
{
	std::vector<AnimPath> anims;
	std::string_view conditionScriptText;
	std::string_view additiveAnimPath;
	// The condition: given at registration, or compiled from the text (Overrides::GetCondition)
	std::atomic<Script*> conditionScript = nullptr;
	// keeps a lambda's variables: the registry's, under its lock
	LambdaVariableContext conditionContext = nullptr;
	const OverrideOrigin* origin = nullptr;
	// its origin's place in the user's order of the origins of the form it is registered for (OverridePrefs::FormPrefs;
	// equal for all while there is none), tried before the origin's rank. Under the registry's lock
	int formRank = 0;
	// when it was registered, or registered again: the order of a stack among the sets of origins of one rank. Under
	// the registry's lock
	UInt32 sequence = 0;
	// per file of anims: disabled by the user (OverridePrefs)
	std::unique_ptr<std::atomic<bool>[]> fileDisabled;
	std::atomic<UInt32> numFilesDisabled = 0;
	FolderCondition folderCondition = FolderCondition::None;
	bool hasOrder = false;
	bool pollCondition = false;
	bool matchBaseGroupId = false;
	bool hasTransitionAnims = false;
	bool hasPartialReload = false;
	bool hasAmmoSwap = false;
	// disabled by a script command, or replaced by a copy with another condition
	std::atomic<bool> disabled = false;
	// its origin is disabled by the user for the form it is registered for (OverridePrefs::FormPrefs)
	std::atomic<bool> formDisabled = false;
	// the condition text failed to compile: never play, whatever enables the set again. Without its condition it would
	// play for every actor the set's identifier covers (global sets: all of them)
	std::atomic<bool> conditionFailed = false;

	bool ContainsAnim(const BSAnimGroupSequence* anim) const;
	bool MatchesFolderCondition(const Actor* actor) const;
	bool IsFileEnabled(const AnimPath& anim) const;
	// Not disabled by a script nor by the user (its origin, its origin for its form, or every one of its files)
	bool IsEnabled() const;
};

// The registry of override sets: per point of view, per identifier (a form's ID, a mod index, or everyone), per full anim
// group ID a stack of sets, the one registered or enabled last on top.
// Changes take one lock for a whole batch of files and bump a generation, which is what tells the per-actor state
// (ActorOverrides) that the sets it resolved are out of date. Lookups do not lock: a bit per group says whether any set
// exists for it (most groups the engine asks about every frame have none), and the sets that can apply to an actor are
// collected once per group under the shared lock.
namespace Overrides
{
	constexpr UInt32 kEveryone = 0xFF;
	constexpr UInt16 kInvalidGroupId = 0xFFFF;

	// Who a batch of files is for and what its sets play under
	struct Source
	{
		// a form ID (actor reference, weapon, base form, race), or with isModIndex a mod index (kEveryone: any actor)
		UInt32 identifier = kEveryone;
		bool isModIndex = true;
		Script* conditionScript = nullptr;
		std::string_view conditionText;
		bool pollCondition = false;
		bool matchBaseGroupId = false;
		// A file that is registered already keeps the condition of its set, whatever this source's is (the override
		// folders: JSON files list a folder for the same form more than once, with another condition or none, and its
		// animations have always played under the first one's). Script commands replace the condition
		bool keepCondition = false;
		// where the files come from: the sets made from them keep it
		const OverrideOrigin* origin = nullptr;
	};

	// The origin of that name, made the first time it is asked for (with the kind and JSON file given then). Lives
	// until the game closes: the user's choices for it outlast kNVSEReset
	const OverrideOrigin* GetOrigin(OverrideOrigin::Kind kind, std::string_view name, std::string_view jsonFile = {}, int jsonPriority = 0);
	// Every origin, in the order they were made
	std::vector<const OverrideOrigin*> GetOrigins();
	// Applies the user's choices (OverridePrefs) to the origins and sets: what is disabled, the order of the stacks
	void ApplyPrefs();
	// What the user's choices for a form are kept by (OverridePrefs::FormPrefs): the same across load orders.
	// "<plugin>:<form ID without the mod index>" (lower case), a mod's name for its actors, "everyone", the full form ID
	// for a form made at run time
	std::string FormKey(UInt32 identifier, bool isModIndex);

	struct File
	{
		// relative to Data\Meshes, any case
		std::string_view path;
		bool firstPerson = false;
	};

	// Registers the files as one batch: those of the same group and subfolder condition become the variants of one set
	// (never of a set registered before). A file that is registered already is enabled again and its set moved to the top
	// of its stack; if the source's condition differs and it does not keep conditions, a copy of the set with that
	// condition takes its place (and the old one is disabled, see SavedAnims). With enable false the sets holding
	// the files are disabled instead. Returns how many of the files it applied
	size_t Register(const Source& source, std::span<const File> files, bool enable = true);
	// Disables every set registered for the form. Whether there were any
	bool DisableForm(UInt32 formId);
	// Registers the sets of one form for another, in the same order. Whether there were any
	bool CopyForm(UInt32 fromFormId, UInt32 toFormId);
	// Drops every set (kNVSEReset). The per-actor states must have dropped what they kept of the sets
	void Clear();

	// Whether any set was ever registered for the group in that point of view. Lock-free
	bool HasGroup(bool firstPerson, UInt16 groupId);
	UInt32 Generation();

	// A set as the registry has it
	struct SetView
	{
		const SavedAnims* set;
		// a form ID, or with isModIndex a mod index (kEveryone: any actor)
		UInt32 identifier;
		bool isModIndex;
		bool firstPerson;
		UInt16 groupId;
		// 0: the top of its stack, tried first
		UInt32 stackPosition;
		UInt32 stackSize;
	};
	// Every set, under the registry's shared lock: the visitor must not change the registry
	void ForEachSet(const std::function<void(const SetView&)>& visitor);

	// What went wrong with the override files, each once, for the kNVSE menu. Not logged: the callers log what they
	// report
	struct Problem
	{
		enum class Kind : UInt8
		{
			Files, // JSON files, folders, forms they name
			Condition, // conditions that do not compile
			Load, // KFs that do not load
		};
		Kind kind;
		std::string text;

		bool operator==(const Problem&) const = default;
	};
	void ReportProblem(Problem::Kind kind, const std::string& text);
	std::vector<Problem> GetProblems();

	// What decides the identifiers an actor's sets are registered under
	struct Identity
	{
		UInt32 actor = 0;
		UInt32 weapon = 0;
		UInt32 baseForm = 0;
		UInt32 race = 0;
		UInt32 actorBase = 0;

		bool operator==(const Identity&) const = default;
	};
	Identity GetIdentity(Actor* actor);
	// Appends the sets of the group that may apply to the actor, in the order they are tried: the stacks of its
	// reference, weapon, base form, race and actor base (top first), then those of the mods these forms are from, then
	// everyone's. Their conditions are not evaluated
	void CollectCandidates(const Identity& identity, bool firstPerson, UInt16 groupId, std::vector<SavedAnims*>& out);
	// Appends the files of every set registered for the form that the actor's sex and kind allow
	void CollectFormPaths(UInt32 formId, bool firstPerson, const Actor* actor, std::vector<const char*>& out);

	// The set's condition script: null when it has none, or its text does not compile (conditionFailed). Text conditions
	// are compiled on the main thread once the files are loaded (CompileConditions); a set asked before that compiles
	// its own, on the asking thread
	Script* GetCondition(SavedAnims& set);
	// Compiles the conditions registered as text. Main thread; does nothing when none are waiting
	void CompileConditions();

	// Files named <name>__<custom anim group>.kf are not part of any set: RegisterCustomAnimGroup scripts run while an
	// animation from the same folder plays. basePath: the path up to _male / _1stperson
	bool HasCustomGroupPaths(std::string_view basePath);
	std::vector<std::string_view> GetCustomGroupPaths(std::string_view basePath);
}
