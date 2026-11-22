#include "actor_overrides.h"

#include <mutex>
#include <optional>

#include "additive_anims.h"
#include "anim_fixes.h"
#include "commands_animation.h"
#include "file_animations.h"
#include "GameObjects.h"
#include "GameTasks.h"
#include "hooks.h"
#include "main.h"
#include "SafeWrite.h"
#include "reload_kinds.h"
#include "sequence_extradata.h"
#include "string_view_util.h"
#include "utility.h"

namespace
{
	// Every Animation that has a state. kNVSEReset walks this list.
	std::mutex s_statesMutex;
	std::vector<ActorOverrides*> s_states;

	// The AnimSequenceSingle of the KF being loaded, read by the Animation::InitGroupSpeed lookup hook. The group is
	// not in the Animation's map, because an override never is.
	thread_local AnimSequenceBase* t_loadingBase = nullptr;

	bool __fastcall InitGroupSpeedLookupHook(NiTPointerMap<AnimSequenceBase*>* map, void*, UInt16 groupId, AnimSequenceBase** base)
	{
		if (t_loadingBase)
		{
			*base = t_loadingBase;
			return true;
		}
		return GameFuncs::NiTPointerMap_Lookup(map, groupId, base);
	}

	// Animation::GroupLoaded for the places where the engine decides whether to load the KFs of a weapon type for an
	// actor (attaching a weapon while its 3D loads on a task thread, queueing an item's files). Only the Animation's own
	// map counts. If an override of the weapon's Equip counted as loaded, the actor would never get the other
	// animations of that weapon type.
	bool __fastcall VanillaGroupLoaded(AnimData* animData, void*, UInt16 groupId)
	{
		AnimSequenceBase* base = nullptr;
		return GameFuncs::NiTPointerMap_Lookup(animData->mapAnimSequenceBase, groupId, &base);
	}

	// True on the background loading threads: IOManager's task threads and ModelLoader's clone thread. An actor's 3D is
	// built there (QueuedReference::BackgroundClone, Actor::Load3D) and asks about its animations while the main thread
	// runs the game.
	bool IsLoaderThread()
	{
		thread_local int t_kind = 0;
		if (!t_kind)
		{
			const auto isThread = [threadId = GetCurrentThreadId()](const BSTaskThread* taskThread)
			{
				return taskThread && taskThread->uiThreadID == threadId;
			};
			bool isLoader = false;
			if (const auto* ioManager = IOManager::GetSingleton(); ioManager && ioManager->ppThreads)
			{
				for (UInt32 i = 0; i < ioManager->uiThreadCount; ++i)
					isLoader |= isThread(ioManager->ppThreads[i]);
			}
			if (const auto* modelLoader = ModelLoader::GetSingleton())
				isLoader |= isThread(modelLoader->pBackgroundCloneThread);
			t_kind = isLoader ? 2 : 1;
		}
		return t_kind == 2;
	}

	// Special and dynamic idles (TESIdleForm, hit reactions, Pip-Boy) share one anim group. An override replaces only
	// the idle whose KF has the override's file name. Animation::SpecialIdlePlay adds the idle's sequence to the map
	// before playing the group.
	std::optional<std::string_view> GetPlayingIdleFileStem(AnimData* animData, UInt16 groupId)
	{
		if (groupId != kAnimGroup_DynamicIdle && groupId != kAnimGroup_SpecialIdle)
			return std::nullopt;
		auto* base = animData->mapAnimSequenceBase->Lookup(groupId);
		auto* anim = base ? base->GetSequenceByIndex(-1) : nullptr;
		if (!anim)
			return std::nullopt;
		return sv::get_file_stem(anim->m_kName.CStr());
	}

	bool IsIdleFileStem(const AnimPath& animPath, std::string_view idleFileStem)
	{
		return sv::equals_ci(sv::get_file_stem(animPath.path), idleFileStem);
	}

	// Aim, AimUp and AimDown (and their IS variants, the attacks and the throws) are three consecutive groups that play
	// together. Returns the first group of the three.
	std::optional<UInt32> GetTripleBase(UInt16 groupId)
	{
		const UInt32 baseGroupId = groupId & 0xFF;
		if (baseGroupId >= kAnimGroup_Aim && baseGroupId <= kAnimGroup_AimISDown)
			return kAnimGroup_Aim;
		if (baseGroupId >= kAnimGroup_AttackLeft && baseGroupId <= kAnimGroup_AttackSpin2ISDown)
			return kAnimGroup_AttackLeft;
		if (baseGroupId >= kAnimGroup_PlaceMine && baseGroupId <= kAnimGroup_AttackThrow8ISDown)
			return kAnimGroup_PlaceMine;
		return std::nullopt;
	}

