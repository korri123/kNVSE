#include "commands_animation.h"

#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <filesystem>
#include <set>
#include <span>

#include "file_animations.h"
#include "GameForms.h"
#include "GameAPI.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "hooks.h"
#include "main.h"
#include "MemoizedMap.h"
#include "utility.h"
#include "PluginAPI.h"
#include "stack_allocator.h"
#include <cassert>
#include <shared_mutex>

#include "additive_anims.h"
#include "anim_fixes.h"
#include "blend_fixes.h"
#include "nihooks.h"
#include "NiNodes.h"
#include "NiObjects.h"
#include "NiTypes.h"
#include "ScriptUtils.h"
#include "sequence_extradata.h"
#include "string_view_util.h"

std::span<AnimGroupInfo> g_animGroupInfos = { reinterpret_cast<AnimGroupInfo*>(0x11977D8), 245 };

AnimOverrideMap g_animGroupThirdPersonMap;
AnimOverrideMap g_animGroupFirstPersonMap;


// mod index map
AnimOverrideMap g_animGroupModIdxThirdPersonMap;
AnimOverrideMap g_animGroupModIdxFirstPersonMap;

AnimData* GetAnimDataForPov(UInt32 playerPov, Actor* actor)
{
	if (actor == g_thePlayer && playerPov)
	{
		if (playerPov == 1)
			return g_thePlayer->baseProcess->GetAnimData();
		if (playerPov == 2)
			return g_thePlayer->firstPersonAnimData;
		return nullptr;
	}
	return actor->GetAnimation();
}

bool Cmd_ForcePlayIdle_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;
	UInt32 playerPov = 0;
	auto* actor = reinterpret_cast<Actor*>(thisObj);
	if (!ExtractArgs(EXTRACT_ARGS, &form, &playerPov) || !actor->IsActor() || !actor->baseProcess)
		return true;
	auto* idle = DYNAMIC_CAST(form, TESForm, TESIdleForm);
	if (!idle)
		return true;
	//SafeWrite8(0x497FA7 + 1, 1);
	actor->baseProcess->SetQueuedIdleForm(idle); // required so IsIdlePlayingEx works
	auto* animData = GetAnimDataForPov(playerPov, actor);
	if (!animData)
		return true;
	GameFuncs::PlayIdle(animData, idle, actor, idle->data.groupFlags & 0x3F, 3); // Exactly like 0x8BB1F4
	//SafeWrite8(0x497FA7 + 1, 0);
	*result = 1;
	return true;
}

bool Cmd_ForceStopIdle_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 playerPov = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &playerPov))
		return true;
	auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
	if (!actor)
		return true;
	auto* animData = GetAnimDataForPov(playerPov, actor);
	if (!animData)
		return true;
	
	//actor->baseProcess->StopIdle(actor);
	ThisStdCall(0x498910, animData, true, false);
	*result = 1;
	return true;
}

GameAnimMap* CreateGameAnimMap(UInt32 bucketSize)
{
	// see 0x48F9F1
	auto* alloc = static_cast<GameAnimMap*>(FormHeap_Allocate(0x10));
	return GameFuncs::NiTPointerMap_Init(alloc, bucketSize);
}

BSAnimGroupSequence* GetGameAnimation(AnimData* animData, UInt16 groupID)
{
	auto* seqBase = animData->mapAnimSequenceBase->Lookup(groupID);
	if (seqBase)
	{
		return seqBase->GetSequenceByIndex(-1);
	}
	return nullptr;
}

BSAnimGroupSequence* GetAnimationByPath(const char* path)
{
	const auto* kfModel = ModelLoader::LoadKFModel(path);
	return kfModel ? kfModel->controllerSequence : nullptr;
}

// intentional const char*, anim paths are pooled and their pointers remain consistent throughout lifetime
std::unordered_map<std::pair<const char*, AnimData*>, BSAnimationContext, pair_hash, pair_equal> g_cachedAnimMap;


#if _DEBUG
NiTPointerMap_t<const char*, NiAVObject*>::Entry entry;
#endif

/*
 * BSAnimGroupSequence ref count is 3 after LoadAnimation; it has the following references:
 * - The AnimSequenceBase
 * - The KFModel
 * - The NiControllerManager sequence list
 * After a call to DeleteAnimSequence it will be reduced to 1, only KFModel reference persists and is reused next time
 * when LoadAnimation is called.
 */
std::shared_mutex g_loadCustomAnimationMutex;

void HandleOnAnimDataDelete(AnimData* animData)
{
	const auto actorId = animData->actor ? animData->actor->refID : 0;
	if (actorId)
	{
		std::unique_lock lock(g_animTimeMutex);
		std::erase_if(g_timeTrackedAnims, _L(auto& p, p.second->actorId == actorId));
		std::erase_if(g_burstFireQueue, _L(auto& p, p.actorId == actorId));
	}
	
	{
		std::unique_lock lock(g_pollConditionMutex);
		std::erase_if(g_timeTrackedGroups, _L(auto & iter, iter.second->animData == animData));
	}
	
	{
		std::unique_lock lock(g_loadCustomAnimationMutex);
		std::erase_if(g_cachedAnimMap, _L(auto& p, p.first.second == animData));
	}
}

thread_local GameAnimMap* s_customMap = nullptr;

std::optional<BSAnimationContext> LoadCustomAnimation(std::string_view path, AnimData* animData)
{
	const auto key = std::make_pair(path.data(), animData);
	{
		std::shared_lock lock(g_loadCustomAnimationMutex);
		if (const auto iter = g_cachedAnimMap.find(key); iter != g_cachedAnimMap.end())
		{
			return iter->second;
		}
	}

	const auto tryCreateAnimation = [&]() -> std::optional<BSAnimationContext>
	{
		auto* kfModel = ModelLoader::LoadKFModel(path.data());
		if (kfModel && kfModel->animGroup && animData)
		{
			const auto groupId = kfModel->animGroup->groupID;

			if (auto* base = animData->mapAnimSequenceBase->Lookup(groupId))
			{
				// fix memory leak, can't previous anim in map since it might be blending
				auto* anim = base->GetSequenceByIndex(-1);
				if (anim && _stricmp(anim->m_kName, path.data()) == 0)
					return BSAnimationContext(anim, base);
				GameFuncs::NiTPointerMap_RemoveKey(animData->mapAnimSequenceBase, groupId);
			}
			const auto oldMovementQueuedGroupId = animData->queuedGroupIDs[kSequence_Movement];
			if (GameFuncs::LoadAnimation(animData, kfModel, false))
			{
				if (animData->queuedGroupIDs[kSequence_Movement] != oldMovementQueuedGroupId)
					// hack fix for hack fix at 0x4946D2
					animData->queuedGroupIDs[kSequence_Movement] = oldMovementQueuedGroupId;
				if (auto* base = animData->mapAnimSequenceBase->Lookup(groupId))
				{
					BSAnimGroupSequence* anim;
					if (base && ((anim = base->GetSequenceByIndex(-1))))
					{
						auto iter = g_cachedAnimMap.emplace(key, BSAnimationContext(anim, base));
						return iter.first->second;
					}
					ERROR_LOG("Map returned null anim " + std::string(path));
				}
				else
					ERROR_LOG("Failed to lookup anim " + std::string(path));
			}
			else
				ERROR_LOG(FormatString("Failed to load anim %s for anim data of actor %X", path.data(), animData->actor->refID));
		}
		else
			ERROR_LOG("Failed to load KF Model " + std::string(path));
		return std::nullopt;
	};

	std::unique_lock lock(g_loadCustomAnimationMutex);
	auto* defaultMap = animData->mapAnimSequenceBase;

	if (!s_customMap)
		s_customMap = CreateGameAnimMap(1);
	
	animData->mapAnimSequenceBase = s_customMap;
	const auto result = tryCreateAnimation();
	animData->mapAnimSequenceBase = defaultMap;
	s_customMap->Clear();
	lock.unlock();

	if (result && result->anim)
		AnimFixes::FixWrongAKeyInRespectEndKey(animData, result->anim);
	
	return result;
}


std::optional<BSAnimationContext> LoadCustomAnimation(SavedAnims& animBundle, UInt16 groupId, AnimData* animData)
{
	if (const auto* animPath = GetAnimPath(animBundle, groupId, animData))
	{
		const auto animCtx = LoadCustomAnimation(animPath->path, animData);
		if (animCtx)
			animBundle.linkedSequences.insert(animCtx->anim);
		return animCtx;
	}
	return std::nullopt;
}

AnimTime::~AnimTime()
{
	Revert3rdPersonAnimTimes(this->respectEndKeyData.anim3rdCounterpart, this->anim);
	if ((this->trackEndTime || this->isOverlayAdditiveAnim) && this->anim->m_eState != NiControllerSequence::EASEOUT && this->anim->m_eState != NiControllerSequence::TRANSSOURCE)
	{
		if (this->trackEndTime && !this->isOverlayAdditiveAnim && AdditiveManager::IsAdditiveSequence(this->anim))
			// PlayAnimationPath anims
			AdditiveManager::RemoveAdditiveTransformsFlags(this->anim);
		const auto easeOutTime = this->anim->GetEaseOutTime();
		this->anim->Deactivate(easeOutTime, false);
	}
}

// Make sure that Aim, AimUp and AimDown all use the same index
AnimPath* HandleAimUpDownRandomness(UInt32 animGroupId, std::vector<AnimPath*>& anims)
{
	UInt32 baseId;
	if (const auto animGroupMinor = animGroupId & 0xFF; animGroupMinor >= kAnimGroup_Aim && animGroupMinor <= kAnimGroup_AimISDown && (baseId = kAnimGroup_Aim)
		|| animGroupMinor >= kAnimGroup_AttackLeft && animGroupMinor <= kAnimGroup_AttackSpin2ISDown && (baseId = kAnimGroup_AttackLeft) 
		|| animGroupMinor >= kAnimGroup_PlaceMine && animGroupMinor <= kAnimGroup_AttackThrow8ISDown && (baseId = kAnimGroup_PlaceMine))
	{
		static unsigned int s_lastRandomId = 0;
		if ((animGroupMinor - baseId) % 3 == 0 || s_lastRandomId >= anims.size())
			s_lastRandomId = GetRandomUInt(anims.size());

		return anims.at(s_lastRandomId);
	}
	return nullptr;
}

std::list<BurstFireData> g_burstFireQueue;

TimeTrackedAnimsMap g_timeTrackedAnims;
TimeTrackedGroupsMap g_timeTrackedGroups;

void EraseTimeTrackedAnim(BSAnimGroupSequence* anim)
{
	std::unique_lock lock(g_animTimeMutex);
	std::erase_if(g_timeTrackedAnims, [anim](const auto& p)
	{
		return p.second->anim == anim;
	});
}

enum class KeyCheckType
{
	KeyEquals, KeyStartsWith
};

/*
 * 	std::unique_ptr<TimedExecution<Script*>> scriptLineKeys = nullptr;
	std::unique_ptr<TimedExecution<Script*>> scriptCallKeys = nullptr;
	std::unique_ptr<TimedExecution<Sound>> soundPaths = nullptr;
 */

std::unordered_map<BSAnimGroupSequence*, TimedExecution<Script*>> g_scriptLineExecutions;
std::unordered_map<BSAnimGroupSequence*, TimedExecution<Script*>> g_scriptCallExecutions;
std::unordered_map<BSAnimGroupSequence*, TimedExecution<BSSoundHandle>> g_scriptSoundExecutions;
std::recursive_mutex g_animTimeMutex;

