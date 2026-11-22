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
	// Relative to Data\Meshes, lower case. Pooled (NiGlobalStringTable), so a sequence loaded from it has the same
	// pointer as its name.
	std::string_view path;
	bool partialReload = false;
	// _start / _stop: a start or stop animation of a walk or run (MovementTransitions) or of aiming down the sights
	// (AimTransitions). Never picked as the group's own animation.
	bool isStartAnim = false;
	bool isStopAnim = false;
	bool isAmmoSwap = false;
};

// The subfolder of _male / _1stperson that a file is in.
enum class FolderCondition : UInt8
{
	None, Male, Female, Mod1, Mod2, Mod3, Hurt, Human
};

// Where sets were registered from, as the user knows them. The kNVSE menu lists origins and disables or orders each as
// one unit; OverridePrefs stores those choices by name.
struct OverrideOrigin
{
	enum class Kind : UInt8
	{
		ModFolder, // AnimGroupOverride\<mod> or AnimGroupOverride\<mod>\<form ID>
		Json, // a folder listed by a JSON file
		Script, // a script command (SetActorAnimationPath, ...)
		Plugin, // another plugin, through kNVSE's exports
	};
	Kind kind;
	// Identifies the origin: the folder relative to AnimGroupOverride (a mod's, a form's within it, or a JSON entry's),
	// "script: <mod of the script>" or "plugin".
	std::string name;
	// The JSON file that first listed the folder, and that entry's priority.
	std::string jsonFile;
	int jsonPriority = 0;
	// Disabled by the user. Written under the registry's lock, read without it.
	std::atomic<bool> userDisabled = false;
	// Position in the user's default order of origins (all equal until the user orders them). A stack is ordered by its
	// form's order of origins (SavedAnims::formRank), then by this, then by registration order. Under the registry's lock.
	int rank = 0;
};

// An override set: the files registered together for one anim group of one identifier (a form, a mod or everyone) that
// play under the same conditions. They are random or ordered variants and, with a reload, its partial and ammo swap
// versions. A set is complete when the registry publishes it and does not change afterwards, except for the atomics
// (disabled flags, the compiled condition, per-file disabled flags). Threads that pick animations read sets without a
// lock and keep pointers to them, which stay valid until Overrides::Clear.
struct SavedAnims
{
	std::vector<AnimPath> anims;
	std::string_view conditionScriptText;
	std::string_view additiveAnimPath;
	// The condition: given at registration, or compiled from the text (Overrides::GetCondition).
	std::atomic<Script*> conditionScript = nullptr;
	// Keeps the variables of a lambda condition alive. Under the registry's lock.
	LambdaVariableContext conditionContext = nullptr;
	const OverrideOrigin* origin = nullptr;
	// Position of the origin in the user's order of origins for the form this set is registered for
	// (OverridePrefs::FormPrefs; all equal if there is none). Tried before the origin's rank. Under the registry's lock.
	int formRank = 0;
	// Registration counter, bumped when the set is registered or registered again. Orders a stack among sets of origins
	// of equal rank. Under the registry's lock.
	UInt32 sequence = 0;
	// Per file of anims: disabled by the user (OverridePrefs).
	std::unique_ptr<std::atomic<bool>[]> fileDisabled;
	std::atomic<UInt32> numFilesDisabled = 0;
	FolderCondition folderCondition = FolderCondition::None;
	bool hasOrder = false;
	bool pollCondition = false;
	bool matchBaseGroupId = false;
	bool hasTransitionAnims = false;
	bool hasPartialReload = false;
	bool hasAmmoSwap = false;
	// Disabled by a script command, or replaced by a copy with another condition.
	std::atomic<bool> disabled = false;
	// The set's origin is disabled by the user for the form the set is registered for (OverridePrefs::FormPrefs).
	std::atomic<bool> formDisabled = false;
	// The condition text failed to compile. The set never plays, because without its condition it would play for every
	// actor its identifier covers (for global sets, all actors).
	std::atomic<bool> conditionFailed = false;

	bool ContainsAnim(const BSAnimGroupSequence* anim) const;
	bool MatchesFolderCondition(const Actor* actor) const;
	bool IsFileEnabled(const AnimPath& anim) const;
	// True unless disabled by a script or by the user (its origin, its origin for its form, or all of its files).
	bool IsEnabled() const;
};

// The registry of override sets. Sets are kept per point of view, per identifier (a form ID, a mod index or everyone)
// and per full anim group ID, as a stack with the set registered or enabled last on top.
// Changes take one lock for a whole batch of files and bump a generation, which tells the per-actor state
// (ActorOverrides) that the sets it resolved are out of date. Lookups do not lock: a bit per group says whether any set
// exists for it (most groups the engine asks about every frame have none), and the sets that can apply to an actor are
// collected once per group under the shared lock.
namespace Overrides
{
	constexpr UInt32 kEveryone = 0xFF;
	constexpr UInt16 kInvalidGroupId = 0xFFFF;