	// Decides which of a set's files can play for the group right now.
	struct PathFilter
	{
		const SavedAnims& set;
		const ActorOverrides* state;
		std::optional<std::string_view> idleFileStem;
		bool isReload = false;
		bool didAmmoSwap = false;
		bool didPartialReload = false;

		PathFilter(const SavedAnims& set, UInt16 groupId, AnimData* animData, const ActorOverrides* state)
			: set(set), state(state), idleFileStem(GetPlayingIdleFileStem(animData, groupId))
		{
			if (IsAnimGroupReload(static_cast<AnimGroupID>(groupId)) && (set.hasAmmoSwap || set.hasPartialReload))
			{
				isReload = true;
				const auto kind = ReloadKinds::Get(animData->actor);
				didAmmoSwap = kind == ReloadKind::AmmoSwap;
				didPartialReload = kind == ReloadKind::Partial;
			}
		}

		bool operator()(const AnimPath& animPath) const
		{
			// Start and stop animations only play through MovementTransitions and AimTransitions.
			if (animPath.isStartAnim || animPath.isStopAnim || !set.IsFileEnabled(animPath))
				return false;
			if (idleFileStem && !IsIdleFileStem(animPath, *idleFileStem))
				return false;
			if (isReload)
			{
				if (set.hasAmmoSwap && animPath.isAmmoSwap != didAmmoSwap)
					return false;
				if (set.hasPartialReload && animPath.partialReload != didPartialReload)
					return false;
			}
			return !state || !state->HasFailed(animPath.path.data());
		}
	};

	struct Choice
	{
		const AnimPath* path = nullptr;
		ActorOverrides::Variant* variant = nullptr;
		UInt32 index = 0;
	};

	Choice ChoosePath(SavedAnims& set, UInt16 groupId, AnimData* animData, ActorOverrides* state)
	{
		const PathFilter filter(set, groupId, animData, state);
		// A set with one file always plays it, whatever the reload's kind.
		if (set.anims.size() == 1 && !filter.idleFileStem && !set.hasTransitionAnims) [[likely]]
			return { (state && state->HasFailed(set.anims[0].path.data())) || !set.IsFileEnabled(set.anims[0]) ? nullptr : &set.anims[0] };

		const auto count = static_cast<UInt32>(std::ranges::count_if(set.anims, filter));
		if (!count)
			return {};
		const auto nth = [&](UInt32 index) -> const AnimPath*
		{
			for (const auto& animPath : set.anims)
			{
				if (filter(animPath) && !index--)
					return &animPath;
			}
			return nullptr;
		};
		if (count == 1)
			return { nth(0) };
		if (!state)
			return { nth(set.hasOrder ? 0 : GetRandomUInt(count)) };

		const auto tripleBase = set.hasOrder ? std::nullopt : GetTripleBase(groupId);
		// The up and down groups follow the variant that their aim (attack) group played.
		if (tripleBase && ((groupId & 0xFF) - *tripleBase) % 3 != 0 && state->tripleIndex < count)
			return { nth(state->tripleIndex), nullptr, state->tripleIndex };

		auto variant = std::ranges::find(state->variants, &set, &ActorOverrides::Variant::set);
		if (variant == state->variants.end())
			variant = state->variants.insert(variant, { &set, nullptr, 0, 0 });
		if (variant->next && filter(*variant->next))
			return { variant->next, &*variant, variant->nextIndex };
		variant->nextIndex = set.hasOrder ? variant->played % count : GetRandomUInt(count);
		variant->next = nth(variant->nextIndex);
		return { variant->next, &*variant, variant->nextIndex };
	}

	// Marks the chosen file as played, so the next play chooses again.
	void Consume(const Choice& choice, UInt16 groupId, ActorOverrides* state)
	{
		if (!state)
			return;
		if (const auto tripleBase = GetTripleBase(groupId); tripleBase && ((groupId & 0xFF) - *tripleBase) % 3 == 0)
			state->tripleIndex = choice.index;
		if (choice.variant)
		{
			choice.variant->next = nullptr;
			++choice.variant->played;
		}
	}

