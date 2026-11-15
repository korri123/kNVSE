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
#include "sequence_extradata.h"
#include "string_view_util.h"
#include "utility.h"

namespace
{
	// The Animations that have a state, and those of them with polled sets: for the main thread to go through
	std::mutex s_statesMutex;
	std::vector<ActorOverrides*> s_states;
	std::vector<ActorOverrides*> s_polledStates;

	// The AnimSequenceSingle of the KF being loaded, for Animation::InitGroupSpeed: it looks the group up in the
	// Animation's map, which an override is never in
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

	// Animation::GroupLoaded where the engine decides whether to load the KFs of a weapon type for an actor (attaching
	// a weapon while its 3D loads on a task thread, queueing an item's files): asked of the Animation's own map only.
	// An override of the weapon's Equip counted as "loaded", and the actor never got the type's other animations
	bool __fastcall VanillaGroupLoaded(AnimData* animData, void*, UInt16 groupId)
	{
		AnimSequenceBase* base = nullptr;
		return GameFuncs::NiTPointerMap_Lookup(animData->mapAnimSequenceBase, groupId, &base);
	}

	// The threads that load in the background: IOManager's task threads and ModelLoader's clone thread. An actor's 3D is
	// built there (QueuedReference::BackgroundClone -> Actor::Load3D), which asks about its animations while the main
	// thread runs the game
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

	// Special and dynamic idles (TESIdleForm, hit reactions, Pip-Boy) all share one anim group: an override replaces only
	// the idle whose KF has its file name. Animation::SpecialIdlePlay adds the idle's sequence to the map before playing
	// the group.
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

	// Aim, AimUp and AimDown (and their IS groups, the attacks, the throws) are three groups in a row that play together:
	// the first of each three
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

