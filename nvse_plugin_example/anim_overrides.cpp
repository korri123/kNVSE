#include "anim_overrides.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <unordered_map>

#include "commands_animation.h"
#include "file_animations.h"
#include "GameObjects.h"
#include "string_view_util.h"
#include "override_prefs.h"
#include "transition_anims.h"
#include "utility.h"

namespace
{
	struct SetStack
	{
		// the set tried last first: by the rank of their origins, then in the order they were registered (or registered
		// again: on top)
		std::vector<std::unique_ptr<SavedAnims>> sets;
	};

	struct IdentifierOverrides
	{
		std::unordered_map<UInt16, SetStack> stacks;
	};

	using OverrideMap = std::unordered_map<UInt32, IdentifierOverrides>;

	constexpr auto kForms = 0;
	constexpr auto kModIndices = 1;
	// [first person][kForms / kModIndices]
	OverrideMap s_maps[2][2];
	std::shared_mutex s_mutex;
	std::atomic<UInt32> s_generation = 1;
	// a bit per full group ID, set under the lock and never cleared but by Clear
	std::atomic<UInt32> s_groups[2][0x10000 / 32];
	// sets replaced by one with another condition (the same files registered again): threads that pick animations may
	// still have them
	std::vector<std::unique_ptr<SavedAnims>> s_retiredSets;
	// base path -> pooled paths
	std::unordered_map<std::string, std::vector<std::string_view>, transparent_string_hash, std::equal_to<>> s_customGroupPaths;
	// every origin made, never freed: sets point to them, and the user's choices are by origin
	std::vector<std::unique_ptr<OverrideOrigin>> s_origins;
	// by lower case name
	std::unordered_map<std::string, OverrideOrigin*> s_originsByName;
	// counts registrations: SavedAnims::sequence
	UInt32 s_sequence = 0;

	std::mutex s_problemsMutex;
	std::vector<Overrides::Problem> s_problems;

	// one thread compiles at a time (CompileConditionScript keeps the scripts by their text)
	std::mutex s_compileMutex;
	std::atomic<bool> s_conditionsWaiting = false;

	int RankOf(const SavedAnims& set)
	{
		return set.origin ? set.origin->rank : 0;
	}

	bool IsTriedBefore(const std::unique_ptr<SavedAnims>& set, const std::unique_ptr<SavedAnims>& other)
	{
		const auto rank = RankOf(*set);
		const auto otherRank = RankOf(*other);
		return rank != otherRank ? rank > otherRank : set->sequence > other->sequence;
	}

	void Order(SetStack& stack)
	{
		std::ranges::sort(stack.sets, [](const auto& set, const auto& other) { return IsTriedBefore(other, set); });
	}

	// Whether the set would be tried first of the sets of its rank or below: registering it again moves nothing
	bool IsOnTopOfItsRank(const SetStack& stack, const SavedAnims& set)
	{
		const auto rank = RankOf(set);
		const auto iter = std::ranges::find(stack.sets, &set, &std::unique_ptr<SavedAnims>::get);
		return std::all_of(std::next(iter), stack.sets.end(), [&](const std::unique_ptr<SavedAnims>& above) { return RankOf(*above) > rank; });
	}

	void ApplyFilePrefs(SavedAnims& set, const OverridePrefs::Prefs& prefs)
	{
		UInt32 numDisabled = 0;
		for (size_t i = 0; i < set.anims.size(); ++i)
		{
			const bool disabled = prefs.disabledFiles.contains(std::string(set.anims[i].path));
			set.fileDisabled[i].store(disabled, std::memory_order_relaxed);
			numDisabled += disabled;
		}
		set.numFilesDisabled.store(numDisabled, std::memory_order_relaxed);
	}

	void SetGroup(bool firstPerson, UInt16 groupId)
	{
		s_groups[firstPerson][groupId / 32].fetch_or(1u << groupId % 32, std::memory_order_relaxed);
	}

	SetStack* FindStack(OverrideMap& map, UInt32 identifier, UInt16 groupId)
	{
		const auto identifierIter = map.find(identifier);
		if (identifierIter == map.end())
			return nullptr;
		const auto stackIter = identifierIter->second.stacks.find(groupId);
		return stackIter != identifierIter->second.stacks.end() ? &stackIter->second : nullptr;
	}