	// Does what Animation::AddAnimation (0x490500) does with a KF, except for the entry in the Animation's map of
	// groups:
	//  - the sequence is the KF model's own while nothing else refers to it, otherwise a clone
	//  - an AnimSequenceSingle holds it, together with the KF model use counted by loading
	//  - it joins the controller manager
	//  - the group's speed is taken from its root motion
	// Safe to call from several threads, unlike calling AddAnimation with a swapped map.
	Overrides::LoadedAnim CreateSequence(const char* path, AnimData* animData)
	{
		auto* manager = animData->controllerManager;
		BSAnimGroupSequence* sequence;
		AnimSequenceBase* base;
		TESAnimGroup* animGroup;
		{
			// One thread at a time decides who gets the KF model's own sequence.
			std::scoped_lock lock(KFLoader::Mutex());
			auto* kfModel = KFLoader::AcquireLocked(path);
			const auto fail = [&](const std::string& reason) -> Overrides::LoadedAnim
			{
				if (kfModel)
					KFLoader::ReleaseLocked(path);
				ERROR_LOG(reason);
				return {};
			};
			if (!kfModel || !kfModel->animGroup || !kfModel->controllerSequence)
				return fail(FormatString("Failed to load KF Model %s", path));
			animGroup = kfModel->animGroup;
			auto* source = kfModel->controllerSequence;
			const auto baseGroupId = animGroup->groupID & 0xFF;
			if (baseGroupId == kAnimGroup_Invalid)
				return fail(FormatString("Failed to load anim %s: its sequence name is not an anim group", path));
			// The engine does not support Clamp animations in locomotion. Start and stop animations play once.
			if (baseGroupId >= kAnimGroup_Forward && baseGroupId <= kAnimGroup_TurnRight && source->m_eCycleType == NiControllerSequence::CLAMP)
			{
				const static NiFixedString kAllowClamp = "allowClampInLocomotion";
				if (!(source->m_spTextKeys && source->m_spTextKeys->FindFirstByName(kAllowClamp))
					&& TransitionAnims::GetPathKind(source->m_kName.Str(), animGroup->groupID) == TransitionKind::None)
					return fail(FormatString("Failed to load anim %s: a movement animation with cycle type Clamp (add an allowClampInLocomotion text key to allow it)", path));
			}
			if (IS_TYPE(source, BSAnimGroupSequence) && source->m_uiRefCount == 1)
			{
				sequence = source;
				// Only the KF model refers to it, so no manager has it. That includes a manager that removed it without
				// clearing its owner.
				sequence->m_pkOwner = nullptr;
			}
			else
			{
				sequence = NiNew<BSAnimGroupSequence>();
				GameFuncs::BSAnimGroupSequence_Init(sequence, animGroup, source);
			}
			base = static_cast<AnimSequenceBase*>(FormHeap_Allocate(sizeof(AnimSequenceSingle)));
			ThisStdCall(0x490E10, base); // AnimSequenceSingle::AnimSequenceSingle
			base->AddAnimGroupSequence(sequence, true);
		}
		if (!manager->AddSequence(sequence, NiFixedString(), true))
		{
			ERROR_LOG(FormatString("Failed to load anim %s for anim data of actor %X: the skeleton lacks nodes it animates, or it is another actor's",
				path, animData->actor->refID));
			std::scoped_lock lock(KFLoader::Mutex());
			base->Destroy(true);
			return {};
		}
		// Animation::pAccumRoot
		if (!animData->nBip01)
			animData->nBip01 = ThisStdCall<NiNode*>(0x490D90, manager); // NiControllerManager::GetAccumRoot
		t_loadingBase = base;
		ThisStdCall(0x493D50, animData, animGroup); // Animation::InitGroupSpeed
		t_loadingBase = nullptr;
		return { sequence, base };
	}

	void Unlist(std::vector<ActorOverrides*>& states, ActorOverrides* state)
	{
		std::erase(states, state);
	}
}

ActorOverrides* ActorOverrides::Peek(AnimData* animData)
{
	const auto* manager = animData->controllerManager;
	if (!manager || !manager->m_pkTarget)
		return nullptr;
	auto* extraData = kAnimationExtraData::Get(manager->m_pkTarget);
	return extraData && extraData->overrides.animData == animData ? &extraData->overrides : nullptr;
}

ActorOverrides* ActorOverrides::Get(AnimData* animData)
{
	const auto* manager = animData->controllerManager;
	if (!manager || !manager->m_pkTarget)
		return nullptr;
	auto& state = kAnimationExtraData::GetOrCreate(manager->m_pkTarget)->overrides;
	if (state.animData != animData) [[unlikely]]
	{
		// The node had another Animation, so the stored state belongs to that Animation's controller manager.
		if (state.animData)
			state.Release();
		state.animData = animData;
		state.actorId = animData->actor ? animData->actor->refID : 0;
		std::scoped_lock lock(s_statesMutex);
		s_states.push_back(&state);
	}
	return &state;
}