	// Which of a set's files can play for the group now
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
				const auto lastReload = OnReloadHandler::GetLastReloadForActor(animData->actor);
				didAmmoSwap = lastReload == ReloadType::AmmoSwap;
				didPartialReload = lastReload == ReloadType::Partial;
			}
		}

		bool operator()(const AnimPath& animPath) const
		{
			// start and stop animations only play through MovementTransitions and AimTransitions
			if (animPath.isStartAnim || animPath.isStopAnim)
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
		// a set of one file plays it, whatever the reload's kind
		if (set.anims.size() == 1 && !filter.idleFileStem && !set.hasTransitionAnims) [[likely]]
			return { state && state->HasFailed(set.anims[0].path.data()) ? nullptr : &set.anims[0] };

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
		// the up and down groups follow the variant their aim (attack) played
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

	// The chosen file plays: the next play chooses again
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

	// What Animation::AddAnimation (0x490500) does with a KF, but for its entry in the Animation's map of groups: the
	// sequence is the KF model's own while nothing else refers to it, else a clone; an AnimSequenceSingle holds it (and
	// the use of the KF model that loading counted); it joins the controller manager; the group's speed is taken from
	// its root motion. kNVSE used to call AddAnimation with the Animation's map swapped for an empty one, under a lock
	// that kept other kNVSE loads out but not the engine.
	Overrides::LoadedAnim CreateSequence(const char* path, AnimData* animData)
	{
		auto* manager = animData->controllerManager;
		BSAnimGroupSequence* sequence;
		AnimSequenceBase* base;
		TESAnimGroup* animGroup;
		{
			// one thread at a time decides who has the KF model's own sequence
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
			// the engine has no Clamp animations in locomotion; start and stop animations play once
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
				// nothing but the KF model refers to it, so no manager has it: not one that was removed without its
				// owner being cleared either
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
		// the node had another Animation: what is kept is of that one's controller manager
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
		Unlist(s_polledStates, this);
	}
	KFLoader::DropPreload(preload);
	if (!loaded.empty())
	{
		std::scoped_lock lock(KFLoader::Mutex());
		for (auto& entry : loaded)
		{
			// read for the player's attack loops (DecreaseAttackTimer): its sequence may be freed below
			if (g_lastLoopSequence == entry.sequence.data)
				g_lastLoopSequence = nullptr;
			// ~AnimSequenceSingle: releases the sequence and the use of its KF model
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
	polled.clear();
	polledScratch.clear();
	polledListed = false;
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
		// The sections keep their sequence after it stopped playing, and pLastMovementSequence the one ClearGroup
		// deactivated: once Release frees the sequence, Animation::Update would read it
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
		// NiControllerManager::RemoveSequence: its targets are cleared, followers released and kNVSE's data of it dropped
		// (SequenceExtraDatas' hook there)
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
	// The player's weapon, or the sets registered for it, changed: its KFs are read ahead by a worker thread, so
	// that the first draw, aim, shot and reload do not each read a file on the main thread. Other actors' load
	// when they first play, as they share most of them and hold few weapons for long. The new batch is queued
	// before the old one is dropped: what both have stays loaded
	if (actor == g_thePlayer)
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
	memoGroups.push_back(groupId);
	memos.push_back({ begin, count });
	return { candidates.data() + begin, count };
}

void ActorOverrides::Track(SavedAnims& set, UInt16 groupId)
{
	const auto iter = std::ranges::find(polled, &set, &Polled::set);
	if (iter != polled.end())
	{
		iter->groupId = groupId;
		return;
	}
	polled.push_back({ &set, groupId });
	if (!polledListed)
	{
		polledListed = true;
		std::scoped_lock lock(s_statesMutex);
		s_polledStates.push_back(this);
	}
}

void ActorOverrides::UpdatePolled()
{
	auto* actor = static_cast<Actor*>(LookupFormByRefID(actorId));
	if (!animData || IsActorInvalid(actor))
	{
		polled.clear();
		return;
	}
	// playing a group below tracks its polled sets again: what stays is tracked again too
	polledScratch.clear();
	std::swap(polled, polledScratch);
	for (const auto [set, groupId] : polledScratch)
	{
		auto* conditionScript = set->conditionScript.load(std::memory_order_acquire);
		if (!conditionScript || set->disabled.load(std::memory_order_relaxed))
			continue;
		const auto* animInfo = GetGroupInfo(static_cast<AnimGroupID>(groupId));
		auto* curAnim = animData->animSequence[animInfo->sequenceType];
		if (!curAnim || !curAnim->animGroup)
			continue;
		// only while the group is the one playing in its section
		const UInt16 currentGroupId = curAnim->animGroup->groupID;
		if ((currentGroupId & 0xFF) != (groupId & 0xFF) || !AnimGroup::FallbacksTo(animData, groupId, currentGroupId))
			continue;
		// a start or stop animation of the group plays (TransitionAnims), which can come from another set than the
		// group's animation (one with only those): playing the group for this set would start it over on every frame
		// (a walk easing out and the player moving on for as long, an aim transition asked to turn around). The
		// condition picks the animation it goes on to
		if (TransitionAnims::Of(curAnim) != TransitionKind::None)
		{
			Track(*set, groupId);
			continue;
		}
		NVSEArrayVarInterface::Element arrResult;
		if (!CallFunction(conditionScript, actor, nullptr, &arrResult))
		{
			Track(*set, groupId);
			continue;
		}
		const auto result = static_cast<bool>(arrResult.GetNumber());
		const auto playGroup = [&]
		{
			const auto nextGroupId = GetNearestGroupID(animData, static_cast<AnimGroupID>(groupId & 0xFF));
			GameFuncs::PlayAnimGroup(animData, nextGroupId, 1, -1, -1);
		};
		if (set->ContainsAnim(curAnim))
		{
			if (result)
				Track(*set, groupId);
			else
				// the set's animation plays and its condition no longer holds: play what the group has now (which tracks
				// this set again if it is tried)
				playGroup();
			continue;
		}
		Track(*set, groupId);
		// Another animation plays and the condition holds now: play the group, if that picks this set. A set above it
		// whose conditions hold too keeps playing (the group used to be played again every frame for as long)
		if (result && Overrides::Find(groupId, animData) == set)
			playGroup();
	}
	polledScratch.clear();
}

SavedAnims* Overrides::Find(UInt16 groupId, AnimData* animData, TransitionKind transition)
{
	if (!FileAnimations::IsLoaded()) [[unlikely]]
		FileAnimations::WaitUntilLoaded();
	if (!animData || !animData->actor || !animData->actor->baseProcess)
		return nullptr;
	const auto firstPerson = animData == g_thePlayer->firstPersonAnimData;
	if (!HasGroup(firstPerson, groupId))
		return nullptr;

	auto* actor = animData->actor;
	auto* state = ActorOverrides::Get(animData);
	// A copy: a condition script may ask about the actor's animations, which can move the state's
	constexpr size_t kInline = 32;
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
		if (set->disabled.load(std::memory_order_relaxed) || !set->MatchesFolderCondition(actor))
			continue;
		if ((set->hasTransitionAnims || transition != TransitionKind::None) && !TransitionAnims::HasPaths(*set, transition))
			continue;
		if (idleFileStem && std::ranges::none_of(set->anims, [&](const AnimPath& animPath) { return IsIdleFileStem(animPath, *idleFileStem); }))
			continue;
		if (set->conditionScript.load(std::memory_order_relaxed) || !set->conditionScriptText.empty())
		{
			if (IsLoaderThread())
			{
#if _DEBUG
				ERROR_LOG_ONCE(FormatString("Overrides: the conditional set of %s was passed over on a loader thread (group %X of actor %X)",
					set->anims.empty() ? "?" : set->anims[0].path.data(), groupId, actor->refID));
#endif
				continue;
			}
			auto* conditionScript = GetCondition(*set);
			if (!conditionScript)
				continue;
			// start and stop animations are looked up when one is due: the poll plays the set's group.
			// Tracked whether or not the condition holds, so that it can come to hold
			if (set->pollCondition && transition == TransitionKind::None && state)
				state->Track(*set, groupId);
			NVSEArrayVarInterface::Element result;
			if (!CallFunction(conditionScript, actor, nullptr, &result) || result.GetNumber() == 0.0)
				continue;
		}
		if (!set->anims.empty())
			return set;
	}
	return nullptr;
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
		// The engine removed the sequence from the manager: Animation::AnimIdleFree removes the sequence a special or
		// dynamic idle played, which is this one when it replaced the idle's KF. Started without an owner it would never
		// update, and the idle that plays it would never end
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
	// a file that does not load is passed over from then on: the next one is chosen
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

void Overrides::UpdatePolled()
{
	// the states can list themselves while a group plays
	static std::vector<ActorOverrides*> s_updating;
	{
		std::scoped_lock lock(s_statesMutex);
		if (s_polledStates.empty())
			return;
		s_updating.assign(s_polledStates.begin(), s_polledStates.end());
	}
	for (auto* state : s_updating)
	{
		state->UpdatePolled();
		if (state->polled.empty())
		{
			std::scoped_lock lock(s_statesMutex);
			Unlist(s_polledStates, state);
			state->polledListed = false;
		}
	}
}

void Overrides::UpdatePlayerPreload()
{
	if (!FileAnimations::IsLoaded() || !g_thePlayer || !g_thePlayer->baseProcess)
		return;
	for (auto* animData : { g_thePlayer->firstPersonAnimData, g_thePlayer->baseProcess->GetAnimData() })
	{
		if (!animData || animData->actor != g_thePlayer)
			continue;
		if (auto* state = ActorOverrides::Get(animData))
			state->Refresh(g_thePlayer, animData == g_thePlayer->firstPersonAnimData);
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

void Overrides::WriteHooks()
{
	WriteRelCall(0x493DC0, InitGroupSpeedLookupHook);
	WriteRelCall(0x571982, VanillaGroupLoaded);
	WriteRelCall(0x913B88, VanillaGroupLoaded);
	WriteRelCall(0x913C87, VanillaGroupLoaded);
}