AnimTime* HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* anim, bool createIfNoKeys)
{
	std::unique_lock lock(g_animTimeMutex);
	if (!anim)
		return nullptr;
	auto applied = false;
	const auto textKeys = anim->m_spTextKeys->GetKeys();
	auto* actor = animData->actor;
	AnimTime* animTimePtr = nullptr;
	BSAnimGroupSequence* baseAnim = nullptr;
	const auto getAnimTimeStruct = [&]() -> AnimTime&
	{
		if (!animTimePtr)
		{
			const auto iter = g_timeTrackedAnims.emplace(anim, std::make_unique<AnimTime>(actor, anim));
			animTimePtr = iter.first->second.get();
		}
		return *animTimePtr;
	};
	const auto createTimedExecution = [&]<typename T>(std::unordered_map<BSAnimGroupSequence*, T>& map)
	{
		if (!baseAnim)
		{
			// idle anims are destroyed after they are done playing, so we can't rely on their pointers being the same in the map
			auto* kfModel = ModelLoader::LoadKFModel(anim->m_kName.CStr());
			if (kfModel)
				baseAnim = kfModel->controllerSequence;
			else
				ERROR_LOG("Failed to load kf model in HandleExtraOperations for " + std::string(anim->m_kName.CStr()));
		}
		auto* key = baseAnim ? baseAnim : anim;
		const auto iter = map.emplace(key, T());
		auto* timedExecution = &iter.first->second;
		const bool uninitialized = iter.second;
		return std::make_pair(timedExecution, uninitialized);
	};
	const auto hasKey = [&](const std::initializer_list<const char*> keyTexts, KeyCheckType type = KeyCheckType::KeyEquals, float* time = nullptr)
	{
		auto iter = textKeys.end();
		for (const char* keyText : keyTexts)
		{
			switch (type)
			{
			case KeyCheckType::KeyEquals:
				iter = ra::find_if(textKeys, _L(NiTextKey &key, _stricmp(key.m_kText.CStr(), keyText) == 0));
				break;
			case KeyCheckType::KeyStartsWith:
				iter = ra::find_if(textKeys, _L(NiTextKey &key, StartsWith(key.m_kText.CStr(), keyText)));
				break;
			}
			if (iter != textKeys.end())
				break;
		}
		if (iter != textKeys.end())
		{
			applied = true;
			if (time)
				*time = iter->m_fTime;
			return true;
		}
		
		return false;
	};

	if (anim->animGroup && anim->animGroup->IsAttack() && hasKey({"burstFire"}))
	{
		std::vector<NiTextKey*> hitKeys;
		std::vector<NiTextKey*> ejectKeys;
		const auto parseForKeys = [&](const char* keyName, std::vector<NiTextKey*>& keys)
		{
			bool skippedFirst = false;
			for (auto& key : textKeys)
			{
				if (_stricmp(key.m_kText.CStr(), keyName) == 0)
				{
					if (!skippedFirst)
					{
						// engine handles first key
						skippedFirst = true;
						continue;
					}
					keys.push_back(&key);
				}
			}
		};
		parseForKeys("hit", hitKeys);
		parseForKeys("eject", ejectKeys);
		if (!hitKeys.empty() || !ejectKeys.empty())
		{
			g_burstFireQueue.emplace_back(animData == g_thePlayer->firstPersonAnimData, anim, 0, std::move(hitKeys), 0.0,false, -FLT_MAX, animData->actor->refID, std::move(ejectKeys), 0, false);
		}
	}
	const auto hasRespectEndKey = hasKey({"respectEndKey", "respectTextKeys"});
	if (animData == g_thePlayer->firstPersonAnimData && anim->animGroup && hasRespectEndKey)
	{
		auto& animTime = getAnimTimeStruct();
		auto& respectEndKeyData = animTime.respectEndKeyData;
		respectEndKeyData.povState = POVSwitchState::NotSet;
		animTime.respectEndKey = true;
	}
	const auto baseGroupID = anim->animGroup ? anim->animGroup->GetBaseGroupID() : kAnimGroup_Invalid;

	if (hasKey({"interruptLoop"}) && (baseGroupID == kAnimGroup_AttackLoop || baseGroupID == kAnimGroup_AttackLoopIS))
	{
		// IS allowed so that anims can finish after releasing LMB (handled in hook)
		// *reinterpret_cast<UInt8*>(g_animationHookContext.groupID) = kAnimGroup_AttackLoopIS;
		if (animData == g_thePlayer->firstPersonAnimData)
		{
			g_thePlayer->baseProcess->GetAnimData()->groupIDs[kSequence_Weapon] = kAnimGroup_AttackLoopIS;
		}
		g_lastLoopSequence = anim;
		g_startedAnimation = true;
	}
	if (hasKey({"noBlend"}))
	{
		animData->noBlend120 = true;
	}
	if (hasKey({"Script:"}, KeyCheckType::KeyStartsWith))
	{
		auto [scriptCallKeys, uninitialized] = createTimedExecution(g_scriptCallExecutions);
		if (uninitialized)
		{
			*scriptCallKeys = TimedExecution<Script*>(textKeys, [&](const char* key, Script*& result)
			{
				if (!StartsWith(key, "Script:"))
					return false;
				const auto edid = GetTextAfterColon(key);
				if (edid.empty())
					return false;
				auto* form = GetFormByID(edid.data());
				if (!form || !IS_ID(form, Script))
				{
					ERROR_LOG(FormatString("Text key contains invalid script %s", edid.data()));
					return false;
				}
				result = static_cast<Script*>(form);
				return true;
			});
		}
		auto& animTime = getAnimTimeStruct();
		animTime.scriptCalls = scriptCallKeys->CreateContext(GetAnimTime(anim));
	}
	if (hasKey({"SoundPath:"}, KeyCheckType::KeyStartsWith))
	{
		auto& animTime = getAnimTimeStruct();

		animTime.soundPathsBase = TimedExecution<Sounds>(textKeys, [&](const char* key, Sounds& result)
		{
			const auto sound = Sounds::FromTextKey(animData, key);
			if (!sound)
				return false;
			result = *sound;
			return true;
		});
		animTime.soundPaths = animTime.soundPathsBase->CreateContext(GetAnimTime(anim));
	}
	if (hasKey({"blendToReloadLoop"}))
	{
		LoopingReloadPauseFix::g_reloadStartBlendFixes.insert(anim->m_kName.CStr());
	}
	if (hasKey({"scriptLine:" }, KeyCheckType::KeyStartsWith))
	{
		auto& animTime = getAnimTimeStruct();
		auto [scriptLineKeys, uninitialized] = createTimedExecution(g_scriptLineExecutions);
		if (uninitialized)
		{
			*scriptLineKeys = TimedExecution<Script*>(textKeys, [&](const char* key, Script*& result)
			{
				if (!StartsWith(key, "scriptLine:"))
					return false;
				static std::unordered_map<const char*, Script*> s_scriptLines;
				auto& cached = s_scriptLines[key]; // by pointer value
				if (cached)
				{
					result = cached;
					return true;
				}
				const auto line = std::string(GetTextAfterColon(key));
				if (line.empty())
					return false;
				auto formattedLine = ReplaceAll(line, "%R", "\r\n");
				formattedLine = ReplaceAll(formattedLine, "%r", "\r\n");
				result = Script::CompileFromText(formattedLine, "ScriptLineKey");
				if (!result)
				{
					ERROR_LOG("Failed to compile script in scriptLine key: " + line + " for anim " + std::string(anim->m_kName.CStr()));
					return false;
				}
				cached = result;
				return true;
			});
		}
		animTime.scriptLines = scriptLineKeys->CreateContext(GetAnimTime(anim));
	}
	float time;
	if (hasKey({"allowAttack"}, KeyCheckType::KeyEquals, &time))
	{
		auto& animTime = getAnimTimeStruct();
		animTime.allowAttack = true;
		animTime.allowAttackTime = time;
	}

	const auto basePath = GetAnimBasePath(anim->m_kName.Str());
	if (auto iter = g_customAnimGroupPaths.find(basePath); iter != g_customAnimGroupPaths.end())
	{
		auto& animTime = getAnimTimeStruct();
		animTime.hasCustomAnimGroups = true;
	}

	std::string_view animName(anim->m_kName);
	
	for (const auto& legacyPath : g_pluginSettings.legacyAnimTimePaths)
	{
		if (animName.contains(legacyPath))
		{
			auto& animTime = getAnimTimeStruct();
			animTime.useLegacyTime = true;
			break;
		}
	}
		
	if (applied || createIfNoKeys)
	{
		auto& animTime = getAnimTimeStruct();
		animTime.actorId = actor->refID;
		animTime.lastNiTime = -FLT_MAX;
		animTime.firstPerson = animData == g_thePlayer->firstPersonAnimData;
		return &animTime;
	}
	
	return nullptr;
}

std::map<std::pair<FormID, SavedAnims*>, int> g_actorAnimOrderMap;

void FilterPartialReloads(std::vector<AnimPath*>& candidates, Actor* actor)
{
	const auto didPartialReload = OnReloadHandler::GetLastReloadForActor(actor) == ReloadType::Partial;
	
	candidates = candidates | std::views::filter([=](const auto& animPath) {
		return animPath->partialReload == didPartialReload;
	}) | std::ranges::to<std::vector>();
}

void FilterAmmoSwaps(std::vector<AnimPath*>& candidates, Actor* actor)
{
	const auto didAmmoSwap = OnReloadHandler::GetLastReloadForActor(actor) == ReloadType::AmmoSwap;
	
	candidates = candidates | std::views::filter([=](const auto& animPath)
	{
		return animPath->isAmmoSwap == didAmmoSwap;
	}) | std::ranges::to<std::vector<AnimPath*>>();
}

std::optional<AnimationResult> PickAnimation(AnimOverrideStruct& overrides, UInt16 groupId, AnimData* animData)
{
	if (const auto stacksIter = overrides.stacks.find(groupId); stacksIter != overrides.stacks.end())
	{
		auto& animStacks = stacksIter->second;
		auto* actor = animData->actor;
		
		auto& stack = animStacks.anims;
		for (auto& ctx : ra::reverse_view(stack))
		{
			if (!ctx->MatchesConditions(actor) || ctx->disabled)
				continue;
			const auto initAnimTime = [&](SavedAnims* savedAnims)
			{
				std::unique_lock lock(g_pollConditionMutex);
				auto& animTime = g_timeTrackedGroups[std::make_pair(savedAnims, animData)];
				if (!animTime)
					animTime = std::make_unique<SavedAnimsTime>();
				animTime->conditionScript = *savedAnims->conditionScript;
				animTime->groupId = groupId;
				animTime->actorId = animData->actor->refID;
				animTime->animData = animData;
			};
			if (!ctx->loaded)
				ctx->Load();
			if (ctx->conditionScript)
			{
				if (ctx->pollCondition)
					initAnimTime(ctx.get()); // init'd here so conditions can activate despite not being overridden
				NVSEArrayVarInterface::Element result;
				if (!CallFunction(*ctx->conditionScript, actor, nullptr, &result) || result.GetNumber() == 0.0)
					continue;
			}
			if (!ctx->anims.empty())
			{
				return AnimationResult(ctx.get());
			}
		}
	}
	return std::nullopt;
}

std::optional<AnimationResult> GetAnimationFromMap(AnimOverrideMap& map, UInt32 id, FullAnimGroupID animGroupId, AnimData* animData)
{
	if (const auto mapIter = map.find(id); mapIter != map.end())
	{
		return PickAnimation(mapIter->second, animGroupId, animData);
	}
	return std::nullopt;
}

AnimOverrideMap& GetMap(bool firstPerson)
{
	return firstPerson ? g_animGroupFirstPersonMap : g_animGroupThirdPersonMap;
}

AnimOverrideMap& GetModIndexMap(bool firstPerson)
{
	return firstPerson ? g_animGroupModIdxFirstPersonMap : g_animGroupModIdxThirdPersonMap;
}

// preserve randomization of variants
thread_local AnimPathCache g_animPathFrameCache;

AnimPath* GetAnimPath(SavedAnims& ctx, UInt16 groupId, AnimData* animData)
{
	const auto cacheKey = std::make_pair(&ctx, animData);
	AnimPath** cachePtr = nullptr;
	const auto useCache = g_isThreadCacheEnabled;
	if (useCache)
	{
		const auto [result, isNew] = g_animPathFrameCache.emplace(cacheKey, nullptr);
		if (!isNew)
			return result->second;
		cachePtr = &result->second;
	}

	if (!ctx.loaded)
		ctx.Load();
		
	Actor* actor = animData->actor;
	
	const auto getAnimPath = [&]() -> AnimPath*
	{
		if (ctx.anims.size() == 1) [[likely]]
			return ctx.anims[0].get();
		auto candidates = ctx.anims 
            | std::views::transform([](const auto& anim) { return anim.get(); }) 
            | std::ranges::to<std::vector<AnimPath*>>();

		if (groupId == kAnimGroup_DynamicIdle || groupId == kAnimGroup_SpecialIdle)
		{
			auto* dynamicIdle = animData->mapAnimSequenceBase->Lookup(groupId);
			BSAnimGroupSequence* idleAnimQueued;
			if (dynamicIdle && ((idleAnimQueued = dynamicIdle->GetSequenceByIndex(-1))))
			{
				// handle pip boy dynamic idles
				const auto queuedFileStem = sv::get_file_stem(idleAnimQueued->m_kName.CStr());
				candidates = candidates | std::views::filter([=](const auto& animPath)
				{
					const auto candidateFileStem = sv::get_file_stem(animPath->path);
					return sv::equals_ci(candidateFileStem, queuedFileStem);
				}) | std::ranges::to<std::vector<AnimPath*>>();
			}
		}
		const auto baseGroupId = static_cast<AnimGroupID>(groupId);
		if (ctx.hasStartAnim)
		{
			const auto* groupInfo = GetGroupInfo(baseGroupId);
			auto* anim = animData->animSequence[groupInfo->sequenceType];
			const auto useStartAnim = !anim || !anim->animGroup || anim->animGroup->GetBaseGroupID() != baseGroupId;
			candidates = candidates | std::views::filter([=](const auto& animPath)
			{
				return animPath->isStartAnim == useStartAnim;
			}) | std::ranges::to<std::vector<AnimPath*>>();
		}
		if (IsAnimGroupReload(baseGroupId))
		{
			if (ctx.hasAmmoSwap)
				FilterAmmoSwaps(candidates, actor);
			if (ctx.hasPartialReload)
				FilterPartialReloads(candidates, actor);
		}

		if (candidates.empty())
			return nullptr;
		
		if (candidates.size() == 1)
			return candidates.front();
		
		if (!ctx.hasOrder)
		{
			// Make sure that Aim, AimUp and AimDown all use the same index
			if (auto* path = HandleAimUpDownRandomness(groupId, candidates))
				return path;
			// pick random variant
			return candidates.at(GetRandomUInt(candidates.size()));
		}
		
		// ordered
		return candidates.at(g_actorAnimOrderMap[std::make_pair(actor->refID, &ctx)]++ % candidates.size());
	};

	const auto result = getAnimPath();
	if (useCache)
		*cachePtr = result;
	return result;
}

BSAnimGroupSequence* LoadAnimationPath(const AnimationResult& result, AnimData* animData, UInt16 groupId)
{
	auto& animBundle = *result.animBundle;
	if (const auto animCtx = LoadCustomAnimation(animBundle, groupId, animData))
	{
		BSAnimGroupSequence* anim = animCtx->anim;
		if (!anim)
			return nullptr;
		HandleExtraOperations(animData, anim);
		if (!animBundle.additiveAnimPath.empty())
		{
			if (auto* additiveAnim = FindOrLoadAnim(animData, animBundle.additiveAnimPath.data()))
				AdditiveManager::PlayManagedAdditiveAnim(animData, anim, additiveAnim);
		}
		return anim;
	}
	return nullptr;
}

// clear this cache every frame
thread_local AnimationResultCache g_animationResultCache;

std::shared_mutex g_overrideMapMutex;

std::optional<AnimationResult> GetActorAnimation(FullAnimGroupID animGroupId, AnimData* animData)
{
	// wait for file loading to finish
	if (g_animFileThread.joinable())
		g_animFileThread.join();

	if (!animData || !animData->actor || !animData->actor->baseProcess)
		return std::nullopt;
	const auto cacheKey = std::make_pair(animGroupId, animData);
	std::optional<AnimationResult>* cachePtr = nullptr;
	const auto useCache = g_isThreadCacheEnabled;
	if (useCache)
	{
		const auto [result, isNew] = g_animationResultCache.emplace(cacheKey, std::nullopt);
		if (!isNew)
			return result->second;
		cachePtr = &result->second;
	}
#if _DEBUG
	int _debug = 0;
#endif
	
	const auto getActorAnimation = [&](FullAnimGroupID animGroupId) -> std::optional<AnimationResult>
	{
		std::shared_lock lock(g_overrideMapMutex);
		
		std::optional<AnimationResult> result;
		std::optional<AnimationResult> modIndexResult;
		StackVector<UInt8, 8> visitedModIndices;

		const auto firstPerson = animData == g_thePlayer->firstPersonAnimData;
		auto& map = GetMap(firstPerson);
		auto* actor = animData->actor;
		auto& modIndexMap = GetModIndexMap(firstPerson);
		const auto getFormAnimation = [&](TESForm* form) -> std::optional<AnimationResult>
		{
			if (auto lResult = GetAnimationFromMap(map, form->refID, animGroupId, animData))
				return lResult;
			// mod index
			if (!modIndexResult.has_value() && ra::find(*visitedModIndices, form->GetModIndex()) == visitedModIndices->end())
			{
				modIndexResult = GetAnimationFromMap(modIndexMap, form->GetModIndex(), animGroupId, animData);
				visitedModIndices->push_back(form->GetModIndex());
			}
			return std::nullopt;
		};
		// actor ref ID
		if ((result = getFormAnimation(actor)))
			return result;
		// weapon form ID
		if (auto* weaponInfo = actor->baseProcess->GetWeaponInfo(); weaponInfo && weaponInfo->weapon && (result = getFormAnimation(weaponInfo->weapon)))
			return result;
		// base form ID
		if (auto* baseForm = actor->baseForm; baseForm && ((result = getFormAnimation(baseForm))))
			return result;
		// race form ID
		auto* race = actor->GetRace();
		if (race && ((result = getFormAnimation(race))))
			return result;
		// actor base
		auto* actorBase = actor->GetActorBase();
		if (actorBase && ((result = getFormAnimation(actorBase))))
			return result;
		
		// mod index (set in getFormAnimation if exists)
		if (modIndexResult)
			return modIndexResult;
		// non-form ID dependent animations (global replacers)
		if ((result = PickAnimation(modIndexMap[0xFF], animGroupId, animData)))
			return result;
		return std::nullopt;
	};
	const auto result = getActorAnimation(animGroupId);
	if (useCache)
		*cachePtr = result;
	return result;
}