void ActorOverrides::Release()
{
	if (!animData)
		return;
	{
		std::scoped_lock lock(s_statesMutex);
		Unlist(s_states, this);
	}
	KFLoader::DropPreload(preload);
	if (!loaded.empty())
	{
		std::scoped_lock lock(KFLoader::Mutex());
		for (auto& entry : loaded)
		{
			// Read by the player's attack loops (DecreaseAttackTimer), and the sequence may be freed below.
			if (g_lastLoopSequence == entry.sequence.data)
				g_lastLoopSequence = nullptr;
			// ~AnimSequenceSingle releases the sequence and the KF model use.
			entry.base->Destroy(true);
			entry.sequence = nullptr;
		}
		loaded.clear();
	}
	failed.clear();
	memoGroups.clear();
	memos.clear();
	candidates.clear();
	variants.clear();
	polledPlays.clear();
	queued.clear();
	generation = 0;
	identity = {};
	tripleIndex = 0;
	animData = nullptr;
	actorId = 0;
}

void ActorOverrides::Unload()
{
	if (!animData)
		return;
	auto* manager = animData->controllerManager;
	for (auto& entry : loaded)
	{
		BSAnimGroupSequence* sequence = entry.sequence;
		// The sections keep their sequence after it stops playing, and pLastMovementSequence keeps the one that ClearGroup
		// deactivated. Clear both before Release frees the sequence, or Animation::Update would read freed memory.
		for (auto section = 0; section < static_cast<int>(std::size(animData->animSequence)); ++section)
		{
			if (animData->animSequence[section] == sequence)
				animData->ClearGroup(static_cast<eAnimSequence>(section), 0.0f);
		}
		if (animData->animSeq100 == sequence)
			animData->animSeq100 = nullptr;
		if (!manager || sequence->m_pkOwner != manager)
			continue;
		if (sequence->m_eState != NiControllerSequence::INACTIVE)
		{
			manager->DeactivateSequence(sequence, 0.0f);
			NiUpdateData updateData{};
			manager->Update(&updateData);
		}
		// NiControllerManager::RemoveSequence clears the sequence's targets, releases followers and drops kNVSE's data
		// for it (SequenceExtraDatas' hook there).
		GameFuncs::NiControllerManager_RemoveSequence(manager, sequence);
	}
	Release();
}

ActorOverrides::Loaded* ActorOverrides::FindLoaded(const char* path)
{
	const auto iter = std::ranges::find(loaded, path, &Loaded::path);
	return iter != loaded.end() ? &*iter : nullptr;
}

bool ActorOverrides::HasFailed(const char* path) const
{
	return std::ranges::find(failed, path) != failed.end();
}

void ActorOverrides::Refresh(Actor* actor, bool firstPerson)
{
	const auto currentGeneration = Overrides::Generation();
	const auto currentIdentity = Overrides::GetIdentity(actor);
	if (currentGeneration == generation && currentIdentity == identity) [[likely]]
		return;
	generation = currentGeneration;
	identity = currentIdentity;
	memoGroups.clear();
	memos.clear();
	candidates.clear();
	// The player's weapon, or the sets registered for it, changed. A worker thread reads its KFs ahead of time so the
	// first draw, aim, shot and reload do not each read a file on the main thread. Other actors load KFs on first play,
	// since they share most of them and hold few weapons for long. The new batch is queued before the old one is
	// dropped, so files in both stay loaded.
	if (actor == PlayerCharacter::GetSingleton())
	{
		std::vector<const char*> paths;
		if (identity.weapon)
			Overrides::CollectFormPaths(identity.weapon, firstPerson, actor, paths);
		auto previous = std::move(preload);
		preload = KFLoader::StartPreload(std::move(paths));
		KFLoader::DropPreload(previous);
	}
}

std::span<SavedAnims* const> ActorOverrides::GetCandidates(Actor* actor, bool firstPerson, UInt16 groupId)
{
	Refresh(actor, firstPerson);
	const auto iter = std::ranges::find(memoGroups, groupId);
	if (iter != memoGroups.end())
	{
		const auto& memo = memos[iter - memoGroups.begin()];
		return { candidates.data() + memo.begin, memo.count };
	}
	const auto begin = static_cast<UInt32>(candidates.size());
	Overrides::CollectCandidates(identity, firstPerson, groupId, candidates);
	const auto count = static_cast<UInt32>(candidates.size()) - begin;
	const std::span<SavedAnims* const> collected{ candidates.data() + begin, count };
	memoGroups.push_back(groupId);
	memos.push_back({ begin, count, std::ranges::any_of(collected, &SavedAnims::pollCondition) });
	return collected;
}