	// Who a batch of files is for and the conditions its sets play under.
	struct Source
	{
		// A form ID (actor reference, weapon, base form, race) or, with isModIndex, a mod index (kEveryone: any actor).
		UInt32 identifier = kEveryone;
		bool isModIndex = true;
		Script* conditionScript = nullptr;
		std::string_view conditionText;
		bool pollCondition = false;
		bool matchBaseGroupId = false;
		// A file that is already registered keeps the condition of its set, whatever this source's condition is. Used for
		// the override folders: JSON files can list a folder for the same form several times with different conditions,
		// and the first one wins. Script commands replace the condition instead.
		bool keepCondition = false;
		// Where the files come from. The sets made from them keep it.
		const OverrideOrigin* origin = nullptr;
	};

	// Returns the origin of that name, creating it on first use (with the kind and JSON file given then). Lives until
	// the game closes, so the user's choices for it survive kNVSEReset.
	const OverrideOrigin* GetOrigin(OverrideOrigin::Kind kind, std::string_view name, std::string_view jsonFile = {}, int jsonPriority = 0);
	// Every origin, in creation order.
	std::vector<const OverrideOrigin*> GetOrigins();
	// Applies the user's choices (OverridePrefs) to the origins and sets: disabled state and stack order.
	void ApplyPrefs();
	// The key the user's choices for a form are stored under (OverridePrefs::FormPrefs), stable across load orders:
	// "<plugin>:<form ID without the mod index>" (lower case), a mod's name for its actors, "everyone", or the full
	// form ID for a form created at run time.
	std::string FormKey(UInt32 identifier, bool isModIndex);

	struct File
	{
		// Relative to Data\Meshes, any case.
		std::string_view path;
		bool firstPerson = false;
	};

	// Registers the files as one batch. Files of the same group and subfolder condition become the variants of one set
	// (never of a set registered earlier). A file that is already registered is enabled again and its set moves to the
	// top of its stack. If the source's condition differs and it does not keep conditions, a copy of the set with that
	// condition takes its place and the old set is disabled (see SavedAnims). With enable false, the sets holding the
	// files are disabled instead. Returns the number of files applied.
	size_t Register(const Source& source, std::span<const File> files, bool enable = true);
	// Disables every set registered for the form. Returns true if there were any.
	bool DisableForm(UInt32 formId);
	// Registers the sets of one form for another, in the same order. Returns true if there were any.
	bool CopyForm(UInt32 fromFormId, UInt32 toFormId);
	// Drops every set (kNVSEReset). The per-actor states must already have released their pointers to the sets.
	void Clear();

	// True if any set was ever registered for the group in that point of view. Lock-free.
	bool HasGroup(bool firstPerson, UInt16 groupId);
	// True if a set with pollCondition was ever registered. Lock-free.
	bool HasPolledSets();
	UInt32 Generation();

	// A read-only view of a set as the registry holds it.
	struct SetView
	{
		const SavedAnims* set;
		// A form ID or, with isModIndex, a mod index (kEveryone: any actor).
		UInt32 identifier;
		bool isModIndex;
		bool firstPerson;
		UInt16 groupId;
		// 0 is the top of the stack, which is tried first.
		UInt32 stackPosition;
		UInt32 stackSize;
	};
	// Calls the visitor for every set under the registry's shared lock. The visitor must not change the registry.
	void ForEachSet(const std::function<void(const SetView&)>& visitor);

	// Problems found in the override files, reported once each for the kNVSE menu. Not logged here; callers log what
	// they report.
	struct Problem
	{
		enum class Kind : UInt8
		{
			Files, // JSON files, folders and forms they name
			Condition, // conditions that do not compile
			Load, // KFs that do not load
		};
		Kind kind;
		std::string text;

		bool operator==(const Problem&) const = default;
	};
	void ReportProblem(Problem::Kind kind, const std::string& text);
	std::vector<Problem> GetProblems();

	// The forms an actor's sets are looked up under.
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
	// reference, weapon, base form, race and actor base (top first), then those of the mods these forms come from, then
	// everyone's. Conditions are not evaluated here.
	void CollectCandidates(const Identity& identity, bool firstPerson, UInt16 groupId, std::vector<SavedAnims*>& out);
	// Appends the files of every set registered for the form that the actor's sex and kind allow.
	void CollectFormPaths(UInt32 formId, bool firstPerson, const Actor* actor, std::vector<const char*>& out);

	// Returns the set's condition script, or null if it has none or its text does not compile (conditionFailed). Text
	// conditions are compiled on the main thread once the files are loaded (CompileConditions). A set queried before
	// that compiles its own condition on the calling thread.
	Script* GetCondition(SavedAnims& set);
	// Compiles the conditions registered as text. Main thread. Does nothing if none are waiting.
	void CompileConditions();

	// Files named <name>__<custom anim group>.kf belong to no set. They are played by RegisterCustomAnimGroup scripts
	// while an animation from the same folder plays. basePath is the path up to _male / _1stperson.
	bool HasCustomGroupPaths(std::string_view basePath);
	std::vector<std::string_view> GetCustomGroupPaths(std::string_view basePath);
}