std::span<const char*> s_moveTypeNames{ reinterpret_cast<const char**>(0x1197798), 3 };
std::span<const char*> s_handTypeNames{ reinterpret_cast<const char**>(0x11977A8), 11 };

std::string_view GetBaseAnimGroupName(const std::string_view name)
{
	std::string_view oName = name;
	if (StartsWith(oName, "pa"))
		oName = oName.substr(2);
	for (auto* moveTypeName : s_moveTypeNames)
	{
		if (StartsWith(oName, moveTypeName))
		{
			oName = oName.substr(strlen(moveTypeName));
			break;
		}
	}
	if (StartsWith(oName, "mt"))
		oName = oName.substr(2);
	else
	{
		for (auto* handTypeName : s_handTypeNames)
		{
			if (StartsWith(oName, handTypeName))
			{
				oName = oName.substr(strlen(handTypeName));
				break;
			}
		}
	}

	return oName;
}

static std::unordered_map<std::string_view, AnimGroupID, CaseInsensitiveHash, CaseInsensitiveEqual> s_animGroupNameToIDMap;


AnimGroupID GroupNameToId(std::string_view name)
{
	if (const auto iter = s_animGroupNameToIDMap.find(name); iter != s_animGroupNameToIDMap.end())
		return iter->second;
	const auto iter = ra::find_if(g_animGroupInfos, _L(AnimGroupInfo& i, sv::equals_ci(i.name, name)));
	if (iter == g_animGroupInfos.end())
		return kAnimGroup_Invalid;
	const auto groupId = iter - g_animGroupInfos.begin();
	assert(groupId < kAnimGroup_Max);
	const auto result = static_cast<AnimGroupID>(groupId);
	s_animGroupNameToIDMap[std::string_view(iter->name)] = result;
	return result;
}

AnimGroupID SimpleGroupNameToId(std::string_view name)
{
	if (const auto underscorePos = name.find('_'); underscorePos != std::string_view::npos)
	{
		name = name.substr(0, underscorePos);
	}
	if (const auto spacePos = name.find(' '); spacePos != std::string_view::npos)
	{
		name = name.substr(0, spacePos);
	}
	return GroupNameToId(name);
}

bool TESAnimGroup::IsLoopingReloadStart() const
{
	switch (GetBaseGroupID())
	{
	case kAnimGroup_ReloadWStart:
	case kAnimGroup_ReloadXStart:
	case kAnimGroup_ReloadYStart:
	case kAnimGroup_ReloadZStart:
		return true;
	default:
		break;
	}
	return false;
}

bool TESAnimGroup::IsLoopingReload() const
{
	const auto animGroupId = GetBaseGroupID();
	return animGroupId >= kAnimGroup_ReloadW && animGroupId <= kAnimGroup_ReloadZ;
}

constexpr UInt16 INVALID_FULL_GROUP_ID = 0xFFFF;

UInt16 GetAnimGroupId(std::string_view path)
{
	const auto toFullId = [&](UInt8 groupId)
	{
		int moveType = 0;
		int animHandType = 0;
		int isPowerArmor = 0;
		CdeclCall(0x5F38D0, path.data(), &moveType, &animHandType, &isPowerArmor); // GetMoveHandAndPowerArmorTypeFromAnimName
		return static_cast<UInt16>(groupId + (moveType << 12) + (isPowerArmor << 15) + (animHandType << 8));
	};
	
	const auto name = sv::get_file_stem(path);
	const auto baseName = GetBaseAnimGroupName(name);
	if (const auto id = SimpleGroupNameToId(baseName); id != kAnimGroup_Invalid)
		return toFullId(id);
	
	// try to load kf model which is slower but if file name is wrong then we have to fall back
	if (const auto* kfModel = ModelLoader::LoadKFModel(path.data()))
		if (kfModel->animGroup)
			return kfModel->animGroup->groupID;
	return INVALID_FULL_GROUP_ID;
}

AnimGroupPathsMap g_customAnimGroupPaths;

std::string_view GetAnimBasePath(std::string_view path)
{
	for (std::string_view it : {"_1stperson", "_male"})
	{
		auto basePath = ExtractUntilStringMatches(path, it, true);
		if (!basePath.empty())
			return basePath;
	}
	return "";
}

std::string_view ExtractCustomAnimGroupName(std::string_view path)
{
	const auto stem = sv::get_file_stem(path);
	const auto pos = stem.find("__");
	if (pos != std::string::npos && pos + 2 < stem.size()) {
		return stem.substr(pos + 2);
	}
	return "";
}

bool RegisterCustomAnimGroupAnim(std::string_view path)
{
	if (!ExtractCustomAnimGroupName(path).empty())
	{
		const auto basePath = GetAnimBasePath(path);
		if (!basePath.empty())
		{
			g_customAnimGroupPaths[std::string(basePath)].insert(std::string(path));
			return true;
		}
	}
	return false;
}

bool SetOverrideAnimation(AnimOverrideData& data, AnimOverrideMap& map)
{
	std::unique_lock lock(g_overrideMapMutex);
	const auto path = data.path;
	const auto groupId = GetAnimGroupId(path);
	if (groupId == INVALID_FULL_GROUP_ID)
	{
		ERROR_LOG(FormatString("Failed to resolve file '%s'", path.data()));
		return false;
	}
	auto& animGroupMap = map[data.identifier];
	auto& stacks = animGroupMap.stacks[groupId];

	auto& stack = stacks.anims;
	const auto findFn = [&](const std::unique_ptr<SavedAnims>& a)
	{
		return ra::any_of(a->anims, _L(const auto& s, sv::equals_ci(path, s->path)));
	};
	
	if (!data.enable)
	{
		// remove from stack
		if (const auto it = ra::find_if(stack, findFn); it != stack.end())
		{
			(*it)->disabled = true;
			return true;
		}
		return false;
	}
	// check if stack already contains path
	if (const auto iter = std::ranges::find_if(stack, findFn); iter != stack.end())
	{
		auto& existingEntry = **iter;
		existingEntry.disabled = false;
		if (data.conditionScript != existingEntry.conditionScript) // hot reload
		{
			existingEntry.conditionScript = data.conditionScript;
			existingEntry.conditionScriptText = data.conditionScriptText;
		}
		// move iter to the top of stack
		std::rotate(iter, std::next(iter), stack.end());
		return true;
	}

	if (RegisterCustomAnimGroupAnim(path))
	{
		// we do not want to add custom anim group anims to the stack or otherwise they'll play
		return true;
	}

	auto folderConditionType = FolderConditionType::None;
	std::function<bool(const Actor*)> folderCondition;
	if (sv::contains_ci(path, R"(\mod1\)"))
	{
		folderConditionType = FolderConditionType::Mod1;
		folderCondition = [&](const Actor* actor) { return actor->HasWeaponWithMod(kWeaponMod_Flag1); };
	}
	else if (sv::contains_ci(path, R"(\mod2\)"))
	{
		folderConditionType = FolderConditionType::Mod2;
		folderCondition = [&](const Actor* actor) { return actor->HasWeaponWithMod(kWeaponMod_Flag2); };
	}
	else if (sv::contains_ci(path, R"(\mod3\)"))
	{
		folderConditionType = FolderConditionType::Mod3;
		folderCondition = [&](const Actor* actor) { return actor->HasWeaponWithMod(kWeaponMod_Flag3); };
	}
	else if (sv::contains_ci(path, R"(\hurt\)"))
	{
		folderConditionType = FolderConditionType::Hurt;
		folderCondition = [&](const Actor* actor) { return actor->HasCrippledLegs(); };
	}
	else if (sv::contains_ci(path, R"(\human\)"))
	{
		folderConditionType = FolderConditionType::Human;
		folderCondition = [&](const Actor* actor) { return actor == g_thePlayer || IS_ID(actor->baseForm, TESNPC); };
	}
	else if (sv::contains_ci(path, R"(\male\)"))
	{
		folderConditionType = FolderConditionType::Male;
		folderCondition = [&](const Actor* actor) { return !actor->IsFemale(); };
	}
	else if (sv::contains_ci(path, R"(\female\)"))
	{
		folderConditionType = FolderConditionType::Female;
		folderCondition = [&](const Actor* actor) { return actor->IsFemale(); };
	}
	
	// if not inserted before, treat as variant; else add to stack as separate set
	auto [_, newItem] = data.groupIdFillSet.emplace(groupId);
	if (newItem || stack.empty() || folderConditionType != stack.back()->folderConditionType)
		stack.emplace_back(std::make_unique<SavedAnims>());
	
	auto& anims = *stack.back();

	if (const auto stem = sv::get_file_stem(path); sv::ends_with_ci(stem, "_additive"))
	{
		anims.additiveAnimPath = path;
		return true;
	}
	
	anims.anims.emplace_back(std::make_unique<AnimPath>(path));
	anims.matchBaseGroupId = data.matchBaseGroupId;
	anims.conditionScript = data.conditionScript;
	anims.pollCondition = data.pollCondition;
	anims.conditionScriptText = data.conditionScriptText;
	anims.folderConditionType = folderConditionType;
	anims.folderCondition = std::move(folderCondition);
	
	return true;
}

bool OverrideFormAnimation(AnimOverrideData& data, bool firstPerson)
{
	auto& map = GetMap(firstPerson);
	return SetOverrideAnimation(data, map);
}

bool OverrideModIndexAnimation(AnimOverrideData& data, bool firstPerson)
{
	auto& map = GetModIndexMap(firstPerson);
	return SetOverrideAnimation(data, map);
}

void PluginOverrideFormAnimation(const TESForm* form, const char* path, bool firstPerson, bool enable, Script* conditionScript, bool pollCondition)
{
	AnimOverrideData animOverrideData = {
		.path = AddStringToPool(path),
		.identifier = form->refID,
		.enable = enable,
		.conditionScript = conditionScript,
		.pollCondition = pollCondition,
		.matchBaseGroupId = false
	};
	OverrideFormAnimation(animOverrideData, firstPerson);
}

bool ClearFormAnimations(TESForm* form)
{
	std::unique_lock lock(g_overrideMapMutex);
	bool result = false;
	for (auto* map : { &g_animGroupFirstPersonMap, &g_animGroupThirdPersonMap })
	{
		if (auto iter = map->find(form->refID); iter != map->end())
		{
			for (auto& stack : iter->second.stacks | std::views::values)
			{
				for (auto& anim : stack.anims)
				{
					anim->disabled = true;
				}
			}
		}
	}
	return result;
}

float GetAnimMult(const AnimData* animData, UInt8 animGroupID)
{
	if (animGroupID < kAnimGroup_Forward || animGroupID > kAnimGroup_TurnRight)
	{
		if (animGroupID == kAnimGroup_Equip || animGroupID == kAnimGroup_Unequip)
		{
			return animData->equipSpeed;
		}
		if (animGroupID < kAnimGroup_Equip || animGroupID > kAnimGroup_Counter)
		{
			if (animGroupID >= kAnimGroup_ReloadA && animGroupID <= kAnimGroup_ReloadZ)
			{
				return animData->weaponReloadSpeed;
			}
		}
		else
		{
			return animData->rateOfFire;
		}
	}
	else
	{
		return animData->movementSpeedMult;
	}
	return 1.0f;
}

bool IsAnimGroupReload(AnimGroupID animGroupId)
{
	return animGroupId >= kAnimGroup_ReloadWStart && animGroupId <= kAnimGroup_ReloadZ;
}

bool IsAnimGroupReload(UInt8 animGroupId)
{
	return IsAnimGroupReload(static_cast<AnimGroupID>(animGroupId));
}

bool IsAnimGroupMovement(AnimGroupID animGroupId)
{
	return GetSequenceType(animGroupId) == kSequence_Movement;
}

bool IsAnimGroupMovement(UInt8 animGroupId)
{
	return IsAnimGroupMovement(static_cast<AnimGroupID>(animGroupId));
}

bool WeaponHasNthMod(Decoding::ContChangesEntry* weaponInfo, TESObjectWEAP* weap, UInt32 mod)
{
	ExtraDataList* xData = weaponInfo->extendData ? weaponInfo->extendData->GetFirstItem() : NULL;
	if (!xData) return 0;
	ExtraWeaponModFlags* xModFlags = GetExtraType((*xData), WeaponModFlags);
	if (!xModFlags) return 0;
	UInt8 modFlags = xModFlags->flags, idx = 3;
	while (idx--) if ((modFlags & (1 << idx)) && (weap->effectMods[idx] == mod)) return 1;
	return 0;
}

int GetWeaponInfoClipSize(Actor* actor)
{
	auto* weaponInfo = reinterpret_cast<Decoding::ContChangesEntry*>(actor->baseProcess->GetWeaponInfo());
	auto weap = static_cast<TESObjectWEAP*>(weaponInfo->type);
	int maxRounds = weap->clipRounds.clipRounds;
	auto* ammoInfo = actor->baseProcess->GetAmmoInfo();
	auto* ammo = ammoInfo ? ammoInfo->ammo : nullptr;
	if (WeaponHasNthMod(weaponInfo, weap, TESObjectWEAP::kWeaponModEffect_IncreaseClipCapacity))
	{
		maxRounds += weap->GetModBonuses(TESObjectWEAP::kWeaponModEffect_IncreaseClipCapacity);
	}

	/*double itemCount = 0;
	if (ammo && CdeclCall<bool>(0x59D8E0, actor, ammo, 0, &itemCount))
	{
		return min(maxRounds, (int)itemCount);
	}*/
	
	return maxRounds;
}

std::map<ReloadKey, ReloadHandler> g_reloadMap;
std::shared_mutex g_reloadMapMutex;