bool ActorOverrides::HasPolledCandidates(Actor* actor, bool firstPerson, UInt16 groupId)
{
	GetCandidates(actor, firstPerson, groupId);
	const auto iter = std::ranges::find(memoGroups, groupId);
	return memos[iter - memoGroups.begin()].hasPolled;
}

namespace
{
	// Decides whether Overrides::Find can use one candidate set. Runs its condition script.
	Overrides::Verdict Judge(SavedAnims& set, Actor* actor, UInt16 groupId, TransitionKind transition,
		const std::optional<std::string_view>& idleFileStem)
	{
		using enum Overrides::Verdict;
		if (!set.IsEnabled())
			return Disabled;
		if (!set.MatchesFolderCondition(actor))
			return FolderCondition;
		if ((set.hasTransitionAnims || transition != TransitionKind::None) && !TransitionAnims::HasPaths(set, transition))
			return NoTransition;
		if (idleFileStem && std::ranges::none_of(set.anims, [&](const AnimPath& animPath) { return IsIdleFileStem(animPath, *idleFileStem); }))
			return OtherIdle;
		if (set.conditionScript.load(std::memory_order_relaxed) || !set.conditionScriptText.empty())
		{
			if (IsLoaderThread())
			{
#if _DEBUG
				ERROR_LOG_ONCE(FormatString("Overrides: the conditional set of %s was passed over on a loader thread (group %X of actor %X)",
					set.anims.empty() ? "?" : set.anims[0].path.data(), groupId, actor->refID));
#endif
				return LoaderThread;
			}
			auto* conditionScript = Overrides::GetCondition(set);
			if (!conditionScript)
				return ConditionFailed;
			NVSEArrayVarInterface::Element result;
			if (!CallFunction(conditionScript, actor, nullptr, &result) || result.GetNumber() == 0.0)
				return ConditionFalse;
		}
		return set.anims.empty() ? NoFiles : Plays;
	}
}

SavedAnims* Overrides::Find(UInt16 groupId, AnimData* animData, TransitionKind transition)
{
	if (!FileAnimations::IsLoaded()) [[unlikely]]
		FileAnimations::WaitUntilLoaded();
	if (!animData || !animData->actor || !animData->actor->baseProcess)
		return nullptr;
	const auto firstPerson = animData == PlayerCharacter::GetSingleton()->firstPersonAnimData;
	if (!HasGroup(firstPerson, groupId))
		return nullptr;

	auto* actor = animData->actor;
	auto* state = ActorOverrides::Get(animData);
	// Copies the candidates, because a condition script may ask about the actor's animations, which can change the
	// state's list. The copy lives on the stack for up to 128 sets (512 bytes). It is looked up several times per frame
	// and Idle on a third person skeleton can have around 40 global conditional sets.
	constexpr size_t kInline = 128;
	SavedAnims* inlineSets[kInline];
	std::vector<SavedAnims*> moreSets;
	std::span<SavedAnims* const> candidates;
	if (state)
	{
		const auto memo = state->GetCandidates(actor, firstPerson, groupId);
		if (memo.empty())
			return nullptr;
		if (memo.size() <= kInline)
		{
			std::ranges::copy(memo, inlineSets);
			candidates = { inlineSets, memo.size() };
		}
		else
		{
			moreSets.assign(memo.begin(), memo.end());
			candidates = moreSets;
		}
	}
	else
	{
		CollectCandidates(GetIdentity(actor), firstPerson, groupId, moreSets);
		candidates = moreSets;
	}

	const auto idleFileStem = GetPlayingIdleFileStem(animData, groupId);
	for (auto* set : candidates)
	{
		if (Judge(*set, actor, groupId, transition, idleFileStem) == Verdict::Plays)
			return set;
	}
	return nullptr;
}

SavedAnims* Overrides::FindForGroup(UInt16 groupId, AnimData* animData, TransitionKind transition)
{
	if (auto* baseSet = Find(groupId & 0xFF, animData, transition); baseSet && baseSet->matchBaseGroupId)
		return baseSet;
	return Find(groupId, animData, transition);
}