	FolderCondition GetFolderCondition(std::string_view path)
	{
		constexpr std::pair<std::string_view, FolderCondition> kFolders[] = {
			{ R"(\mod1\)", FolderCondition::Mod1 }, { R"(\mod2\)", FolderCondition::Mod2 }, { R"(\mod3\)", FolderCondition::Mod3 },
			{ R"(\hurt\)", FolderCondition::Hurt }, { R"(\human\)", FolderCondition::Human },
			{ R"(\male\)", FolderCondition::Male }, { R"(\female\)", FolderCondition::Female },
		};
		for (const auto& [folder, condition] : kFolders)
		{
			if (path.find(folder) != std::string_view::npos)
				return condition;
		}
		return FolderCondition::None;
	}

	bool RegisterCustomGroupPath(std::string_view path)
	{
		if (ExtractCustomAnimGroupName(path).empty())
			return false;
		const auto basePath = GetAnimBasePath(path);
		if (basePath.empty())
			return false;
		auto& paths = s_customGroupPaths[std::string(basePath)];
		if (std::ranges::find(paths, path.data(), [](std::string_view registered) { return registered.data(); }) == paths.end())
			paths.push_back(path);
		return true;
	}

	// What the names of a set's files say, once the set has all of them
	void Complete(SavedAnims& set)
	{
		for (auto& anim : set.anims)
		{
			const auto fileStem = sv::get_file_stem(anim.path);
			if (sv::contains_ci(fileStem, "_order_"))
				set.hasOrder = true;
			if (sv::contains_ci(fileStem, "_partial"))
			{
				anim.partialReload = true;
				set.hasPartialReload = true;
			}
			if (sv::contains_ci(fileStem, "_ammo_swap"))
			{
				anim.isAmmoSwap = true;
				set.hasAmmoSwap = true;
			}
		}
		if (set.hasOrder)
			std::ranges::sort(set.anims, {}, &AnimPath::path);
		set.fileDisabled = std::make_unique<std::atomic<bool>[]>(set.anims.size());
		UInt32 numDisabled = 0;
		for (size_t i = 0; i < set.anims.size(); ++i)
		{
			const bool disabled = OverridePrefs::IsFileDisabled(set.anims[i].path);
			set.fileDisabled[i].store(disabled, std::memory_order_relaxed);
			numDisabled += disabled;
		}
		set.numFilesDisabled.store(numDisabled, std::memory_order_relaxed);
	}

	// A set gets its script once: at registration, or when its text is compiled
	void SetCondition(SavedAnims& set, Script* script)
	{
		set.conditionContext = script;
		set.conditionScript.store(script, std::memory_order_release);
	}

	// Whether the set plays under the source's condition. A text decides by itself (its script is compiled from it)
	bool HasCondition(const SavedAnims& set, const Overrides::Source& source)
	{
		if (!set.conditionScriptText.empty() || !source.conditionText.empty())
			return set.conditionScriptText == source.conditionText;
		return set.conditionScript.load(std::memory_order_relaxed) == source.conditionScript;
	}

	// The same files, played the same way: a copy of the set with another condition
	bool IsCopyOf(const SavedAnims& copy, const SavedAnims& set)
	{
		return copy.additiveAnimPath.data() == set.additiveAnimPath.data() && copy.folderCondition == set.folderCondition
			&& copy.pollCondition == set.pollCondition && copy.matchBaseGroupId == set.matchBaseGroupId
			&& std::ranges::equal(copy.anims, set.anims, {}, [](const AnimPath& animPath) { return animPath.path.data(); },
				[](const AnimPath& animPath) { return animPath.path.data(); });
	}