std::optional<ReloadKey> GetReloadKey(Actor* actor)
{
	if (!actor || !actor->baseProcess)
		return std::nullopt;
	const auto* weapon = actor->GetWeaponForm();
	if (!weapon)
		return std::nullopt;
	const auto* ammo = actor->baseProcess->GetAmmoInfo();
	if (!ammo || !ammo->ammo)
		return std::nullopt;
	return ReloadKey{ .actorId = actor->refID };
}

ReloadType OnReloadHandler::GetLastReloadForActor(Actor* actor)
{
	const auto key = GetReloadKey(actor);
	if (!key)
		return ReloadType::NonPartial;
	std::shared_lock lock(g_reloadMapMutex);
	const auto iter = g_reloadMap.find(*key);
	if (iter == g_reloadMap.end())
		return ReloadType::NonPartial;
	return iter->second.reloadType;
}

void OnReloadHandler::SetDidReload(Actor* actor, ReloadType reloadType)
{
	const auto key = GetReloadKey(actor);
	if (!key)
		return;
	auto* weapon = actor->GetWeaponForm();
	if (!weapon)
		return;
	std::unique_lock lock(g_reloadMapMutex);
	g_reloadMap.insert_or_assign(*key, ReloadHandler {
		.isLoopingReload = weapon->HasLoopingReloadAnim(),
		.reloadType = reloadType
	});
}

void OnReloadHandler::Update()
{
	std::unique_lock lock(g_reloadMapMutex);
	for (auto iter = g_reloadMap.begin(); iter != g_reloadMap.end();)
	{
		auto* actor = static_cast<Actor*>(LookupFormByRefID(iter->first.actorId));
		if (!actor || !DYNAMIC_CAST(actor, TESForm, Actor) || !actor->baseProcess)
		{
			iter = g_reloadMap.erase(iter);
			continue;
		}
		const auto* animData = actor == g_thePlayer ? g_thePlayer->GetAnimData() : actor->baseProcess->GetAnimData();
		if (!animData)
		{
			iter = g_reloadMap.erase(iter);
			continue;
		}
		auto* weaponAnim = animData->animSequence[kSequence_Weapon];
		if (!weaponAnim || !weaponAnim->animGroup)
		{
			iter = g_reloadMap.erase(iter);
			continue;
		}
		const auto* ammoInfo = actor->baseProcess->GetAmmoInfo();
		if (!ammoInfo)
		{
			iter = g_reloadMap.erase(iter);
			continue;
		}
		if (weaponAnim->animGroup->IsReload() && !iter->second.isLoopingReload)
		{
			// if the weapon anim is reload, it has already played
			iter = g_reloadMap.erase(iter);
			continue;
		}
		if (iter->second.isLoopingReload)
		{
			if (ammoInfo->count != 0 && !weaponAnim->animGroup->IsReload()) // reload finished
			{
				iter = g_reloadMap.erase(iter);
				continue;
			}
		}
		++iter;
	}
}

float GetTimePassed(AnimData* animData, UInt8 animGroupID)
{
	const auto isMenuMode = CdeclCall<bool>(0x702360);
	if (isMenuMode)
		return 0.0;
	auto result = g_timeGlobal->secondsPassed * static_cast<float>(ThisStdCall<double>(0x9C8CC0, reinterpret_cast<void*>(0x11F2250)));
	if (animData)
		result *= GetAnimMult(animData, animGroupID);
	return result;
}

void LogScript(Script* scriptObj, TESForm* form, const std::string& funcName)
{
	LOG(FormatString("Script %s %X from mod %s has called %s", scriptObj->GetName(), scriptObj->refID, GetModName(scriptObj), funcName.c_str()));
	if (form)
		LOG(FormatString("\ton form %s %X", form->GetName(), form->refID));
	else
		LOG("\tGLOBALLY on any form");
}

ScopedList<char> GetDirectoryAnimPaths(std::string_view path)
{
	sv::stack_string<0x400> resultPath = path;
	if (resultPath.ends_with('\\'))
		resultPath.pop_back();
	
	const sv::stack_string<0x400> searchPath = { R"(data\meshes\%s\*.kf)", resultPath.c_str() };
	const sv::stack_string<0x400> renamePath = { R"(%s\*.kf)", resultPath.c_str() };
	return FileFinder::FindFiles(searchPath.c_str(), renamePath.c_str(), ARCHIVE_TYPE_MESHES);
}

template <typename F>
bool OverrideAnimsFromScript(char* path, const F& overrideAnim)
{
	const std::string_view pathStr(path);
	if (!sv::get_file_extension(pathStr).empty()) // single file
		return overrideAnim(pathStr.data());
	
	// directory
	const auto animPaths = GetDirectoryAnimPaths(pathStr.data());

	if (animPaths.Empty())
		return false;

	size_t numAnims = 0;
	for (const auto* animPath : animPaths)
	{
		numAnims += overrideAnim(animPath);
	}
	return numAnims != 0;
}

bool Cmd_SetWeaponAnimationPath_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* weaponForm = nullptr;
	auto firstPerson = 0;
	auto enable = 0;
	char path[0x400];
	if (!ExtractArgs(EXTRACT_ARGS, &weaponForm, &firstPerson, &enable, &path))
		return true;
	if (!weaponForm)
		return true;
	auto* weapon = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
	if (!weapon)
		return true;
	try
	{
		LogScript(scriptObj, weapon, "SetWeaponAnimationPath");
		const auto overrideAnim = [&](const char* animPath)
		{
			const auto pooledPath = AddStringToPool(ToLower(animPath));
			AnimOverrideData animOverrideData = {
				.path = pooledPath,
				.identifier = weapon->refID,
				.enable = static_cast<bool>(enable),
				.conditionScript = nullptr,
				.pollCondition = false,
				.matchBaseGroupId = false
			};
			return OverrideFormAnimation(animOverrideData, firstPerson);
		};
		*result = OverrideAnimsFromScript(path, overrideAnim);
	}
	catch (std::exception& e)
	{
		ShowRuntimeError(scriptObj, "SetWeaponAnimationPath: %s", e.what());
	}
	return true;
}

bool Cmd_SetActorAnimationPath_Execute(COMMAND_ARGS)
{
	*result = 0;
	int firstPerson = false;
	auto enable = 0;
	char path[0x1000];
	int pollCondition = 0;
	Script* conditionScript = nullptr;
	int matchBaseGroupId = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &firstPerson, &enable, &path, &pollCondition, &conditionScript, &matchBaseGroupId))
		return true;
	Actor* actor = nullptr;
	if (thisObj)
	{
		actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
		if (!actor)
		{
			ERROR_LOG("ERROR: SetActorAnimationPath received an invalid reference as actor");
			return true;
		}
	}
	else if (firstPerson) // first person means it can only be the player
		actor = g_thePlayer;

	LogScript(scriptObj, actor, "SetActorAnimationPath");
	
	if (conditionScript && !IS_ID(conditionScript, Script))
	{
		ERROR_LOG("ERROR: SetActorAnimationPath received an invalid script/lambda as parameter.");
		conditionScript = nullptr;
	}
	try
	{
		AnimOverrideData animOverrideData = {
			.identifier = actor ? actor->refID : 0xFF,
			.enable = static_cast<bool>(enable),
			.conditionScript = conditionScript,
			.pollCondition = static_cast<bool>(pollCondition),
			.matchBaseGroupId = static_cast<bool>(matchBaseGroupId)
		};
		*result = OverrideAnimsFromScript(path, [&](const char* animPath)
		{
			const auto pooledPath = AddStringToPool(ToLower(animPath));
			animOverrideData.path = pooledPath;
			return actor ? OverrideFormAnimation(animOverrideData, static_cast<bool>(firstPerson)) : OverrideModIndexAnimation(animOverrideData, static_cast<bool>(firstPerson));
		});
	}
	catch (std::exception& e)
	{
		ShowRuntimeError(scriptObj, "SetWeaponAnimationPath: %s", e.what());
	}
	return true;
}

template <typename Function>
static void QueueScriptAnimOperation(Actor* actor, double* result, Function&& callback)
{
	const auto currentThreadId = GetCurrentThreadId();

	const auto isPlayerOnMainThread = actor == g_thePlayer && currentThreadId == OSGlobals::GetSingleton()->mainThreadID;
	if (isPlayerOnMainThread || !AILinearTaskManager::ShouldQueue3DTask())
	{
		*result = callback();
		return;
	}
	*result = 1.0;
	// main thread queue runs before AILinearTaskManager threads are started and ended
	g_mainThreadQueue.Add(std::forward<Function>(callback));
}

AnimData* GetAnimData(Actor* actor, int firstPerson)
{
	if (!actor || !actor->baseProcess)
		return nullptr;
	switch (firstPerson)
	{
	case -1:
		if (actor != g_thePlayer)
			return actor->GetAnimation();
		return !g_thePlayer->IsThirdPerson() ? g_thePlayer->firstPersonAnimData : g_thePlayer->baseProcess->GetAnimData();
	case 0:
		return actor->baseProcess->GetAnimData();
	case 1:
	default:
		if (actor == g_thePlayer)
			return g_thePlayer->firstPersonAnimData;
		return actor->GetAnimation();
	}
}

void ApplyFootIK(Actor* actor)
{
	if (actor && actor->IsActor() && INISettings::UseRagdollAnimFootIK() &&
			actor->ragDollController && actor->ragDollController->bHasFootIK )
	{
		actor->ragDollController->ApplyBoneTransforms();
	}
}

bool Cmd_PlayAnimationPath_Execute(COMMAND_ARGS)
{
	*result = 0;
	sv::stack_string<0x400> path;
	auto bIsFirstPerson = -1;
	
	if (!ExtractArgs(EXTRACT_ARGS, &path.data_, &bIsFirstPerson))
		return true;
	path.to_lower();

	auto* pActor = DYNAMIC_CAST(thisObj, TESForm, Actor);
	if (!pActor)
		return true;
	
	const auto uiFormId = thisObj->refID;
	const auto sPath = std::string(path.str());
	
	QueueScriptAnimOperation(pActor, result, [uiFormId, sPath, bIsFirstPerson]
	{
		auto* actor = static_cast<Actor*>(LookupFormByID(uiFormId));
		if (!actor->IsActor())
			return 0.0;
		auto* animData = GetAnimData(actor, bIsFirstPerson);
		if (!animData)
			return 0.0;

		if (const auto anim = FindOrLoadAnim(animData, sPath.c_str()))
		{
			if (anim->m_eState != NiControllerSequence::INACTIVE)
				animData->controllerManager->DeactivateSequence(anim, 0.0f);
			const auto easeInTime = anim->GetEaseInTime();
			if (anim->Activate(0, true, anim->m_fSeqWeight, easeInTime, nullptr, false))
			{
				ApplyFootIK(actor);
				if (auto* animTime = HandleExtraOperations(animData, anim, true); animTime && anim->m_eCycleType != NiControllerSequence::LOOP)
				{
					animTime->trackEndTime = true;
					animTime->endIfSequenceTypeChanges = false;
				}
				auto* extraData = SequenceExtraDatas::GetOrCreate(anim);
				extraData->startInReloadTargets = true;
				return 1.0;
			}
		}
		return 0.0;
	});
	
	return true;
}

bool Cmd_kNVSEReset_Execute(COMMAND_ARGS)
{
	bool refresh = false;
	for (auto& [nameAndAnimData, context] : g_cachedAnimMap)
	{
		auto name = NiFixedString(nameAndAnimData.first);
		auto* animData = nameAndAnimData.second;
		BSAnimGroupSequence* anim = context.anim;

		auto* manager = animData->controllerManager;
		bool wasActive = false;
		if (anim->m_eState != NiControllerSequence::INACTIVE)
		{
			wasActive = true;
			if (anim->animGroup)
				animData->ClearGroup(anim->animGroup->GetSequenceType(), 0.0);
			manager->DeactivateSequence(anim, 0.0f);
			NiUpdateData updateData{};
			manager->Update(&updateData);
		}
		const auto seqCount = manager->sequences.EffectiveSize();
		manager->sequences.Remove(anim);
		const auto newSeqCount = manager->sequences.EffectiveSize();
		DebugAssert(newSeqCount == seqCount - 1);

		const auto seqMapCount = manager->m_kSequenceMap.m_uiCount;
		manager->m_kSequenceMap.RemoveAt(name);
		DebugAssert(manager->m_kSequenceMap.m_uiCount == seqMapCount - 1);
		
		auto* kfMap = ModelLoader::GetSingleton()->kfMap;
		const auto kfCount = kfMap->GetCount();
		kfMap->RemoveAt(name);
		const auto newKfCount = kfMap->GetCount();
		DebugAssert(newKfCount == kfCount - 1);

		if (wasActive && animData->actor == g_thePlayer)
			refresh = true;
	}
	
	g_animGroupFirstPersonMap.clear();
	g_animGroupThirdPersonMap.clear();
	g_animGroupModIdxFirstPersonMap.clear();
	g_animGroupModIdxThirdPersonMap.clear();
	g_scriptSoundExecutions.clear();
	g_scriptCallExecutions.clear();
	g_scriptLineExecutions.clear();
	g_cachedAnimMap.clear();
	g_timeTrackedAnims.clear();
	g_timeTrackedGroups.clear();
	// HandleGarbageCollection();
	LoadFileAnimPaths();

	if (refresh)
		g_thePlayer->RestartAnims();
	
	if (!IsConsoleMode())
		Console_Print("kNVSEReset called from a script! This is a DEBUG function that maybe leak memory");
	return true;
}

bool Cmd_PlayGroupAlt_Execute(COMMAND_ARGS)
{
	*result = 0;
	int groupIdMinor;
	int flags = 1;
	if (!ExtractArgs(EXTRACT_ARGS, &groupIdMinor, &flags))
		return true;
	if (!thisObj || !thisObj->IsTypeActor())
		return true;
	auto* actor = static_cast<Actor*>(thisObj);
	const auto groupId = GameFuncs::GetActorAnimGroupId(actor, groupIdMinor, nullptr, false, nullptr);
	if (groupId == 0xFF)
		return true;
	if (GameFuncs::PlayAnimGroup(actor->GetAnimation(), groupId, flags, -1, -1))
		*result = 1;
	return true;
}

#if _DEBUG

bool Cmd_kNVSETest_Execute(COMMAND_ARGS)
{
	auto* animData = g_thePlayer->baseProcess->GetAnimData();
	auto* anim = FindOrLoadAnim(animData, "characters\\_male\\sneak1hpattack8.kf");
	auto controlledBlocks = anim->GetControlledBlocks();
	auto idTagArray = anim->GetIDTags();
	return true;
}

#endif