bool Overrides::Passes(SavedAnims& set, UInt16 groupId, AnimData* animData)
{
	if (!animData || !animData->actor)
		return false;
	return Judge(set, animData->actor, groupId, TransitionKind::None, GetPlayingIdleFileStem(animData, groupId)) == Verdict::Plays;
}

std::vector<Overrides::Candidate> Overrides::Explain(UInt16 groupId, AnimData* animData, TransitionKind transition)
{
	std::vector<Candidate> explained;
	if (!animData || !animData->actor || !animData->actor->baseProcess)
		return explained;
	auto* actor = animData->actor;
	std::vector<SavedAnims*> candidates;
	CollectCandidates(GetIdentity(actor), animData == PlayerCharacter::GetSingleton()->firstPersonAnimData, groupId, candidates);
	const auto idleFileStem = GetPlayingIdleFileStem(animData, groupId);
	for (auto* set : candidates)
		explained.push_back({ set, Judge(*set, actor, groupId, transition, idleFileStem) });
	return explained;
}

Overrides::LoadedAnim Overrides::Load(std::string_view path, AnimData* animData)
{
	auto* state = animData ? ActorOverrides::Get(animData) : nullptr;
	if (!state)
		return {};
	if (auto* entry = state->FindLoaded(path.data()))
	{
		BSAnimGroupSequence* sequence = entry->sequence;
		auto* manager = animData->controllerManager;
		if (sequence->m_pkOwner == manager)
			return { sequence, entry->base };
		// The engine removed the sequence from the manager: Animation::AnimIdleFree removes the sequence that a special
		// or dynamic idle played, which is this one when it replaced the idle's KF. Without an owner it would never
		// update, and the idle playing it would never end.
		if (!sequence->m_pkOwner && manager->AddSequence(sequence, NiFixedString(), true))
			return { sequence, entry->base };
		ERROR_LOG(FormatString("Failed to add anim %s back to the controller manager of actor %X", path.data(),
			animData->actor ? animData->actor->refID : 0));
		return {};
	}
	if (state->HasFailed(path.data()))
		return {};
	const auto created = CreateSequence(path.data(), animData);
	if (!created)
	{
		state->failed.push_back(path.data());
		Overrides::ReportProblem(Overrides::Problem::Kind::Load, std::string(path));
		return {};
	}
	state->loaded.push_back({ path.data(), created.sequence, created.base });
	AnimFixes::FixWrongAKeyInRespectEndKey(animData, created.sequence);
	return created;
}

Overrides::LoadedAnim Overrides::Load(SavedAnims& set, const AnimPath& animPath, AnimData* animData)
{
	const auto loaded = Load(animPath.path, animData);
	if (loaded && (animPath.isStartAnim || animPath.isStopAnim))
		TransitionAnims::Prepare(loaded.sequence);
	return loaded;
}

const AnimPath* Overrides::GetNextPath(SavedAnims& set, UInt16 groupId, AnimData* animData)
{
	return ChoosePath(set, groupId, animData, ActorOverrides::Get(animData)).path;
}

Overrides::LoadedAnim Overrides::LoadNext(SavedAnims& set, UInt16 groupId, AnimData* animData)
{
	auto* state = ActorOverrides::Get(animData);
	// A file that does not load is skipped from then on, so the next one is chosen.
	for (auto attempts = set.anims.size(); attempts; --attempts)
	{
		const auto choice = ChoosePath(set, groupId, animData, state);
		if (!choice.path)
			break;
		if (const auto loaded = Load(set, *choice.path, animData))
			return loaded;
	}
	return {};
}

BSAnimGroupSequence* Overrides::Play(SavedAnims& set, UInt16 groupId, AnimData* animData)
{
	auto* state = ActorOverrides::Get(animData);
	for (auto attempts = set.anims.size(); attempts; --attempts)
	{
		const auto choice = ChoosePath(set, groupId, animData, state);
		if (!choice.path)
			break;
		const auto loaded = Load(set, *choice.path, animData);
		if (!loaded)
			continue;
		Consume(choice, groupId, state);
		HandleExtraOperations(animData, loaded.sequence);
		if (!set.additiveAnimPath.empty())
		{
			if (auto* additiveAnim = FindOrLoadAnim(animData, set.additiveAnimPath.data()))
				AdditiveManager::PlayManagedAdditiveAnim(animData, loaded.sequence, additiveAnim);
		}
		return loaded.sequence;
	}
	return nullptr;
}