	// The set with another condition, in its place in the stack. A copy retired before with that condition comes back:
	// the same files registered by turns with conditions that differ (a lambda is a script per calling reference) would
	// otherwise retire a set on every call
	void ReplaceCondition(std::unique_ptr<SavedAnims>& set, const Overrides::Source& source)
	{
		std::unique_ptr<SavedAnims> replacement;
		if (const auto retired = std::ranges::find_if(s_retiredSets, [&](const std::unique_ptr<SavedAnims>& copy)
			{
				return HasCondition(*copy, source) && IsCopyOf(*copy, *set);
			}); retired != s_retiredSets.end())
		{
			replacement = std::move(*retired);
			s_retiredSets.erase(retired);
		}
		else
		{
			replacement = std::make_unique<SavedAnims>();
			replacement->anims = set->anims;
			replacement->additiveAnimPath = set->additiveAnimPath;
			replacement->folderCondition = set->folderCondition;
			replacement->hasOrder = set->hasOrder;
			replacement->pollCondition = set->pollCondition;
			replacement->matchBaseGroupId = set->matchBaseGroupId;
			replacement->hasTransitionAnims = set->hasTransitionAnims;
			replacement->hasPartialReload = set->hasPartialReload;
			replacement->hasAmmoSwap = set->hasAmmoSwap;
			replacement->conditionScriptText = source.conditionText;
			if (source.conditionScript)
				SetCondition(*replacement, source.conditionScript);
		}
		// in the set's place, with what the user chose for it
		replacement->origin = set->origin;
		replacement->sequence = set->sequence;
		if (!replacement->fileDisabled)
			replacement->fileDisabled = std::make_unique<std::atomic<bool>[]>(replacement->anims.size());
		for (size_t i = 0; i < replacement->anims.size(); ++i)
			replacement->fileDisabled[i].store(set->fileDisabled[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
		replacement->numFilesDisabled.store(set->numFilesDisabled.load(std::memory_order_relaxed), std::memory_order_relaxed);
		if (!replacement->conditionScriptText.empty() && !replacement->conditionScript.load(std::memory_order_relaxed)
			&& !replacement->conditionFailed.load(std::memory_order_relaxed))
			s_conditionsWaiting.store(true, std::memory_order_relaxed);
		set->disabled.store(true, std::memory_order_relaxed);
		s_retiredSets.push_back(std::move(set));
		set = std::move(replacement);
	}

	Script* CompileCondition(SavedAnims& set)
	{
		std::scoped_lock compileLock(s_compileMutex);
		if (auto* script = set.conditionScript.load(std::memory_order_acquire))
			return script;
		if (set.conditionFailed.load(std::memory_order_relaxed))
			return nullptr;
		auto* script = CompileConditionScript(set.conditionScriptText);
		if (!script)
		{
			// once per condition: a mod's JSON gives the same one to every folder
			ERROR_LOG_ONCE(FormatString("Condition '%s' failed to compile (see the console for the error): the animations of the sets that have it are disabled",
				set.conditionScriptText.data()));
			Overrides::ReportProblem(Overrides::Problem::Kind::Condition, std::string(set.conditionScriptText));
			set.conditionFailed.store(true, std::memory_order_relaxed);
			return nullptr;
		}
		std::unique_lock lock(s_mutex);
		SetCondition(set, script);
		return script;
	}

	// Every stack with what it is registered under. Under the lock
	template <typename F>
	void ForEachStack(F&& f)
	{
		for (const bool firstPerson : { false, true })
			for (const int mapIndex : { kForms, kModIndices })
				for (auto& [identifier, identifierOverrides] : s_maps[firstPerson][mapIndex])
					for (auto& [groupId, stack] : identifierOverrides.stacks)
						f(stack, identifier, mapIndex == kModIndices, firstPerson, groupId);
	}

	template <typename F>
	void ForEachSetLocked(F&& f)
	{
		ForEachStack([&](SetStack& stack, auto&&...)
		{
			for (auto& set : stack.sets)
				f(*set);
		});
	}
}

bool SavedAnims::ContainsAnim(const BSAnimGroupSequence* anim) const
{
	return std::ranges::find(anims, anim->m_kName.CStr(), [](const AnimPath& animPath) { return animPath.path.data(); }) != anims.end();
}

bool SavedAnims::MatchesFolderCondition(const Actor* actor) const
{
	switch (folderCondition)
	{
	case FolderCondition::Male: return !actor->IsFemale();
	case FolderCondition::Female: return actor->IsFemale();
	case FolderCondition::Mod1: return actor->HasWeaponWithMod(kWeaponMod_Flag1);
	case FolderCondition::Mod2: return actor->HasWeaponWithMod(kWeaponMod_Flag2);
	case FolderCondition::Mod3: return actor->HasWeaponWithMod(kWeaponMod_Flag3);
	case FolderCondition::Hurt: return actor->HasCrippledLegs();
	case FolderCondition::Human: return actor == g_thePlayer || IS_ID(actor->baseForm, TESNPC);
	default: return true;
	}
}

size_t Overrides::Register(const Source& source, std::span<const File> files, bool enable)
{
	struct Entry
	{
		std::string_view path;
		UInt16 groupId;
		bool firstPerson;
	};
	// before the lock: a file whose name does not say its group is loaded for it
	std::vector<Entry> entries;
	entries.reserve(files.size());
	for (const auto& file : files)
	{
		const auto path = AddStringToPool(ToLower(file.path));
		const auto groupId = GetAnimGroupId(path);
		if (groupId == kInvalidGroupId)
		{
			ERROR_LOG(FormatString("Failed to resolve file '%s'", path.data()));
			continue;
		}
		entries.push_back({ path, groupId, file.firstPerson });
	}

	// the set this batch made last for a group
	struct OpenSet
	{
		bool firstPerson;
		UInt16 groupId;
		SavedAnims* set;
	};
	std::vector<OpenSet> openSets;
	std::vector<SavedAnims*> newSets;
	// stacks with a set added or moved: put in order at the end
	std::vector<SetStack*> touchedStacks;
	size_t applied = 0;
	bool changed = false;

	std::unique_lock lock(s_mutex);
	for (const auto& [path, groupId, firstPerson] : entries)
	{
		auto& map = s_maps[firstPerson][source.isModIndex ? kModIndices : kForms];
		auto* stack = FindStack(map, source.identifier, groupId);
		// the set that has the file, as one of its animations or as its additive animation
		const auto existing = stack ? std::ranges::find_if(stack->sets, [&](const std::unique_ptr<SavedAnims>& set)
		{
			return set->additiveAnimPath.data() == path.data()
				|| std::ranges::find(set->anims, path.data(), [](const AnimPath& animPath) { return animPath.path.data(); }) != set->anims.end();
		}) : decltype(stack->sets.begin()){};
		const bool exists = stack && existing != stack->sets.end();
		if (!enable)
		{
			if (exists)
			{
				if (!(*existing)->disabled.exchange(true, std::memory_order_relaxed))
					changed = true;
				++applied;
			}
			continue;
		}
		if (exists)
		{
			// Registered again. Only what changes bumps the generation, which every actor's state and the player's
			// preload start over from: a script may register its files on every frame.
			// Hot reload: a set does not change once it is published, so another takes its place
			if (!source.keepCondition && !HasCondition(**existing, source))
			{
				ReplaceCondition(*existing, source);
				changed = true;
			}
			if ((*existing)->disabled.exchange(false, std::memory_order_relaxed))
				changed = true;
			// on top of the sets of its origin's rank
			(*existing)->sequence = ++s_sequence;
			if (!IsOnTopOfItsRank(*stack, **existing))
			{
				touchedStacks.push_back(stack);
				changed = true;
			}
			++applied;
			continue;
		}
		// not part of any set, or they would play as the group's animation
		if (RegisterCustomGroupPath(path))
		{
			++applied;
			continue;
		}

		const auto folderCondition = GetFolderCondition(path);
		auto openSet = std::ranges::find_if(openSets, [&](const OpenSet& open) { return open.firstPerson == firstPerson && open.groupId == groupId; });
		if (openSet == openSets.end() || openSet->set->folderCondition != folderCondition)
		{
			if (!stack)
				stack = &map[source.identifier].stacks[groupId];
			auto* set = stack->sets.emplace_back(std::make_unique<SavedAnims>()).get();
			set->origin = source.origin;
			set->sequence = ++s_sequence;
			touchedStacks.push_back(stack);
			set->folderCondition = folderCondition;
			set->matchBaseGroupId = source.matchBaseGroupId;
			set->pollCondition = source.pollCondition;
			set->conditionScriptText = source.conditionText;
			if (source.conditionScript)
				SetCondition(*set, source.conditionScript);
			newSets.push_back(set);
			if (openSet == openSets.end())
				openSet = openSets.insert(openSets.end(), { firstPerson, groupId, set });
			else
				openSet->set = set;
			SetGroup(firstPerson, groupId);
		}
		auto& set = *openSet->set;
		changed = true;
		++applied;
		if (sv::ends_with_ci(sv::get_file_stem(path), "_additive"))
		{
			set.additiveAnimPath = path;
			continue;
		}
		TransitionAnims::TagPath(set, set.anims.emplace_back(path), groupId);
	}
	for (auto* stack : touchedStacks)
		Order(*stack);
	for (auto* set : newSets)
	{
		Complete(*set);
		if (!set->conditionScriptText.empty() && !set->conditionScript.load(std::memory_order_relaxed))
			s_conditionsWaiting.store(true, std::memory_order_relaxed);
	}
	if (changed)
		s_generation.fetch_add(1, std::memory_order_release);
	return applied;
}

bool SavedAnims::IsFileEnabled(const AnimPath& anim) const
{
	return !fileDisabled || !fileDisabled[&anim - anims.data()].load(std::memory_order_relaxed);
}

bool SavedAnims::IsEnabled() const
{
	return !disabled.load(std::memory_order_relaxed)
		&& !(origin && origin->userDisabled.load(std::memory_order_relaxed))
		&& (anims.empty() || numFilesDisabled.load(std::memory_order_relaxed) < anims.size());
}

bool Overrides::DisableForm(UInt32 formId)
{
	std::unique_lock lock(s_mutex);
	bool found = false;
	for (auto& povMaps : s_maps)
	{
		const auto iter = povMaps[kForms].find(formId);
		if (iter == povMaps[kForms].end())
			continue;
		for (auto& stack : iter->second.stacks | std::views::values)
		{
			for (auto& set : stack.sets)
			{
				set->disabled.store(true, std::memory_order_relaxed);
				found = true;
			}
		}
	}
	if (found)
		s_generation.fetch_add(1, std::memory_order_release);
	return found;
}

bool Overrides::CopyForm(UInt32 fromFormId, UInt32 toFormId)
{
	// a set of the form as one batch for the other: its files stay the variants of one set
	struct Batch
	{
		Source source;
		std::vector<File> files;
	};
	std::vector<Batch> batches;
	{
		std::shared_lock lock(s_mutex);
		for (const bool firstPerson : { true, false })
		{
			auto& map = s_maps[firstPerson][kForms];
			const auto iter = map.find(fromFormId);
			if (iter == map.end())
				continue;
			for (const auto& stack : iter->second.stacks | std::views::values)
			{
				for (const auto& set : stack.sets)
				{
					auto& batch = batches.emplace_back();
					batch.source = {
						.identifier = toFormId,
						.isModIndex = false,
						.conditionScript = set->conditionScript.load(std::memory_order_relaxed),
						.conditionText = set->conditionScriptText,
						.pollCondition = set->pollCondition,
						.matchBaseGroupId = set->matchBaseGroupId,
						.origin = set->origin,
					};
					for (const auto& anim : set->anims)
						batch.files.push_back({ anim.path, firstPerson });
					if (!set->additiveAnimPath.empty())
						batch.files.push_back({ set->additiveAnimPath, firstPerson });
				}
			}
		}
	}
	size_t applied = 0;
	for (const auto& batch : batches)
		applied += Register(batch.source, batch.files);
	return applied != 0;
}

void Overrides::Clear()
{
	std::unique_lock lock(s_mutex);
	for (auto& povMaps : s_maps)
		for (auto& map : povMaps)
			map.clear();
	for (auto& povGroups : s_groups)
		for (auto& bits : povGroups)
			bits.store(0, std::memory_order_relaxed);
	s_customGroupPaths.clear();
	s_retiredSets.clear();
	s_conditionsWaiting.store(false, std::memory_order_relaxed);
	s_generation.fetch_add(1, std::memory_order_release);
	std::scoped_lock problemsLock(s_problemsMutex);
	s_problems.clear();
}

const OverrideOrigin* Overrides::GetOrigin(OverrideOrigin::Kind kind, std::string_view name, std::string_view jsonFile, int jsonPriority)
{
	auto key = ToLower(name);
	{
		std::shared_lock lock(s_mutex);
		if (const auto iter = s_originsByName.find(key); iter != s_originsByName.end())
			return iter->second;
	}
	auto origin = std::make_unique<OverrideOrigin>();
	origin->kind = kind;
	origin->name = name;
	origin->jsonFile = jsonFile;
	origin->jsonPriority = jsonPriority;
	origin->userDisabled = OverridePrefs::IsOriginDisabled(name);
	origin->rank = OverridePrefs::GetRank(OverridePrefs::Get(), name);
	std::unique_lock lock(s_mutex);
	// another thread may have made it meanwhile
	const auto [iter, inserted] = s_originsByName.try_emplace(std::move(key), origin.get());
	if (inserted)
		s_origins.push_back(std::move(origin));
	return iter->second;
}

std::vector<const OverrideOrigin*> Overrides::GetOrigins()
{
	std::shared_lock lock(s_mutex);
	std::vector<const OverrideOrigin*> origins;
	origins.reserve(s_origins.size());
	for (const auto& origin : s_origins)
		origins.push_back(origin.get());
	return origins;
}

void Overrides::ApplyPrefs()
{
	const auto prefs = OverridePrefs::Get();
	std::unique_lock lock(s_mutex);
	for (const auto& origin : s_origins)
	{
		origin->userDisabled.store(prefs.disabledOrigins.contains(ToLower(origin->name)), std::memory_order_relaxed);
		origin->rank = OverridePrefs::GetRank(prefs, origin->name);
	}
	ForEachStack([&](SetStack& stack, auto&&...)
	{
		for (const auto& set : stack.sets)
			ApplyFilePrefs(*set, prefs);
		Order(stack);
	});
	for (const auto& set : s_retiredSets)
		ApplyFilePrefs(*set, prefs);
	s_generation.fetch_add(1, std::memory_order_release);
}

void Overrides::ForEachSet(const std::function<void(const SetView&)>& visitor)
{
	std::shared_lock lock(s_mutex);
	ForEachStack([&](const SetStack& stack, UInt32 identifier, bool isModIndex, bool firstPerson, UInt16 groupId)
	{
		const auto size = static_cast<UInt32>(stack.sets.size());
		for (UInt32 i = 0; i < size; ++i)
			visitor({ stack.sets[i].get(), identifier, isModIndex, firstPerson, groupId, size - 1 - i, size });
	});
}

void Overrides::ReportProblem(Problem::Kind kind, const std::string& text)
{
	Problem problem{ kind, text };
	std::scoped_lock lock(s_problemsMutex);
	if (std::ranges::find(s_problems, problem) == s_problems.end())
		s_problems.push_back(std::move(problem));
}

std::vector<Overrides::Problem> Overrides::GetProblems()
{
	std::scoped_lock lock(s_problemsMutex);
	return s_problems;
}

bool Overrides::HasGroup(bool firstPerson, UInt16 groupId)
{
	return s_groups[firstPerson][groupId / 32].load(std::memory_order_relaxed) & 1u << groupId % 32;
}

UInt32 Overrides::Generation()
{
	return s_generation.load(std::memory_order_acquire);
}

Overrides::Identity Overrides::GetIdentity(Actor* actor)
{
	Identity identity;
	identity.actor = actor->refID;
	if (const auto* weaponInfo = actor->baseProcess->GetWeaponInfo(); weaponInfo && weaponInfo->weapon)
		identity.weapon = weaponInfo->weapon->refID;
	if (const auto* baseForm = actor->baseForm)
		identity.baseForm = baseForm->refID;
	if (const auto* race = actor->GetRace())
		identity.race = race->refID;
	if (const auto* actorBase = actor->GetActorBase())
		identity.actorBase = actorBase->refID;
	return identity;
}

void Overrides::CollectCandidates(const Identity& identity, bool firstPerson, UInt16 groupId, std::vector<SavedAnims*>& out)
{
	const UInt32 formIds[] = { identity.actor, identity.weapon, identity.baseForm, identity.race, identity.actorBase };
	// a form's sets come before those of any mod, a mod's before everyone's: collected in that order, each identifier
	// once (an NPC's base form is its actor base; a reference made at run time has everyone's "mod index")
	UInt32 tried[std::size(formIds)];
	size_t numTried = 0;
	UInt8 modIndices[std::size(formIds) + 1];
	size_t numModIndices = 0;

	std::shared_lock lock(s_mutex);
	const auto append = [&](OverrideMap& map, UInt32 identifier)
	{
		if (const auto* stack = FindStack(map, identifier, groupId))
		{
			for (const auto& set : stack->sets | std::views::reverse)
				out.push_back(set.get());
		}
	};
	for (const auto formId : formIds)
	{
		if (!formId || std::find(tried, tried + numTried, formId) != tried + numTried)
			continue;
		tried[numTried++] = formId;
		append(s_maps[firstPerson][kForms], formId);
		const auto modIndex = static_cast<UInt8>(formId >> 24);
		if (std::find(modIndices, modIndices + numModIndices, modIndex) == modIndices + numModIndices)
			modIndices[numModIndices++] = modIndex;
	}
	if (std::find(modIndices, modIndices + numModIndices, kEveryone) == modIndices + numModIndices)
		modIndices[numModIndices++] = kEveryone;
	for (size_t i = 0; i < numModIndices; ++i)
		append(s_maps[firstPerson][kModIndices], modIndices[i]);
}

void Overrides::CollectFormPaths(UInt32 formId, bool firstPerson, const Actor* actor, std::vector<const char*>& out)
{
	std::shared_lock lock(s_mutex);
	auto& map = s_maps[firstPerson][kForms];
	const auto iter = map.find(formId);
	if (iter == map.end())
		return;
	for (const auto& stack : iter->second.stacks | std::views::values)
	{
		for (const auto& set : stack.sets)
		{
			if (!set->IsEnabled() || set->conditionFailed.load(std::memory_order_relaxed))
				continue;
			// the weapon's mods can change while it is equipped, the actor's sex and kind do not
			const auto condition = set->folderCondition;
			if ((condition == FolderCondition::Male || condition == FolderCondition::Female || condition == FolderCondition::Human)
				&& !set->MatchesFolderCondition(actor))
				continue;
			for (const auto& anim : set->anims)
			{
				if (set->IsFileEnabled(anim))
					out.push_back(anim.path.data());
			}
			if (!set->additiveAnimPath.empty())
				out.push_back(set->additiveAnimPath.data());
		}
	}
}

Script* Overrides::GetCondition(SavedAnims& set)
{
	if (auto* script = set.conditionScript.load(std::memory_order_acquire))
		return script;
	if (set.conditionScriptText.empty() || set.conditionFailed.load(std::memory_order_relaxed))
		return nullptr;
	return CompileCondition(set);
}

void Overrides::CompileConditions()
{
	if (!s_conditionsWaiting.exchange(false, std::memory_order_relaxed))
		return;
	std::vector<SavedAnims*> waiting;
	{
		std::shared_lock lock(s_mutex);
		ForEachSetLocked([&](SavedAnims& set)
		{
			if (!set.conditionScriptText.empty() && !set.conditionScript.load(std::memory_order_relaxed)
				&& !set.conditionFailed.load(std::memory_order_relaxed))
				waiting.push_back(&set);
		});
	}
	for (auto* set : waiting)
		CompileCondition(*set);
}

bool Overrides::HasCustomGroupPaths(std::string_view basePath)
{
	std::shared_lock lock(s_mutex);
	return s_customGroupPaths.contains(basePath);
}

std::vector<std::string_view> Overrides::GetCustomGroupPaths(std::string_view basePath)
{
	std::shared_lock lock(s_mutex);
	const auto iter = s_customGroupPaths.find(basePath);
	return iter != s_customGroupPaths.end() ? iter->second : std::vector<std::string_view>{};
}