bool Cmd_CreateIdleAnimForm_Execute(COMMAND_ARGS)
{
	*result = 0;
	char path[0x1000];
	eAnimSequence type = kSequence_SpecialIdle;
	if (!ExtractArgs(EXTRACT_ARGS, &path, &type))
	{
		return true;
	}
	auto* idleForm = New<TESIdleForm>(0x5FE040);
	idleForm->anim.SetModelPath(path);
	idleForm->data.groupFlags = 0x40 + type;
	*reinterpret_cast<UInt32*>(&result) = idleForm->refID;
	return true;
}

bool Cmd_EjectWeapon_Execute(COMMAND_ARGS)
{
	if (!thisObj || !DYNAMIC_CAST(thisObj, TESObjectREFR, Actor))
		return true;
	auto* actor = static_cast<Actor*>(thisObj);
	actor->EjectFromWeapon(actor->GetWeaponForm());
	return true;
}

NVSEArrayVarInterface::Array* CreateAnimationObjectArray(BSAnimGroupSequence* anim, Script* callingScript, const AnimData* animData)
{
	NVSEStringMapBuilder builder;

	builder.Add("sequenceName", anim->m_kName.CStr());
	builder.Add("seqWeight", anim->m_fSeqWeight);
	builder.Add("cycleType", anim->m_eCycleType);
	builder.Add("frequency", anim->m_fFrequency);
	builder.Add("beginKeyTime", anim->m_fBeginKeyTime);
	builder.Add("endKeyTime", anim->m_fEndKeyTime);
	if (animData)
	{
		builder.Add("lastTime", anim->m_fLastTime);
		builder.Add("weightedLastTime", anim->m_fWeightedLastTime);
		builder.Add("lastScaledTime", anim->m_fLastScaledTime);
		builder.Add("state", anim->m_eState);
		builder.Add("offset", anim->m_fOffset);
		builder.Add("startTime", anim->m_fStartTime);
		builder.Add("endTime", anim->m_fEndTime);
		builder.Add("calculatedTime", GetAnimTime(anim));
		if (anim->animGroup)
		{
			builder.Add("multiplier", GetAnimMult(animData, anim->animGroup->groupID));

		}
	}
	builder.Add("accumRootName", anim->m_kAccumRootName.CStr());
	if (anim->animGroup)
	{
		builder.Add("animGroupId", anim->animGroup->groupID);
		if (const auto* animGroupInfo = anim->animGroup->GetGroupInfo())
		{
			builder.Add("sequenceType", animGroupInfo->sequenceType);
			builder.Add("keyType", animGroupInfo->keyType);
			builder.Add("animGroupName", animGroupInfo->name);
		}
	}

#if _DEBUG
	const auto address = FormatString("0x%X", reinterpret_cast<UInt32>(anim));
	builder.Add("address", address.c_str());
#endif

	const NiTextKeyExtraData* textKeyData = anim->m_spTextKeys;
	if (textKeyData)
	{
		NVSEArrayBuilder textKeyTimeBuilder;
		NVSEArrayBuilder textKeyValueBuilder;

		for (const auto& key : textKeyData->GetKeys())
		{
			textKeyTimeBuilder.Add(key.m_fTime);
			textKeyValueBuilder.Add(key.m_kText.CStr());
		}

		auto* textKeyTimes = textKeyTimeBuilder.Build(g_arrayVarInterface, callingScript);
		auto* textKeyValues = textKeyValueBuilder.Build(g_arrayVarInterface, callingScript);

		builder.Add("textKeyTimes", textKeyTimes);
		builder.Add("textKeyValues", textKeyValues);
	}
	

	return builder.Build(g_arrayVarInterface, callingScript);
}

std::vector<std::string_view> FindCustomAnimations(AnimData* animData, const UInt16 nearestGroupId)
{
	std::vector<std::string_view> result;
	const auto animationResult = GetActorAnimation(nearestGroupId, animData);
	if (animationResult && animationResult->animBundle && !animationResult->animBundle->anims.empty())
	{
		const auto& animPaths = animationResult->animBundle->anims;
		for (const auto& animPath : animPaths)
		{
			result.push_back(animPath->path);
		}

	}
	return result;
}

std::unordered_set<BaseProcess*> g_allowedNextAnims;

BSAnimGroupSequence* FindActiveAnimationByPath(AnimData* animData, const char* path)
{
	if (!animData->controllerManager)
		return nullptr;
	if (auto* anim = animData->controllerManager->m_kSequenceMap.Lookup(path))
		return static_cast<BSAnimGroupSequence*>(anim);
	return nullptr;
}

BSAnimGroupSequence* FindActiveAnimationForActor(Actor* actor, const char* path, POVSwitchState pov = POVSwitchState::NotSet)
{
	AnimData* animData = nullptr;
	if ((actor == g_thePlayer && g_thePlayer->IsFirstPerson() && pov == POVSwitchState::NotSet) || (pov == POVSwitchState::POV1st && actor == g_thePlayer))
		animData = g_thePlayer->firstPersonAnimData;
	else if (actor->baseProcess)
		animData = actor->baseProcess->GetAnimData();
	if (!animData)
		return nullptr;
	auto* anim = FindActiveAnimationByPath(animData, path);
	if (!anim && actor == g_thePlayer)
	{
		auto* otherAnimData = g_thePlayer->IsFirstPerson() ? g_thePlayer->baseProcess->GetAnimData() : g_thePlayer->firstPersonAnimData;
		anim = FindActiveAnimationByPath(otherAnimData, path);
	}
	return anim;
}

BSAnimGroupSequence* FindActiveAnimationForRef(TESObjectREFR* thisObj, const char* path, POVSwitchState povState = POVSwitchState::NotSet)
{
	if (!thisObj)
		return GetAnimationByPath(path);
	if (thisObj->IsActor())
		return FindActiveAnimationForActor(static_cast<Actor*>(thisObj), path, povState);
	auto* ninode = thisObj->Get3D();
	if (!ninode)
		return nullptr;
	auto* controller = ninode->m_controller;
	if (!controller)
		return nullptr;
	if (IS_TYPE(controller, NiControllerManager))
	{
		auto* controllerManager = static_cast<NiControllerManager*>(controller);
		if (auto* sequence = controllerManager->m_kSequenceMap.Lookup(path))
		{
			return static_cast<BSAnimGroupSequence*>(sequence);
		}
	}
	return nullptr;
}

BSAnimGroupSequence* FindOrLoadAnim(AnimData* animData, const char* path)
{
	if (!animData || !animData->controllerManager)
		return nullptr;
	if (auto* anim = animData->controllerManager->m_kSequenceMap.Lookup(path))
		return static_cast<BSAnimGroupSequence*>(anim);
	const std::string_view pooledPath = AddStringToPool(ToLower(path));
	const auto& ctx = LoadCustomAnimation(pooledPath, animData);
	if (ctx)
		return ctx->anim;
	return nullptr;
}

BSAnimGroupSequence* FindOrLoadAnim(TESObjectREFR* thisObj, const char* path)
{
	if (!thisObj)
		return GetAnimationByPath(path);
	if (thisObj->IsActor())
	{
		auto* animData = GetAnimData(static_cast<Actor*>(thisObj), -1);
		if (!animData)
			return nullptr;
		return FindOrLoadAnim(animData, path);
	}
	return FindActiveAnimationForRef(thisObj, path);
}

BSAnimGroupSequence* FindOrLoadAnim(Actor* actor, const char* path, bool firstPerson)
{
	if (!actor->baseProcess)
		return nullptr;
	auto* animData = firstPerson && actor == g_thePlayer ? g_thePlayer->firstPersonAnimData : actor->baseProcess->GetAnimData();
	if (!animData)
		return nullptr;
	return FindOrLoadAnim(animData, path);
}

AnimData* GetAnimDataForAnim(TESObjectREFR* thisObj, BSAnimGroupSequence* anim)
{
	if (!thisObj)
		return nullptr;
	if (thisObj == g_thePlayer && anim->m_pkOwner == g_thePlayer->firstPersonAnimData->controllerManager)
		return g_thePlayer->firstPersonAnimData;
	if (const auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor); actor && actor->baseProcess)
		return actor->baseProcess->GetAnimData();
	return nullptr;
}

float GetIniFloat(UInt32 addr)
{
	return ThisStdCall<SettingT::Info*>(0x403E20, reinterpret_cast<void*>(addr))->f;
}

float AnimData::GetBlendTime() const
{
	if (noBlend120)
		return 0.0f;
	return GetDefaultBlendTime() / 30.0f / GetDefaultAnimMult();
}

float GetDefaultBlendOutTime(const BSAnimGroupSequence* destSequence)
{
	const static auto sNoBlend = NiFixedString("noBlend");
	if (destSequence->m_spTextKeys->FindFirstByName(sNoBlend))
		return 0.0f;
	const auto defaultBlend = GetDefaultBlendTime();
	const auto blendMult = GetDefaultAnimMult();
	if (!destSequence->animGroup)
		return defaultBlend;
	const auto blendOut = max(destSequence->animGroup->blendOut, destSequence->animGroup->blend);
	if (blendOut == 0)
		return defaultBlend;
	return static_cast<float>(blendOut) / 30.0f / blendMult;
}

float GetDefaultBlendTime(const BSAnimGroupSequence* destSequence, const BSAnimGroupSequence* sourceSequence)
{
	const static auto sNoBlend = NiFixedString("noBlend");
	if (destSequence->m_spTextKeys->FindFirstByName(sNoBlend))
		return 0.0f;
	const auto defaultBlend = GetDefaultBlendTime();
	const auto blendMult = GetDefaultAnimMult();

	if (!destSequence->animGroup)
		return defaultBlend;

	const float sourceBlendOut = sourceSequence ? max(destSequence->animGroup->blend, destSequence->animGroup->blendOut) : 0;
	const float destBlendIn = max(destSequence->animGroup->blend, destSequence->animGroup->blendIn);

	float blendValue;

	if (destBlendIn > sourceBlendOut)
		blendValue = destBlendIn;
	else if (sourceBlendOut > 0)
		blendValue = sourceBlendOut;
	else
		return defaultBlend;

	if (blendValue == 0)
		return defaultBlend;
	return blendValue / 30.0f / blendMult;
}

bool IsPlayerInFirstPerson(Actor* actor)
{
	if (actor != g_thePlayer)
		return false;
	return !g_thePlayer->IsThirdPerson();
}

UInt16 GetNearestGroupID(AnimData* animData, AnimGroupID animGroupId)
{
	const auto fullGroupId = GetActorRealAnimGroup(animData->actor, animGroupId);
	const auto nearestGroupId = GameFuncs::GetNearestGroupID(animData, fullGroupId, false);
	if ((animGroupId & 0xFF) != 0 && nearestGroupId == 0)
		return 0xFFFF; // bethesda returns 0 in Animation::PickBestAnimation when it can't find one when it's actually the group id for idle...
	return nearestGroupId;
}

NiControllerSequence::InterpArrayItem* FindAnimInterp(TESObjectREFR* thisObj, const char* animPath, const char* interpName)
{
	auto* anim = FindActiveAnimationForRef(thisObj, animPath);
	if (!anim)
		return nullptr;
	return anim->GetControlledBlock(interpName);
}

bool CopyAnimationsToForm(TESForm* fromForm, TESForm* toForm)
{
	bool applied = false;
	for (auto* map : {&g_animGroupFirstPersonMap, &g_animGroupThirdPersonMap})
	{
		if (const auto iter = map->find(fromForm->refID); iter != map->end())
		{
			AnimOverrideData animOverrideData = {
				.identifier = toForm->refID,
				.enable = true,
			};
			auto& entry = iter->second;
			for (auto& stacks : entry.stacks | std::views::values)
			{
				auto& stack = stacks.anims;
				for (auto& anims : stack)
				{
					for (const auto& anim : anims->anims)
					{
						animOverrideData.path = anim->path;
						animOverrideData.conditionScript = *anims->conditionScript;
						animOverrideData.conditionScriptText = anims->conditionScriptText;
						animOverrideData.pollCondition = anims->pollCondition;
						animOverrideData.matchBaseGroupId = anims->matchBaseGroupId;
						SetOverrideAnimation(animOverrideData, *map);
					}
				}
			}
		}
		
	}
	
	return applied;
}