void Overrides::Played(AnimData* animData, const BSAnimGroupSequence* sequence)
{
	auto* state = ActorOverrides::Peek(animData);
	const auto* path = sequence->m_kName.CStr();
	if (!state || !sequence->animGroup || !state->FindLoaded(path))
		return;
	const auto variant = std::ranges::find_if(state->variants, [&](const ActorOverrides::Variant& pending)
	{
		return pending.next && pending.next->path.data() == path;
	});
	Consume(variant != state->variants.end() ? Choice{ variant->next, &*variant, variant->nextIndex } : Choice{},
		sequence->animGroup->groupID, state);
}

void Overrides::OnAnimDataDelete(AnimData* animData)
{
	ActorOverrides* state = nullptr;
	{
		std::scoped_lock lock(s_statesMutex);
		const auto iter = std::ranges::find(s_states, animData, &ActorOverrides::animData);
		if (iter != s_states.end())
			state = *iter;
	}
	if (state)
		state->Release();
}

void Overrides::UpdatePlayerPreload()
{
	auto* player = PlayerCharacter::GetSingleton();
	if (!FileAnimations::IsLoaded() || !player || !player->baseProcess)
		return;
	for (auto* animData : { player->firstPersonAnimData, player->baseProcess->GetAnimData() })
	{
		if (!animData || animData->actor != player)
			continue;
		if (auto* state = ActorOverrides::Get(animData))
			state->Refresh(player, animData == player->firstPersonAnimData);
	}
}

void Overrides::UnloadAll()
{
	std::vector<ActorOverrides*> states;
	{
		std::scoped_lock lock(s_statesMutex);
		states = s_states;
	}
	for (auto* state : states)
		state->Unload();
}

namespace
{
	// Groups whose animation plays on until something else is picked: idles, walks, runs, turns, jump loops, the block
	// idle, hand grips and aims. Every other group plays once, so a changed condition applies when it plays next, and
	// playing it again partway through would restart an attack, an equip, a landing or a reload. Attack loops fire and
	// dynamic idles are special idles (AnimIdle keeps their sequence), so neither is played again either.
	bool IsPolledGroup(UInt16 groupId)
	{
		const auto baseGroupId = static_cast<AnimGroupID>(groupId & 0xFF);
		if (baseGroupId >= g_animGroupInfos.size() || baseGroupId == kAnimGroup_DynamicIdle
			|| (baseGroupId >= kAnimGroup_AttackLoop && baseGroupId <= kAnimGroup_AttackLoopISDown))
			return false;
		return GetGroupInfo(baseGroupId)->actionType == kAnimGroupActionType_Loop;
	}

	bool IsPlaying(const BSAnimGroupSequence* sequence)
	{
		const auto state = sequence->m_eState;
		return state == NiControllerSequence::ANIMATING || state == NiControllerSequence::EASEIN
			|| state == NiControllerSequence::TRANSDEST;
	}

	void PollAnimation(AnimData* animData)
	{
		auto* state = animData ? ActorOverrides::Peek(animData) : nullptr;
		if (!state || state->polledPlays.empty())
			return;
		// Played after the loop: playing a group records it again (OnGroupPlayed), which changes the list.
		UInt16 replays[std::size(animData->animSequence)];
		size_t numReplays = 0;
		auto& plays = state->polledPlays;
		for (auto iter = plays.begin(); iter != plays.end();)
		{
			auto& play = *iter;
			// Only while the group still plays in its section. A section keeps its last sequence after it stops.
			const auto* current = animData->animSequence[play.section];
			const auto baseGroupId = static_cast<AnimGroupID>(play.groupId & 0xFF);
			if (!current || !current->animGroup || (current->animGroup->groupID & 0xFF) != baseGroupId || !IsPlaying(current))
			{
				iter = plays.erase(iter);
				continue;
			}
			++iter;
			// A start or stop animation of the group (TransitionAnims) can come from another set than the group's own
			// animation. The set decides which animation it continues with when it ends.
			if (TransitionAnims::Of(current) != TransitionKind::None)
				continue;
			// The group as the actor would play it now. It can be another variant than the one that plays: the first
			// person skeleton plays the variant it has (1hpfastforward for a rifle, mtidle from a script's PlayGroup
			// Idle), and an override whose condition holds now can add a more specific one.
			const auto groupId = GetNearestGroupID(animData, baseGroupId);
			if (groupId == Overrides::kInvalidGroupId)
				continue;
			auto* now = Overrides::FindForGroup(groupId, animData);
			if (play.tried && (play.triedGroup != groupId || play.triedSet != now))
				play.tried = false;
			if (groupId == play.groupId && now == play.set)
				continue;
			// A polled set's condition started to hold and it is the one that plays now, or the polled set that played
			// no longer applies. A set without pollCondition that starts to hold above the playing one waits for the
			// group's next play.
			const bool polledNow = now && now != play.set && now->pollCondition;
			const bool polledLeft = play.set && play.set->pollCondition && !Overrides::Passes(*play.set, play.groupId, animData);
			// Once per change: the engine may play another variant than the one asked for (one it cannot load).
			if ((!polledNow && !polledLeft) || play.tried)
				continue;
			play.set = now;
			play.tried = true;
			play.triedGroup = groupId;
			play.triedSet = now;
			replays[numReplays++] = groupId;
		}
		for (size_t i = 0; i < numReplays; ++i)
			GameFuncs::PlayAnimGroup(animData, replays[i], 1, -1, -1);
	}

	// The instructions the jump at Actor::PickAnimations (0x895110) replaces, then the rest of the function.
	__declspec(naked) void PickAnimationsOriginal()
	{
		__asm
		{
			push ebp
			mov ebp, esp
			sub esp, 0x18C
			mov eax, 0x895119
			jmp eax
		}
	}

	// Actor::PickAnimations(float walkSpeed, float turnSpeed): the player's on the main thread, other actors' on AI
	// linear task thread 1 (ProcessLists::DoPickAnims, Actor::UpdateAnimation, HighProcess::Update, ...).
	void __fastcall PickAnimationsHook(Actor* actor, void*, float walkSpeed, float turnSpeed)
	{
		ThisStdCall(reinterpret_cast<UInt32>(&PickAnimationsOriginal), actor, walkSpeed, turnSpeed);
		Overrides::Poll(actor);
	}
}

void Overrides::OnGroupPlayed(AnimData* animData, UInt16 groupId, SavedAnims* set, bool pollable)
{
	if (!animData || !animData->actor)
		return;
	const auto baseGroupId = static_cast<AnimGroupID>(groupId & 0xFF);
	if (baseGroupId >= g_animGroupInfos.size())
		return;
	const auto section = GetGroupInfo(baseGroupId)->sequenceType;
	if (section >= std::size(animData->animSequence))
		return;
	const auto polled = pollable && HasPolledSets() && IsPolledGroup(groupId);
	auto* state = polled ? ActorOverrides::Get(animData) : ActorOverrides::Peek(animData);
	if (!state)
		return;
	ActorOverrides::PolledPlay play{ set, groupId, static_cast<UInt8>(section) };
	if (const auto previous = std::ranges::find(state->polledPlays, play.section, &ActorOverrides::PolledPlay::section);
		previous != state->polledPlays.end())
	{
		// What Poll played last stays tried, so a variant the engine plays instead is not asked for again.
		play.tried = previous->tried;
		play.triedGroup = previous->triedGroup;
		play.triedSet = previous->triedSet;
		state->polledPlays.erase(previous);
	}
	if (!polled)
		return;
	const auto firstPerson = animData == PlayerCharacter::GetSingleton()->firstPersonAnimData;
	const auto hasPolled = [&](UInt16 lookupGroupId)
	{
		return lookupGroupId != kInvalidGroupId && HasGroup(firstPerson, lookupGroupId)
			&& state->HasPolledCandidates(animData->actor, firstPerson, lookupGroupId);
	};
	// Poll plays the group as the actor would play it now (GetNearestGroupID), which can be another variant.
	if (hasPolled(groupId) || (baseGroupId != groupId && hasPolled(baseGroupId))
		|| [&] { const auto nearest = GetNearestGroupID(animData, baseGroupId); return nearest != groupId && hasPolled(nearest); }())
		state->polledPlays.push_back(play);
}

void Overrides::Poll(Actor* actor)
{
	if (IsActorInvalid(actor))
		return;
	PollAnimation(actor->baseProcess->GetAnimData());
	if (auto* player = PlayerCharacter::GetSingleton(); actor == player)
		PollAnimation(player->firstPersonAnimData);
}

void Overrides::WriteHooks()
{
	WriteRelCall(0x493DC0, InitGroupSpeedLookupHook);
	WriteRelCall(0x571982, VanillaGroupLoaded);
	WriteRelCall(0x913B88, VanillaGroupLoaded);
	WriteRelCall(0x913C87, VanillaGroupLoaded);
	WriteRelJump(0x895110, PickAnimationsHook);
}