void CreateCommands(NVSECommandBuilder& builder)
{
	const auto getAnimBySequenceTypeParams = {
		ParamInfo{"sequence id", kParamType_Integer, false},
		ParamInfo{"bIsFirstPerson", kParamType_Integer, true}
	};
	builder.Create("GetPlayingAnimBySequenceType", kRetnType_Array, getAnimBySequenceTypeParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		UInt32 sequenceId = -1;
		int firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &sequenceId, &firstPerson))
			return true;
		if (sequenceId < 0 || sequenceId > kSequence_SpecialIdle)
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* animData = GetAnimData(actor, firstPerson);
		if (!animData)
			return true;
		auto* anim = animData->animSequence[sequenceId];
		if (!anim)
			return true;
		*result = reinterpret_cast<UInt32>(CreateAnimationObjectArray(anim, scriptObj, animData));
		return true;
	}, nullptr, "GetAnimByType");

	builder.Create("GetPlayingAnimSequencePaths", kRetnType_Array, { ParamInfo{"bIsFirstPerson", kParamType_Integer, true} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		int firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &firstPerson))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (const auto* animData = GetAnimData(actor, firstPerson))
		{
			NVSEArrayBuilder arr;
			for (const auto* anim : animData->animSequence)
			{
				if (anim)
					arr.Add(anim->m_kName.CStr());
				else
					arr.Add("");
			}
			*result = reinterpret_cast<UInt32>(arr.Build(g_arrayVarInterface, scriptObj));
		}
		return true;
	}, nullptr, "GetAnimsByType");

	builder.Create("GetAnimationAttributes", kRetnType_Array, { ParamInfo{"path", kParamType_String, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char path[0x400];
		path[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &path))
			return true;
		SetLowercase(path);
		BSAnimGroupSequence* anim = FindActiveAnimationForRef(thisObj, path);

		if (!anim)
			anim = GetAnimationByPath(path);
		if (!anim)
			return true;
		const auto* animData = GetAnimDataForAnim(thisObj, anim);
		
		*result = reinterpret_cast<UInt32>(CreateAnimationObjectArray(anim, scriptObj, animData));
		return true;
	}, nullptr, "GetAnimAttrs");

	const auto getAnimsByAnimGroupParams = {
		ParamInfo{"anim group id", kParamType_AnimationGroup, false},
		ParamInfo{"bIsFirstPerson", kParamType_Integer, true}
	};
	builder.Create("GetAnimationsByAnimGroup", kRetnType_Array, getAnimsByAnimGroupParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		UInt32 animGroupId = -1;
		int firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &animGroupId, &firstPerson) || !thisObj)
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* animData = GetAnimData(actor, firstPerson);
		if (!animData)
			return true;
		const auto nearestGroupId = GetNearestGroupID(animData, static_cast<AnimGroupID>(animGroupId));
		if (nearestGroupId == 0xFFFF)
			return true;
		const auto customAnims = FindCustomAnimations(animData, nearestGroupId);
		if (!customAnims.empty())
		{
			NVSEArrayBuilder builder;
			for (auto& customAnim : customAnims)
			{
				builder.Add(customAnim.data());
			}
			*result = reinterpret_cast<UInt32>(builder.Build(g_arrayVarInterface, scriptObj));
			return true;
		}
		if (auto* base = animData->mapAnimSequenceBase->Lookup(nearestGroupId))
		{
			NVSEArrayBuilder builder;
			if (base->IsSingle())
			{
				const auto* anim = base->GetSequenceByIndex(0);
				builder.Add(anim->m_kName.CStr());
				*result = reinterpret_cast<UInt32>(builder.Build(g_arrayVarInterface, scriptObj));
				return true;
			}
			const auto* multi = static_cast<AnimSequenceMultiple*>(base);
			multi->anims->ForEach([&](BSAnimGroupSequence* sequence)
			{
				builder.Add(sequence->m_kName.CStr());
			});
			*result = reinterpret_cast<UInt32>(builder.Build(g_arrayVarInterface, scriptObj));
			return true;
		}

		return true;
	}, nullptr, "GetAnimsByGroup");

	builder.Create("ForceFireWeapon", kRetnType_Default, {}, true, [](COMMAND_ARGS)
	{
		*result = 0;
		if (auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor))
		{
			actor->FireWeapon();
			*result = 1;
		}
		return true;
	});

	builder.Create("SetAnimTextKeys", kRetnType_Default, { 
		ParamInfo{"anim path", kNVSEParamType_String, false}, 
		ParamInfo{"text key times", kNVSEParamType_Array, false}, 
		ParamInfo{"text key values", kNVSEParamType_Array, false},
		ParamInfo{"point of view", kNVSEParamType_Number, true}
	}, false, [](COMMAND_ARGS)
	{
		*result = 0;
		PluginExpressionEvaluator eval(PASS_COMMAND_ARGS);
		if (!eval.ExtractArgs() || eval.NumArgs() < 3)
			return true;
		auto* path = eval.GetNthArg(0)->GetString();
		auto* textKeyTimesArray = eval.GetNthArg(1)->GetArrayVar();
		auto* textKeyValuesArray = eval.GetNthArg(2)->GetArrayVar();
		if (!path || !textKeyTimesArray || !textKeyValuesArray)
			return true;
		POVSwitchState povState = POVSwitchState::NotSet;
		if (auto* firstPersonArg = eval.GetNthArg(3))
			povState = static_cast<POVSwitchState>(firstPersonArg->GetInt());
		const auto lowerPath = ToLower(path);
		auto* anim = FindActiveAnimationForRef(thisObj, lowerPath.c_str(), povState);
				
		if (!anim)
		{
			ERROR_LOG("SetAnimTextKeys: animation not found");
			return true;
		}
		if (anim->m_eState != NiControllerSequence::INACTIVE)
		{
			ERROR_LOG("SetAnimTextKeys: you can not call this function while the animation is playing.");
			return true;
		}

		const auto textKeyTimesVector= NVSEArrayToVector<double>(g_arrayVarInterface, textKeyTimesArray);
		const auto textKeyValuesVector = NVSEArrayToVector<BSString>(g_arrayVarInterface, textKeyValuesArray);

		if (textKeyTimesVector.size() != textKeyValuesVector.size())
		{
			ERROR_LOG("SetAnimTextKeys: text key times and values arrays must be the same size");
			return true;
		}

		std::vector<NiTextKey> textKeyVector;
		for (auto i = 0; i < textKeyTimesVector.size(); ++i)
		{
			const auto key = textKeyTimesVector.at(i);
			const auto& value = textKeyValuesVector.at(i);
			textKeyVector.emplace_back(key, value.CStr());
		}

		NiFixedArray<NiTextKey> newKeyArray(textKeyVector);

		if (!anim->animGroup) 
		{
			anim->m_spTextKeys->m_kKeyArray = std::move(newKeyArray);
			*result = 1;
			return true;
		}
		
		NiPointer oldAnimGroup = anim->animGroup;
		const auto* animGroupInfo = oldAnimGroup->GetGroupInfo();

		// let the game parse the text keys
		auto oldKeyArray = std::move(anim->m_spTextKeys->m_kKeyArray);
		anim->m_spTextKeys->m_kKeyArray = std::move(newKeyArray);
		anim->animGroup = nullptr;
		const NiFixedString oldSequenceName = anim->m_kName;
		anim->m_kName = animGroupInfo->name; // yes, the game initially has the sequence name set to the anim group name
		NiPointer animGroup = TESAnimGroup::Init(anim, path);
		anim->m_kName = oldSequenceName;

		if (!animGroup)
		{
			anim->m_spTextKeys->m_kKeyArray = std::move(oldKeyArray);
			anim->animGroup = oldAnimGroup;
			
			ERROR_LOG("SetAnimTextKeys: Game failed to parse text key data, see falloutnv_error.log");
			*result = 0;
			return true;
		}
		anim->animGroup = animGroup;
		*result = 1;
		return true;
	}, Cmd_Expression_Plugin_Parse);

	builder.Create("EraseCurrentAnimAction", kRetnType_Default, {}, true, [](COMMAND_ARGS)
	{
		*result = 0;
		const auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor || !actor->baseProcess)
			return true;
		actor->baseProcess->currentAction = Decoding::kAnimAction_None;
		actor->baseProcess->currentSequence = nullptr;
		*result = 1;
		return true;
	});

	builder.Create("SetAnimCurrentTime", kRetnType_Default, { ParamInfo{"path", kParamType_String, false}, ParamInfo{"time", kParamType_Float, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char path[0x400];
		path[0] = 0;
		float time;
		if (!ExtractArgs(EXTRACT_ARGS, &path, &time))
			return true;
		SetLowercase(path);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor || !actor->baseProcess)
			return true;
		auto* anim = FindActiveAnimationForActor(actor, path);
		if (!anim)
			return true;
		AnimData* animData;
		if (actor == g_thePlayer && anim->m_pkOwner == g_thePlayer->firstPersonAnimData->controllerManager)
			animData = g_thePlayer->firstPersonAnimData;
		else
			animData = actor->baseProcess->GetAnimData();

		anim->m_fOffset = time - animData->timePassed;
		*result = 1;
		return true;
	});

	builder.Create("CopyAnimationsToForm", kRetnType_Default, { ParamInfo{"from form", kParamType_AnyForm, false}, ParamInfo{"to form", kParamType_AnyForm, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		TESForm* fromForm = nullptr;
		TESForm* toForm = nullptr;
		if (!ExtractArgs(EXTRACT_ARGS, &fromForm, &toForm))
			return true;
		*result = CopyAnimationsToForm(fromForm, toForm);
		return true;
	});

	builder.Create("IsAnimSequencePlaying", kRetnType_Default, { ParamInfo{"path", kParamType_String, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char path[0x400];
		path[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &path))
			return true;
		SetLowercase(path);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		const auto* anim = FindActiveAnimationForActor(actor, path);
		if (!anim)
			return true;
		if (anim->m_eState != kAnimState_Inactive && anim->m_eState != kAnimState_EaseOut) 
			*result = 1;
		return true;
	});

	const auto activateAnimParams = {
		ParamInfo{"anim sequence path", kParamType_String, false},
		ParamInfo{"bIsFirstPerson", kParamType_Integer, true},
		ParamInfo{"iPriority", kParamType_Integer, true},
		ParamInfo{"bStartOver", kParamType_Integer, true},
		ParamInfo{"fWeight", kParamType_Float, true},
		ParamInfo{"fEaseInTime", kParamType_Float, true},
		ParamInfo{"time sync sequence path", kParamType_String, true}
	};
	builder.Create("ActivateAnim", kRetnType_Default, activateAnimParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char sequencePath[0x400];
		sequencePath[0] = 0;
		int firstPerson = -1;
		int priority = 0;
		int startOver = 1;
		float weight = INVALID_TIME;
		float easeInTime = INVALID_TIME;
		char timeSyncSequence[0x400];
		timeSyncSequence[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &firstPerson, &priority, &startOver, &weight, &easeInTime, &timeSyncSequence))
			return true;
		SetLowercase(sequencePath);
		SetLowercase(timeSyncSequence);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);

		const auto actorId = actor->refID;
		const auto sSequencePath = std::string(sequencePath);
		const auto sTimeSyncSequence = std::string(timeSyncSequence);

		QueueScriptAnimOperation(actor, result, [=]
		{
			auto* actor = static_cast<Actor*>(LookupFormByID(actorId));
			if (!actor || !actor->IsActor())
				return false;
			auto* anim = FindOrLoadAnim(actor, sSequencePath.c_str(), firstPerson);
			if (!anim)
				return false;
			auto actualWeight = weight;
			auto actualEaseInTime = easeInTime;
			if (actualWeight == INVALID_TIME)
				actualWeight = anim->m_fSeqWeight;
			if (actualEaseInTime == INVALID_TIME)
			{
				if (anim->animGroup && (anim->animGroup->blendIn || anim->animGroup->blend))
					actualEaseInTime = anim->GetEaseInTime();
				else
					actualEaseInTime = 0.0f;
			}
			BSAnimGroupSequence* timeSyncSeq = nullptr;
			if (!sTimeSyncSequence.empty())
			{
				timeSyncSeq = FindOrLoadAnim(actor, sTimeSyncSequence.c_str(), firstPerson);
				if (!timeSyncSeq)
					return false;
			}
			auto* animData = GetAnimData(actor, firstPerson);

			auto* manager = anim->m_pkOwner;
			if (anim->m_eState != NiControllerSequence::INACTIVE)
				manager->DeactivateSequence(anim, 0.0);

			const auto activateResult = manager->ActivateSequence(anim, priority, startOver, actualWeight, actualEaseInTime, timeSyncSeq);
			if (activateResult)
				if (auto* animTime = HandleExtraOperations(animData, anim, true))
					animTime->endIfSequenceTypeChanges = false;
			return activateResult;
		});
		return true;
	});

	/*
	 NiControllerManager::DeactivateSequence(
        NiControllerManager *unused,
        NiControllerSequence *pkSequence,
        float fEaseOutTime)
	 */
	const auto deactivateAnimParams = {
			ParamInfo{"anim sequence path", kParamType_String, false},
			ParamInfo{"fEaseOutTime", kParamType_Float, true}
	};
	builder.Create("DeactivateAnim", kRetnType_Default, deactivateAnimParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char sequencePath[0x400];
		sequencePath[0] = 0;
		float easeOutTime = INVALID_TIME;
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &easeOutTime))
			return true;
		SetLowercase(sequencePath);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;

		const auto actorId = actor->refID;
		const auto sSequencePath = std::string(sequencePath);

		QueueScriptAnimOperation(actor, result, [actorId, easeOutTime, sSequencePath]
		{
			auto* actor = static_cast<Actor*>(LookupFormByID(actorId));
			if (!actor || !actor->IsActor())
				return false;
			auto* anim = FindActiveAnimationForActor(actor, sSequencePath.c_str());
			if (!anim)
				return false;
			auto actualEaseOutTime = easeOutTime;
			if (actualEaseOutTime == INVALID_TIME)
			{
				if (anim->animGroup && (anim->animGroup->blendOut || anim->animGroup->blend))
					actualEaseOutTime = anim->GetEaseOutTime();
				else
					actualEaseOutTime = 0.0f;
			}
			if (AdditiveManager::IsAdditiveSequence(anim))
				AdditiveManager::RemoveAdditiveTransformsFlags(anim);
			auto* manager = anim->m_pkOwner;
			return manager->DeactivateSequence(anim, actualEaseOutTime);
		});
		return true;
	});

	const auto crossFadeParams = {
		ParamInfo{"source sequence path", kParamType_String, false},
		ParamInfo{"dest sequence path", kParamType_String, false},
		ParamInfo{"bFirstPerson", kParamType_Integer, true},
		ParamInfo{"fEaseInTime", kParamType_Float, true},
		ParamInfo{"iPriority", kParamType_Integer, true},
		ParamInfo{"bStartOver", kParamType_Integer, true},
		ParamInfo{"fWeight", kParamType_Float, true},
		ParamInfo{"time sync sequence path", kParamType_String, true}
	};

	builder.Create("CrossFadeAnims", kRetnType_Default, crossFadeParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		int firstPerson = -1;
		char sourceSequence[0x400];
		char destSequence[0x400];
		char timeSyncSequence[0x400];
		sourceSequence[0] = 0;
		destSequence[0] = 0;
		timeSyncSequence[0] = 0;
		float easeInTime = FLT_MIN;
		int priority = 0;
		float weight = FLT_MIN;
		int startOver = 1;
		if (!ExtractArgs(EXTRACT_ARGS, &sourceSequence, &destSequence, &firstPerson, &easeInTime, &priority, &startOver, &weight, &timeSyncSequence))
			return true;
		SetLowercase(sourceSequence);
		SetLowercase(destSequence);
		SetLowercase(timeSyncSequence);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);

		const auto actorId = actor->refID;
		const auto sSourceSequence = std::string(sourceSequence);
		const auto sDestSequence = std::string(destSequence);
		const auto sTimeSyncSequence = std::string(timeSyncSequence);

		QueueScriptAnimOperation(actor, result, [=]
		{
			auto* actor = static_cast<Actor*>(LookupFormByID(actorId));
			if (!actor || !actor->IsActor())
				return false;
			auto* sourceAnim = FindOrLoadAnim(actor, sSourceSequence.c_str(), firstPerson);
			auto* destAnim = FindOrLoadAnim(actor, sDestSequence.c_str(), firstPerson);
			BSAnimGroupSequence* timeSyncAnim = nullptr;

			if (!sTimeSyncSequence.empty())
			{
				timeSyncAnim = FindOrLoadAnim(actor, sTimeSyncSequence.c_str(), firstPerson);
				if (!timeSyncAnim)
					return false;
			}

			if (!sourceAnim || !destAnim)
				return false;

			auto actualWeight = weight;
			auto actualEaseInTime = easeInTime;

			if (actualWeight == FLT_MIN)
				actualWeight = destAnim->m_fSeqWeight;

			if (actualEaseInTime == FLT_MIN)
				actualEaseInTime = GetDefaultBlendTime(destAnim, sourceAnim);

			auto* manager = sourceAnim->m_pkOwner;

			if (destAnim->m_eState != kAnimState_Inactive)
				GameFuncs::DeactivateSequence(destAnim->m_pkOwner, destAnim, 0.0f);

			return manager->CrossFade(
				sourceAnim,
				destAnim,
				actualEaseInTime,
				priority,
				startOver,
				actualWeight,
				timeSyncAnim
			);
		});
		return true;
	});

	const auto blendToAnimSequenceParams = {
		ParamInfo{"sequence path", kParamType_String, false},
		ParamInfo{"bFirstPerson", kParamType_Integer, true},
		ParamInfo{"fDuration", kParamType_Float, true},
		ParamInfo{"fDestFrame", kParamType_Float, true},
		ParamInfo{"iPriority", kParamType_Integer, true},
		ParamInfo{"time sync sequence path", kParamType_String, true}
	};

	builder.Create("BlendToAnimFromPose", kRetnType_Default, blendToAnimSequenceParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		int firstPerson = -1;
		char sequencePath[0x400];
		char timeSyncSequence[0x400];
		sequencePath[0] = 0;
		timeSyncSequence[0] = 0;
		float destFrame = 0.0f;
		float duration = FLT_MIN;
		int priority = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &firstPerson, &duration, &destFrame, &priority, &timeSyncSequence))
			return true;
		SetLowercase(sequencePath);
		SetLowercase(timeSyncSequence);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);

		const auto actorId = actor->refID;
		const auto sSequencePath = std::string(sequencePath);
		const auto sTimeSyncSequence = std::string(timeSyncSequence);

		QueueScriptAnimOperation(actor, result, [actorId, firstPerson, duration, destFrame, priority, sSequencePath, sTimeSyncSequence]
		{
			auto* actor = static_cast<Actor*>(LookupFormByID(actorId));
			if (!actor || !actor->IsActor())
				return false;
			auto* anim = FindOrLoadAnim(actor, sSequencePath.c_str(), firstPerson);
			if (!anim)
				return false;
			BSAnimGroupSequence* timeSyncAnim = nullptr;
			if (!sTimeSyncSequence.empty())
			{
				timeSyncAnim = FindOrLoadAnim(actor, sTimeSyncSequence.c_str(), firstPerson);
				if (!timeSyncAnim)
					return false;
			}
			auto actualDuration = duration;
			if (actualDuration == FLT_MIN)
				actualDuration = GetDefaultBlendTime(anim, nullptr);
			auto* manager = anim->m_pkOwner;
			if (anim->m_eState != kAnimState_Inactive)
				GameFuncs::DeactivateSequence(manager, anim, 0.0f);
			auto* animData = GetAnimData(actor, firstPerson);
			HandleExtraOperations(animData, anim);
			return GameFuncs::BlendFromPose(
				manager,
				anim,
				destFrame,
				actualDuration,
				priority,
				timeSyncAnim
			);
		});
		return true;
	});

	const auto blendFromAnimSequenceParams = {
		ParamInfo{"source sequence path", kParamType_String, false},
		ParamInfo{"dest sequence path", kParamType_String, false},
		ParamInfo{"bFirstPerson", kParamType_Integer, true},
		ParamInfo{"fDuration", kParamType_Float, true},
		ParamInfo{"fDestFrame", kParamType_Float, true},
		ParamInfo{"fSourceWeight", kParamType_Float, true},
		ParamInfo{"fDestWeight", kParamType_Float, true},
		ParamInfo{"iPriority", kParamType_Integer, true},
		ParamInfo{"time sync sequence path", kParamType_String, true}
	};

	builder.Create("BlendToAnim", kRetnType_Default, blendFromAnimSequenceParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char sourceSequence[0x400];
		char destSequence[0x400];
		char timeSyncSequence[0x400];
		sourceSequence[0] = 0;
		destSequence[0] = 0;
		timeSyncSequence[0] = 0;
		int firstPerson = -1;
		float duration = FLT_MIN;
		float destFrame = 0.0f;
		float sourceWeight = FLT_MIN;
		float destWeight = FLT_MIN;
		int priority = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &sourceSequence, &destSequence, &firstPerson, &duration, &destFrame, &sourceWeight,
		                 &destWeight, &priority, &timeSyncSequence))
			return true;
		SetLowercase(sourceSequence);
		SetLowercase(destSequence);
		SetLowercase(timeSyncSequence);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);
		auto* sourceAnim = FindOrLoadAnim(actor, sourceSequence, firstPerson);
		auto* destAnim = FindOrLoadAnim(actor, destSequence, firstPerson);
		if (!sourceAnim || !destAnim)
			return true;
		BSAnimGroupSequence* timeSyncAnim = nullptr;
		if (strlen(timeSyncSequence))
		{
			timeSyncAnim = FindOrLoadAnim(actor, timeSyncSequence, firstPerson);
			if (!timeSyncAnim)
				return true;
		}
		if (duration == FLT_MIN)
			duration = GetDefaultBlendTime(destAnim, sourceAnim);
		if (sourceWeight == FLT_MIN)
			sourceWeight = sourceAnim->m_fSeqWeight;
		if (destWeight == FLT_MIN)
			destWeight = destAnim->m_fSeqWeight;
		if (destAnim->m_eState != kAnimState_Inactive)
			GameFuncs::DeactivateSequence(destAnim->m_pkOwner, destAnim, 0.0f);
		auto* animData = GetAnimData(actor, firstPerson);
		HandleExtraOperations(animData, destAnim);
		*result = GameFuncs::StartBlend(sourceAnim, destAnim, duration, destFrame, priority, sourceWeight, destWeight, timeSyncAnim);
		return true;
	});

	builder.Create("SetAnimWeight", kRetnType_Default, { ParamInfo{"anim sequence path", kParamType_String, false }, ParamInfo{"weight", kParamType_Float, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char sequencePath[0x400];
		sequencePath[0] = 0;
		float weight = 0.0f;
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &weight) || !thisObj)
			return true;
		SetLowercase(sequencePath);
		BSAnimGroupSequence* anim;
		if (thisObj)
		{
			auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
			if (!actor)
				return true;
			anim = FindActiveAnimationForActor(actor, sequencePath);
		}
		else
			anim = GetAnimationByPath(sequencePath);
		if (!anim)
			return true;
		anim->m_fSeqWeight = weight;
		*result = 1;
		return true;
	});

	builder.Create("SetGlobalAnimTime", kRetnType_Default, { ParamInfo{"timePassed", kParamType_Float, false}, ParamInfo{"bFirstPerson", kParamType_Integer, true} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		float timePassed;
		int firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &timePassed, &firstPerson))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);
		auto* animData = GetAnimData(actor, firstPerson);
		if (!animData)
			return true;
		animData->timePassed = timePassed;
		*result = 1;
		return true;
	});

	builder.Create("GetGlobalAnimTime", kRetnType_Default, { ParamInfo{"bFirstPerson", kParamType_Integer, true} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		int firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &firstPerson))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);
		auto* animData = GetAnimData(actor, firstPerson);
		if (!animData)
			return true;
		*result = animData->timePassed;
		return true;
	});

	builder.Create("GetAnimCurrentTime", kRetnType_Default, { ParamInfo{"path", kParamType_String, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char path[0x400];
		path[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &path))
			return true;
		SetLowercase(path);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindActiveAnimationForActor(actor, path);
		if (!anim)
			return true;
		*result = anim->m_fLastScaledTime;
		return true;
	});

	builder.Create("RegisterCustomAnimGroup", kRetnType_Default, { 
		ParamInfo{"name", kParamType_String, false},
		ParamInfo{"script", kParamType_AnyForm, false},
		ParamInfo{"clean up script", kParamType_AnyForm, true},
	}, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char name[0x400];
		name[0] = 0;
		Script* script;
		Script* cleanupScript = nullptr;
		if (!ExtractArgs(EXTRACT_ARGS, &name, &script, &cleanupScript) || NOT_TYPE(script, Script) || (cleanupScript && NOT_TYPE(cleanupScript, Script)))
			return true;
		auto& scripts = g_customAnimGroups[ToLower(name)];
		if (ra::find(scripts, CustomAnimGroupScript{script, cleanupScript}) == scripts.end())
		{
			scripts.emplace_back(script, cleanupScript);
			*result = 1;
		}
		return true;
	});

	builder.Create("QueueNextAnim", kRetnType_Default, { ParamInfo { "sAnimPath", kParamType_String, false }, ParamInfo { "bFirstPerson", kParamType_Integer, true } }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char animPath[0x400];
		animPath[0] = 0;
		int firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &firstPerson))
			return true;
		SetLowercase(animPath);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);
		auto* animData = GetAnimData(actor, firstPerson);
		if (!animData)
			return true;
		auto* anim = FindOrLoadAnim(animData, animPath);
		if (!anim || !anim->animGroup)
			return true;
		auto& queue = g_queuedReplaceAnims[std::make_pair(anim->animGroup->groupID, animData)];
		queue.push_back(anim);
		*result = 1;
		return true;
	});

	builder.Create("IsAnimQueued", kRetnType_Default, { ParamInfo { "sAnimPath", kParamType_String, false } }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char animPath[0x400];
		animPath[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath))
			return true;
		SetLowercase(animPath);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor || !actor->baseProcess)
			return true;
		
		auto* anim = FindActiveAnimationForActor(actor, animPath);
		if (!anim || !anim->animGroup)
			return true;
		auto* animData = actor->baseProcess->GetAnimData();
		if (animData->controllerManager != anim->m_pkOwner)
		{
			if (actor != g_thePlayer || anim->m_pkOwner != g_thePlayer->firstPersonAnimData->controllerManager)
				return true;
			animData = g_thePlayer->firstPersonAnimData;
		}
		if (const auto iter = g_queuedReplaceAnims.find(std::make_pair(anim->animGroup->groupID, animData)); iter != g_queuedReplaceAnims.end())
		{
			const auto& queue = iter->second;
			*result = ra::find(queue, anim) != queue.end();
		}
		return true;
	});

	const auto animPriorityParams = { ParamInfo{"sAnimPath", kParamType_String, false}, ParamInfo{"sInterpName", kParamType_String, false}};
	builder.Create("GetAnimInterpPriority", kRetnType_Default, animPriorityParams,false, [](COMMAND_ARGS)
	{
		*result = -1;
		char animPath[0x400];
		char interpName[0x400];
		animPath[0] = 0;
		interpName[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &interpName))
			return true;
		SetLowercase(animPath);
		auto* interp = FindAnimInterp(thisObj, animPath, interpName);
		if (!interp)
			return true;
		*result = interp->m_ucPriority;
		return true;
	});

	builder.Create("SetAnimInterpPriority", kRetnType_Default, { ParamInfo{"sAnimPath", kParamType_String, false}, ParamInfo{"sInterpName", kParamType_String, false}, ParamInfo{"iPriority", kParamType_Integer, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char animPath[0x400];
		char interpName[0x400];
		animPath[0] = 0;
		interpName[0] = 0;
		UInt32 priority = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &interpName, &priority) || priority > 255)
			return true;
		SetLowercase(animPath);
		auto* interp = FindAnimInterp(thisObj, animPath, interpName);
		if (!interp)
			return true;
		interp->m_ucPriority = static_cast<UInt8>(priority);
		*result = 1;
		return true;
	});

	builder.Create("AddAnimTextKey", kRetnType_Default, { ParamInfo{"sAnimPath", kParamType_String, false}, ParamInfo{"fTime", kParamType_Float, false}, ParamInfo{"sTextKey", kParamType_String, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char animPath[0x400];
		float time;
		char textKey[0x400];
		animPath[0] = 0;
		textKey[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &time, &textKey))
			return true;
		SetLowercase(animPath);
		const auto* anim = FindActiveAnimationForRef(thisObj, animPath);
		if (!anim)
			return true;
		NiTextKeyExtraData* textKeyData = anim->m_spTextKeys;
		if (!textKeyData)
			return true;
		textKeyData->AddKey(textKey, time);
		*result = 1;
		return true;
	});

	builder.Create("RemoveAnimTextKey", kRetnType_Default, { ParamInfo{"sAnimPath", kParamType_String, false}, ParamInfo("iIndex", kParamType_Integer, false) }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char animPath[0x400];
		animPath[0] = 0;
		UInt32 index;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &index))
			return true;
		SetLowercase(animPath);
		const auto* anim = FindActiveAnimationForRef(thisObj, animPath);
		if (!anim)
			return true;
		NiTextKeyExtraData* textKeyData = anim->m_spTextKeys;
		if (!textKeyData)
			return true;
		*result = textKeyData->RemoveKey(index);
		return true;
	});

	builder.Create("CopyAnimPriorities", kRetnType_Default, { ParamInfo{"sSourceAnimPath", kParamType_String, false}, ParamInfo{"sDestAnimPath", kParamType_String, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char sourceAnimPath[0x400];
		char destAnimPath[0x400];
		sourceAnimPath[0] = 0;
		destAnimPath[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &sourceAnimPath, &destAnimPath) || !thisObj)
			return true;
		SetLowercase(sourceAnimPath);
		SetLowercase(destAnimPath);
		auto* sourceAnim = FindActiveAnimationForRef(thisObj, sourceAnimPath);
		auto* destAnim = FindActiveAnimationForRef(thisObj, destAnimPath);
		if (!sourceAnim || !destAnim)
			return true;
		const std::span srcControlledBlocks(sourceAnim->m_pkInterpArray, sourceAnim->m_uiArraySize);
		int idx = 0;
		for (const auto& srcControlledBlock : srcControlledBlocks)
		{
			auto& tag = sourceAnim->m_pkIDTagArray[idx];
			auto* dstControlledBlock = destAnim->GetControlledBlock(tag.m_kAVObjectName.CStr());
			if (dstControlledBlock)
			{
				dstControlledBlock->m_ucPriority = srcControlledBlock.m_ucPriority;
				*result = *result + 1;
			}
			++idx;
		}
		return true;
	});

	builder.Create("IsAnimPlayingAlt", kRetnType_Default, { ParamInfo{"anim group", kParamType_AnimationGroup, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		UInt32 groupId = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &groupId) || !thisObj)
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		const auto* animData = actor == g_thePlayer ? g_thePlayer->GetAnimData() : actor->GetAnimation();
		if (!animData)
			return true;
		if (groupId >= g_animGroupInfos.size())
			return true;
		const auto& groupInfo = g_animGroupInfos[groupId];
		auto* anim = animData->animSequence[groupInfo.sequenceType];
		if (!anim || !anim->animGroup)
			return true;
		if (anim->animGroup->GetBaseGroupID() == groupId && anim->m_eState != kAnimState_Inactive)
			*result = 1;
		return true;
	});

	auto setDestFrameParams = {ParamInfo{"anim path", kParamType_String, false},
		ParamInfo{"dest frame", kParamType_Float, false},
		ParamInfo{"first person", kParamType_Integer, true},
	};
	
	builder.Create("SetAnimDestFrame", kRetnType_Default, setDestFrameParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char animPath[0x400];
		animPath[0] = 0;
		float destFrame;
		int firstPerson = false;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &destFrame, &firstPerson) || !thisObj)
			return true;
		SetLowercase(animPath);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindOrLoadAnim(actor, animPath, firstPerson);
		if (!anim)
			return true;
		const static auto sDestFrame = NiFixedString("fDestFrame");
		anim->m_spTextKeys->SetOrAddKey(sDestFrame, destFrame);
		*result = 1;
		return true;
	});

	builder.Create("GetAnimPathBySequenceType", kRetnType_String, getAnimBySequenceTypeParams, true, [](COMMAND_ARGS)
	{
		g_stringVarInterface->Assign(PASS_COMMAND_ARGS, "");
		UInt32 sequenceId = -1;
		int firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &sequenceId, &firstPerson))
			return true;
		if (sequenceId < 0 || sequenceId > kSequence_SpecialIdle)
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* animData = GetAnimData(actor, firstPerson);
		if (!animData)
			return true;
		auto* anim = animData->animSequence[sequenceId];
		if (!anim)
			return true;
		g_stringVarInterface->Assign(PASS_COMMAND_ARGS, anim->m_kName.CStr());
		return true;
	}, nullptr, "GetAnimPathByType");

	builder.Create("GetAnimHandType", kRetnType_Default, { ParamInfo{"sAnimPath", kParamType_String, false} }, true, [](COMMAND_ARGS)
	{
		*result = -1;
		char animPath[0x400];
		animPath[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath))
			return true;
		SetLowercase(animPath);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindActiveAnimationForActor(actor, animPath);
		if (!anim || !anim->animGroup)
			return true;
		*result = anim->animGroup->groupID >> 8 & 0xF;
		return true;
	});

	builder.Create("GetAnimTextKeyTime", kRetnType_Default, { ParamInfo{"sAnimPath", kParamType_String, false}, ParamInfo{"sTextKey", kParamType_String, false} }, true, [](COMMAND_ARGS)
	{
		*result = -1;
		char animPath[0x400];
		char textKey[0x400];
		animPath[0] = 0;
		textKey[0] = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &textKey))
			return true;
		SetLowercase(animPath);
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindActiveAnimationForActor(actor, animPath);
		if (!anim || !anim->animGroup)
			return true;
		NiTextKeyExtraData* textKeyData = anim->m_spTextKeys;
		if (!textKeyData)
			return true;
		const NiTextKey* key = textKeyData->FindFirstByName(textKey);
		if (!key)
			return true;
		*result = key->m_fTime;
		return true;
	});

	const auto setAdditiveParams = {ParamInfo{"additive anim path", kParamType_String, false},
		ParamInfo{"first person", kParamType_Integer, true},
		ParamInfo{"reference pose anim path", kParamType_String, true},
		ParamInfo{"reference pose time point", kParamType_Float, true},
		ParamInfo{"ignore priorities", kParamType_Integer, true},
	};
	builder.Create("SetAnimAdditive", kRetnType_Default, setAdditiveParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		sv::stack_string<0x400> animPath;
		sv::stack_string<0x400> refPoseAnimPath;
		int firstPerson = -1;
		float refPoseTimePoint = 0.0f;
		UInt32 ignorePriorities = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath.data_, &firstPerson, &refPoseAnimPath.data_, &refPoseTimePoint, &ignorePriorities))
			return true;
		for (auto* path : {&animPath, &refPoseAnimPath})
			path->to_lower();
		if (animPath.empty())
			return true;
		if (!thisObj)
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		const auto actorId = actor->refID;
		
		const auto sAnimPath = std::string(animPath.str());
		const auto sRefPoseAnimPath = std::string(refPoseAnimPath.str());
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);
		
		QueueScriptAnimOperation(actor, result, [firstPerson, actorId, sAnimPath, sRefPoseAnimPath, ignorePriorities, refPoseTimePoint]
		{
			auto* actor = static_cast<Actor*>(LookupFormByID(actorId));
			if (!actor || !actor->IsActor() || !actor->Get3D())
				return false;
			auto* animData = GetAnimData(actor, firstPerson);
			if (!animData)
				return false;
			auto* additiveAnim = FindOrLoadAnim(animData, sAnimPath.c_str());
			if (!additiveAnim)
				return false;

			BSAnimGroupSequence* refPoseAnim;
			if (!sRefPoseAnimPath.empty())
				refPoseAnim = FindOrLoadAnim(animData, sRefPoseAnimPath.c_str());
			else
				refPoseAnim = GetAnimByGroupID(animData, kAnimGroup_Idle);
			if (!refPoseAnim)
				return false;
			const auto bIgnorePriorities = static_cast<bool>(ignorePriorities);
			AdditiveManager::InitAdditiveSequence(animData, additiveAnim, refPoseAnim, refPoseTimePoint, bIgnorePriorities);
			return true;
		});
		return true;
	});

	builder.Create("ThisAnim", kRetnType_String, {}, false, [](COMMAND_ARGS)
	{
		const auto path = !g_globals.thisAnimScriptPath.empty() ? g_globals.thisAnimScriptPath : "";
		g_stringVarInterface->Assign(PASS_COMMAND_ARGS, path.data());
		return true;
	});

	builder.Create("ThisAnimDir", kRetnType_String, {}, false, [](COMMAND_ARGS)
	{
		const auto path = !g_globals.thisAnimScriptPath.empty() ? sv::get_file_directory(g_globals.thisAnimScriptPath.data()) : "";
		const sv::stack_string<0x400> resultPath = path;
		g_stringVarInterface->Assign(PASS_COMMAND_ARGS, resultPath.c_str());
		return true;
	});

	builder.Create("GetAnimWeight", kRetnType_Default, { ParamInfo{"anim sequence path", kParamType_String, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		sv::stack_string<0x400> animPath;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath))
			return true;
		animPath.to_lower();
		auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
		if (!actor)
			return true;
		const auto* anim = FindActiveAnimationForActor(actor, animPath.c_str());
		if (!anim)
			return true;
		*result = anim->m_fSeqWeight;
		return true;
	});

	const auto getDirFilesParams = { ParamInfo{"sPath", kParamType_String, false}, { "archive type", kParamType_Integer, true } };
	builder.Create("GetDirectoryFiles", kRetnType_Array, getDirFilesParams, false, [](COMMAND_ARGS)
	{
		*result = 0;
		sv::stack_string<0x400> path;
		auto archiveType = ARCHIVE_TYPE_ALL;
		if (!ExtractArgs(EXTRACT_ARGS, &path, &archiveType))
			return true;
		std::string realPath;
		if (!path.starts_with_ci("data\\"))
		{
			realPath = "data\\" + std::string(path.str());
		}
		const auto list = FileFinder::FindFiles(!realPath.empty() ? realPath.data() : path.data(), path.data(), archiveType);
		NVSEArrayBuilder arr;
		for (const auto* filePath : list)
		{
			arr.Add(filePath);
		}
		*result = reinterpret_cast<UInt32>(arr.Build(g_arrayVarInterface, scriptObj));
		return true;
	});

	// GetActiveAnims
	builder.Create("GetActiveAnims", kRetnType_Array, { ParamInfo{"bIsFirstPerson", kParamType_Integer, true} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		UInt32 firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &firstPerson))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
		if (!actor)
			return true;
		auto* animData = GetAnimData(actor, firstPerson);
		if (!animData || !animData->controllerManager)
			return true;
		NVSEArrayBuilder arr;
		for (auto* entry : animData->controllerManager->m_kActiveSequences)
		{
			if (entry && entry->m_eState != NiControllerSequence::INACTIVE)
				arr.Add(entry->m_kName.CStr());
		}
		*result = reinterpret_cast<UInt32>(arr.Build(g_arrayVarInterface, scriptObj));
		return true;
	});

	// IsAnimAdditive
	builder.Create("IsAnimAdditive", kRetnType_Default, { ParamInfo{"anim sequence path", kParamType_String, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		sv::stack_string<0x400> animPath;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath))
			return true;
		animPath.to_lower();
		auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
		if (!actor)
			return true;
		auto* additiveAnim = FindActiveAnimationForActor(actor, animPath.c_str());
		if (!additiveAnim)
			return true;
		*result = AdditiveManager::IsAdditiveSequence(additiveAnim);
		return true;
	});

	builder.Create("SetAnimCycleType", kRetnType_Default, { ParamInfo{"anim sequence path", kParamType_String, false}, ParamInfo{"cycle type", kParamType_Integer, false}, ParamInfo{"first person", kParamType_Integer, true} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		sv::stack_string<0x400> animPath;
		UInt32 cycleType;
		UInt32 firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &cycleType, &firstPerson) || !thisObj)
			return true;
		if (cycleType > NiControllerSequence::MAX_CYCLE_TYPES)
			return true;
		animPath.to_lower();
		auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(static_cast<Actor*>(thisObj));
		auto* anim = FindOrLoadAnim(actor, animPath.c_str(), firstPerson);
		if (!anim)
			return true;
		anim->m_eCycleType = static_cast<NiControllerSequence::CycleType>(cycleType);
		*result = 1;
		return true;
	});

	const auto boneParams = { ParamInfo{"anim sequence path", kParamType_String, false}, ParamInfo{"root node name", kParamType_String, false}, {"weight", kParamType_Float, false}, {"recursive", kParamType_Integer, false}, ParamInfo{"first person", kParamType_Integer, true} };
	builder.Create("SetAnimAdditiveInterpsWeightMult", kRetnType_Default, boneParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		sv::stack_string<0x400> animPath;
		sv::stack_string<0x400> nodeName;
		float weight = 1.0f;
		int firstPerson = -1;
		int recursive = false;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &nodeName, &weight, &recursive, &firstPerson) || !thisObj)
			return true;
		animPath.to_lower();
		auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
		if (!actor)
			return true;
		
		const auto actorId = actor->refID;
		const auto sAnimPath = std::string(animPath.str());
		const auto sNodeName = std::string(nodeName.str());
		
		QueueScriptAnimOperation(actor, result, [actorId, firstPerson, weight, recursive, sAnimPath, sNodeName]
		{
			auto* actor = static_cast<Actor*>(LookupFormByID(actorId));
			if (!actor || !actor->IsActor())
				return false;
			const auto* animData = GetAnimData(actor, firstPerson);
			if (!animData)
				return false;
			auto* baseNode = animData->nBip01;
			if (!baseNode) return false;
			auto* anim = FindOrLoadAnim(actor, sAnimPath.c_str(), firstPerson);
			if (!anim || !AdditiveManager::IsAdditiveSequence(anim))
				return false;
			auto* bone = baseNode->GetObjectByName(sNodeName.c_str());
			if (!bone)
				return false;
			const auto setNodeWeightMult = [&](this auto& self, NiAVObject* node) -> void
			{
				if (auto* block = anim->GetControlledBlock(node->m_pcName))
					AdditiveManager::SetAdditiveInterpWeightMult(node, block->m_spInterpolator, weight);
				if (recursive)
				{
					if (auto* niNode = node->GetAsNiNode())
					{
						for (auto* child : niNode->m_children)
							if (child)
								self(child);
					}
				}
			};
			setNodeWeightMult(bone);
			return true;
		});
		
		return true;
	});

	builder.Create("GetAnimMovementSpeedMult", kRetnType_Default, {}, true, [](COMMAND_ARGS)
	{
		*result = 0;
		const auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
		if (!actor || !actor->baseProcess)
			return true;
		auto* animData = actor->baseProcess->GetAnimData();
		if (!animData)
			return true;
		*result = animData->movementSpeedMult;
		return true;
	});
	
	builder.Create("CloneAnimation", kRetnType_Default, {}, true, [](COMMAND_ARGS)
	{
		return true;
	});

#define PARAM(name, type) ParamInfo{name, kParamType_##type, false}
#define OPT_PARAM(name, type) ParamInfo{name, kParamType_##type, true}

	builder.Create("PlayAnimSequenceEx", kRetnType_Default, {
		PARAM("sequence name", String),
		OPT_PARAM("node name", String),
		OPT_PARAM("priority", Integer),
		OPT_PARAM("ease in time", Float)
	}, true, [](COMMAND_ARGS)
	{
		*result = 0;
		sv::stack_string<0x400> sequenceName;
		sv::stack_string<0x400> nodeName;
		int priority = 0;
		float easeInTime = 0.0f;
		if (!ExtractArgs(EXTRACT_ARGS, &sequenceName, &nodeName, &priority, &easeInTime) || !thisObj)
			return true;
		auto* sceneRoot = thisObj->Get3D();
		if (!sceneRoot)
			return true;
		auto* node = nodeName.empty() ? sceneRoot :
			BSUtilities::GetObjectByName(sceneRoot, nodeName.c_str());
		if (!node)
			return true;
		auto* controller = static_cast<NiControllerManager*>(node->GetController(NiControllerManager::ms_RTTI));
		if (!controller)
			return true;
		auto* sequence = controller->GetSequenceByName(sequenceName.c_str());
		if (!sequence)
			return true;
		*result = controller->ActivateSequence(sequence, priority, true, sequence->m_fSeqWeight, easeInTime, nullptr);
		return true;
	});

	builder.Create("SetAnimSequenceFrequencyAlt", kRetnType_Default, {
		PARAM("sequence name", String),
		PARAM("frequency", Float),
		OPT_PARAM("node name", String),
	}, true, [](COMMAND_ARGS)
	{
		*result = 0;
		sv::stack_string<0x400> sequenceName;
		sv::stack_string<0x400> nodeName;
		float frequency = 1.0f;
		if (!ExtractArgs(EXTRACT_ARGS, &sequenceName, &frequency, &nodeName) || !thisObj)
			return true;
		auto* sceneRoot = thisObj->Get3D();
		if (!sceneRoot)
			return true;
		auto* node = nodeName.empty() ? sceneRoot :
			BSUtilities::GetObjectByName(sceneRoot, nodeName.c_str());
		if (!node)
			return true;
		auto* controller = static_cast<NiControllerManager*>(node->GetController(NiControllerManager::ms_RTTI));
		if (!controller)
			return true;
		auto* sequence = controller->GetSequenceByName(sequenceName.c_str());
		if (!sequence)
			return true;
		sequence->m_fFrequency = frequency;
		*result = 1.0;
		return true;
	});

#undef PARAM
#undef OPT_PARAM

#if _DEBUG

	builder.Create("kNVSEToggleSetting", kRetnType_Default, { ParamInfo{"sFeatureName", kParamType_String, false} }, false, [](COMMAND_ARGS)
	{
		sv::stack_string<0x400> featureName;
		*result = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &featureName))
			return true;

		if (featureName.str() == "blendSmoothing")
		{
			g_pluginSettings.blendSmoothing = !g_pluginSettings.blendSmoothing;
			*result = g_pluginSettings.blendSmoothing;
		}
		
		return true;
	});
#endif
}
