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
#include <array>
#include <cassert>

#include "anim_fixes.h"
#include "nihooks.h"
#include "NiNodes.h"
#include "NiObjects.h"
#include "NiTypes.h"
#include "ScriptUtils.h"
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
	return actor->GetAnimData();
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

std::map<std::pair<std::string_view, AnimData*>, BSAnimationContext> g_cachedAnimMap;


#if _DEBUG
NiTPointerMap_t<const char*, NiAVObject>::Entry entry;
#endif

/*
 * BSAnimGroupSequence ref count is 3 after LoadAnimation; it has the following references:
 * - The AnimSequenceBase
 * - The KFModel
 * - The NiControllerManager sequence list
 * After a call to DeleteAnimSequence it will be reduced to 1, only KFModel reference persists and is reused next time
 * when LoadAnimation is called.
 */
void HandleOnAnimDataDelete(AnimData* animData)
{
	while (true)
	{
		// delete all animations when anim data destructor runs
		auto iter = ra::find_if(g_cachedAnimMap, _L(auto& p, p.first.second == animData));
		if (iter == g_cachedAnimMap.end())
			break;

		auto anim = iter->second.anim;
		g_timeTrackedAnims.erase(anim);
		std::erase_if(g_timeTrackedGroups, _L(auto& iter, iter.second->anim == anim));
		std::erase_if(g_burstFireQueue, _L(auto& p, p.anim == anim));
		std::erase_if(g_cachedAnimMap, _L(auto & p, p.second.anim == anim));
	}
	std::erase_if(g_timeTrackedGroups, _L(auto & iter, iter.second->animData == animData));
}

thread_local GameAnimMap* s_customMap = nullptr;

std::optional<BSAnimationContext> LoadCustomAnimation(std::string_view path, AnimData* animData)
{
	const auto key = std::make_pair(path, animData);
	if (const auto iter = g_cachedAnimMap.find(key); iter != g_cachedAnimMap.end())
	{
		return iter->second;
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
			if (GameFuncs::LoadAnimation(animData, kfModel, false))
			{
				if (auto* base = animData->mapAnimSequenceBase->Lookup(groupId))
				{
					BSAnimGroupSequence* anim;
					if (base && ((anim = base->GetSequenceByIndex(-1))))
					{
						AnimFixes::FixWrongAKeyInRespectEndKey(animData, anim);
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
	
	auto* defaultMap = animData->mapAnimSequenceBase;

	if (!s_customMap)
		s_customMap = CreateGameAnimMap(1);
	
	animData->mapAnimSequenceBase = s_customMap;
	const auto result = tryCreateAnimation();
	animData->mapAnimSequenceBase = defaultMap;
	s_customMap->Clear();
	
	return result;
}

ICriticalSection g_loadCustomAnimationCS;

std::optional<BSAnimationContext> LoadCustomAnimation(SavedAnims& ctx, UInt16 groupId, AnimData* animData)
{
	ScopedLock lock(g_loadCustomAnimationCS);
	if (const auto* animPath = GetAnimPath(ctx, groupId, animData))
	{
		const auto animCtx = LoadCustomAnimation(animPath->path, animData);
		if (animCtx)
			ctx.linkedSequences.insert(animCtx->anim);
		return animCtx;
	}
	return std::nullopt;
}

AnimTime::~AnimTime()
{
	Revert3rdPersonAnimTimes(this->respectEndKeyData.anim3rdCounterpart, this->anim);
}

// Make sure that Aim, AimUp and AimDown all use the same index
AnimPath* HandleAimUpDownRandomness(UInt32 animGroupId, SavedAnims& anims)
{
	UInt32 baseId;
	if (const auto animGroupMinor = animGroupId & 0xFF; animGroupMinor >= kAnimGroup_Aim && animGroupMinor <= kAnimGroup_AimISDown && (baseId = kAnimGroup_Aim)
		|| animGroupMinor >= kAnimGroup_AttackLeft && animGroupMinor <= kAnimGroup_AttackSpin2ISDown && (baseId = kAnimGroup_AttackLeft) 
		|| animGroupMinor >= kAnimGroup_PlaceMine && animGroupMinor <= kAnimGroup_AttackThrow8ISDown && (baseId = kAnimGroup_PlaceMine))
	{
		static unsigned int s_lastRandomId = 0;
		if ((animGroupMinor - baseId) % 3 == 0 || s_lastRandomId >= anims.anims.size())
			s_lastRandomId = GetRandomUInt(anims.anims.size());

		return anims.anims.at(s_lastRandomId).get();
	}
	return nullptr;
}

std::list<BurstFireData> g_burstFireQueue;

TimeTrackedAnimsMap g_timeTrackedAnims;
TimeTrackedGroupsMap g_timeTrackedGroups;

enum class KeyCheckType
{
	KeyEquals, KeyStartsWith
};

std::string GetTextAfterColon(std::string in)
{
	auto str = in.substr(in.find_first_of(':') + 1);
	while (!str.empty() && isspace(*str.begin()))
		str.erase(str.begin());
	return str;
}

/*
 * 	std::unique_ptr<TimedExecution<Script*>> scriptLineKeys = nullptr;
	std::unique_ptr<TimedExecution<Script*>> scriptCallKeys = nullptr;
	std::unique_ptr<TimedExecution<Sound>> soundPaths = nullptr;
 */

std::unordered_map<BSAnimGroupSequence*, TimedExecution<Script*>> g_scriptLineExecutions;
std::unordered_map<BSAnimGroupSequence*, TimedExecution<Script*>> g_scriptCallExecutions;
std::unordered_map<BSAnimGroupSequence*, TimedExecution<Sound>> g_scriptSoundExecutions;

bool HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* anim)
{
	if (!anim || !anim->animGroup)
		return false;
	auto applied = false;
	const auto textKeys = anim->m_spTextKeys->GetKeys();
	auto* actor = animData->actor;
	AnimTime* animTimePtr = nullptr;
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
		const auto iter = map.emplace(anim, T());
		auto* timedExecution = &iter.first->second;
		const bool uninitialized = iter.second;
		return std::make_pair(timedExecution, uninitialized);
	};
	const auto hasKey = [&](const std::initializer_list<const char*> keyTexts, KeyCheckType type = KeyCheckType::KeyEquals)
	{
		auto result = false;
		for (const char* keyText : keyTexts)
		{
			switch (type)
			{
			case KeyCheckType::KeyEquals: 
				result = ra::any_of(textKeys, _L(NiTextKey &key, _stricmp(key.m_kText.CStr(), keyText) == 0));
				break;
			case KeyCheckType::KeyStartsWith:
				result = ra::any_of(textKeys, _L(NiTextKey &key, StartsWith(key.m_kText.CStr(), keyText)));
				break;
			}
			if (result)
				break;
		}
		if (result)
			applied = true;
		
		return result;
	};

	if (anim->animGroup->IsAttack() && hasKey({"burstFire"}))
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
			SubscribeOnActorReload(actor, ReloadSubscriber::BurstFire);
			g_burstFireQueue.emplace_back(animData == g_thePlayer->firstPersonAnimData, anim, 0, std::move(hitKeys), 0.0,false, -FLT_MAX, animData->actor->refID, std::move(ejectKeys), 0, false);
		}
	}
	const auto hasRespectEndKey = hasKey({"respectEndKey", "respectTextKeys"});
	if (animData == g_thePlayer->firstPersonAnimData && hasRespectEndKey)
	{
		auto& animTime = getAnimTimeStruct();
		auto& respectEndKeyData = animTime.respectEndKeyData;
		respectEndKeyData.povState = POVSwitchState::NotSet;
		animTime.respectEndKey = true;
	}
	const auto baseGroupID = anim->animGroup->GetBaseGroupID();

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
				auto* form = GetFormByID(edid.c_str());
				if (!form || !IS_ID(form, Script))
				{
					ERROR_LOG(FormatString("Text key contains invalid script %s", edid.c_str()));
					return false;
				}
				result = static_cast<Script*>(form);
				return true;
			});
		}
		auto& animTime = getAnimTimeStruct();
		animTime.scriptCalls = scriptCallKeys->CreateContext();
	}
	if (hasKey({"SoundPath:"}, KeyCheckType::KeyStartsWith))
	{
		auto& animTime = getAnimTimeStruct();

		animTime.soundPathsBase = TimedExecution<Sounds>(textKeys, [&](const char* key, Sounds& result)
		{
			if (!StartsWith(key, "SoundPath:"))
				return false;
			const auto line = GetTextAfterColon(key);
			if (line.empty())
				return false;
			result = Sounds(line);
			if (result.failed)
				return false;
			return true;
		});
		animTime.soundPaths = animTime.soundPathsBase->CreateContext();
	}
	if (hasKey({"blendToReloadLoop"}))
	{
		LoopingReloadPauseFix::g_reloadStartBlendFixes.insert(anim->m_kName.CStr());
	}
	if (hasKey({"scriptLine:", "allowAttack" }, KeyCheckType::KeyStartsWith))
	{
		auto& animTime = getAnimTimeStruct();
		auto [scriptLineKeys, uninitialized] = createTimedExecution(g_scriptLineExecutions);
		if (uninitialized)
		{
			*scriptLineKeys = TimedExecution<Script*>(textKeys, [&](const char* key, Script*& result)
			{
				if (StartsWith(key, "allowAttack"))
				{
					// already released this as beta with custom key but in future encourage scriptLine: AllowAttack
					static Script* allowAttackScript = nullptr;
					if (!allowAttackScript)
					{
						allowAttackScript = Script::CompileFromText("AllowAttack", "ScriptLineKey");
					}
					result = allowAttackScript;
					animTime.allowAttack = true;
					if (!result)
					{
						ERROR_LOG("Failed to compile script in allowAttack key");
						return false;
					}
					return true;
				}
				if (!StartsWith(key, "scriptLine:"))
					return false;
				static std::unordered_map<const char*, Script*> s_scriptLines;
				auto& cached = s_scriptLines[key]; // by pointer value
				if (cached)
				{
					result = cached;
					return true;
				}
				const auto line = GetTextAfterColon(key);
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
		animTime.scriptLines = scriptLineKeys->CreateContext();
	}

	const auto basePath = GetAnimBasePath(anim->m_kName.Str());
	if (auto iter = g_customAnimGroupPaths.find(basePath); iter != g_customAnimGroupPaths.end())
	{
		auto& animTime = getAnimTimeStruct();
		animTime.hasCustomAnimGroups = true;
		applied = true;
	}
		
	if (applied)
	{
		auto& animTime = getAnimTimeStruct();
		animTime.actorId = actor->refID;
		animTime.lastNiTime = -FLT_MAX;
		animTime.firstPerson = animData == g_thePlayer->firstPersonAnimData;
	}
	return applied;
}

bool IsLoopingReload(UInt8 groupId)
{
	return groupId >= kAnimGroup_ReloadW && groupId <= kAnimGroup_ReloadZ;
}

std::map<std::pair<FormID, SavedAnims*>, int> g_actorAnimOrderMap;

PartialLoopingReloadState g_partialLoopReloadState = PartialLoopingReloadState::NotSet;

AnimPath* GetPartialReload(SavedAnims& ctx, Actor* actor, UInt16 groupId)
{
	auto* ammoInfo = actor->baseProcess->GetAmmoInfo();
	auto* weapon = actor->GetWeaponForm();
	if (!weapon || !ammoInfo)
		return nullptr;
	std::vector<AnimPath*> reloads;
	if ((ammoInfo->count == 0 || DidActorReload(actor, ReloadSubscriber::Partial) || g_partialLoopReloadState == PartialLoopingReloadState::NotPartial) 
		&& g_partialLoopReloadState != PartialLoopingReloadState::Partial)
	{
		reloads = ctx.anims | std::views::filter([](const auto& animTimePtr) { return !animTimePtr->partialReload; })
			| std::views::transform([](const auto& animTimePtr) { return animTimePtr.get(); })
			| std::ranges::to<std::vector>();
		if (IsLoopingReload(groupId) && g_partialLoopReloadState == PartialLoopingReloadState::NotSet)
			g_partialLoopReloadState = PartialLoopingReloadState::NotPartial;
	}
	else
	{
		reloads = ctx.anims | std::views::filter([](const auto& animTimePtr) { return animTimePtr->partialReload; })
			| std::views::transform([](const auto& animTimePtr) { return animTimePtr.get(); })
			| std::ranges::to<std::vector>();
		if (IsLoopingReload(groupId) && g_partialLoopReloadState == PartialLoopingReloadState::NotSet)
			g_partialLoopReloadState = PartialLoopingReloadState::Partial;
	}
	if (reloads.empty())
		return nullptr;
	if (!ctx.hasOrder)
		return reloads.at(GetRandomUInt(reloads.size()));
	return reloads.at(g_actorAnimOrderMap[std::make_pair(actor->refID, &ctx)]++ % reloads.size());
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
			if (!ctx->MatchesConditions(actor))
				continue;
			const auto initAnimTime = [&](SavedAnims* savedAnims)
			{
				auto& animTime = g_timeTrackedGroups[std::make_pair(savedAnims, animData)];
				if (!animTime)
					animTime = std::make_unique<SavedAnimsTime>();
				animTime->conditionScript = *savedAnims->conditionScript;
				animTime->groupId = groupId;
				animTime->actorId = animData->actor->refID;
				animTime->animData = animData;
				if (const auto animCtx = LoadCustomAnimation(*savedAnims, groupId, animData))
					animTime->anim = animCtx->anim;
				return &animTime;
			};
			ctx->Load();
			SavedAnimsTime* animsTime = nullptr;
			if (ctx->conditionScript)
			{
				if (ctx->pollCondition)
					animsTime = initAnimTime(&*ctx)->get(); // init'd here so conditions can activate despite not being overridden
				NVSEArrayVarInterface::Element result;
				if (!CallFunction(*ctx->conditionScript, actor, nullptr, &result) || result.GetNumber() == 0.0)
					continue;
			}
			if (!ctx->anims.empty())
			{
				return AnimationResult(ctx, animsTime, &animStacks);
			}
		}
	}
	return std::nullopt;
}

std::optional<AnimationResult> GetAnimationFromMap(AnimOverrideMap& map, UInt32 id, UInt32 animGroupId, AnimData* animData)
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
AnimPathCache g_animPathFrameCache;

AnimPath* GetAnimPath(SavedAnims& ctx, UInt16 groupId, AnimData* animData)
{
	auto [iter, success] = g_animPathFrameCache.emplace(std::make_pair(&ctx, animData), nullptr);
	if (!success)
		return iter->second;
	
	AnimPath* savedAnimPath = nullptr;
	Actor* actor = animData->actor;

	if (IsAnimGroupReload(groupId) && ra::any_of(ctx.anims, _L(const auto & p, p->partialReload)))
	{
		if (auto* path = GetPartialReload(ctx, actor, groupId))
			savedAnimPath = path;
	}

	else if (!ctx.hasOrder)
	{
		// Make sure that Aim, AimUp and AimDown all use the same index
		if (auto* path = HandleAimUpDownRandomness(groupId, ctx))
			savedAnimPath = path;
		else
			// pick random variant
			savedAnimPath = ctx.anims.at(GetRandomUInt(ctx.anims.size())).get();
	}
	else
	{
		// ordered
		savedAnimPath = ctx.anims.at(g_actorAnimOrderMap[std::make_pair(actor->refID, &ctx)]++ % ctx.anims.size()).get();
	}
	iter->second = savedAnimPath;
	return savedAnimPath;
}

BSAnimGroupSequence* LoadAnimationPath(const AnimationResult& result, AnimData* animData, UInt16 groupId)
{
	if (const auto animCtx = LoadCustomAnimation(*result.parent, groupId, animData))
	{
		BSAnimGroupSequence* anim = animCtx->anim;
		if (!anim)
			return nullptr;
		auto& [ctx, animsTime, stacks] = result;
		SubscribeOnActorReload(animData->actor, ReloadSubscriber::Partial);
		HandleExtraOperations(animData, anim);
		if (ctx->conditionScript && animsTime)
		{
			animsTime->anim = anim;
			// ClearSameSequenceTypeGroups(anim, result.animsTime); // should no longer be needed since g_timeTrackedGroups is keyed on sequence type
		}
		return anim;
	}
	ERROR_LOG(FormatString("Failed to load animation for group %X", groupId));
	return nullptr;
}

// clear this cache every frame
AnimationResultCache g_animationResultCache;


std::optional<AnimationResult> GetActorAnimation(FullAnimGroupID animGroupId, AnimData* animData)
{
	// wait for file loading to finish
	if (g_animFileThread.joinable())
		g_animFileThread.join();
	if (!animData || !animData->actor || !animData->actor->baseProcess)
		return std::nullopt;
	auto [cache, isNew] = g_animationResultCache.emplace(std::make_pair(animGroupId, animData), std::nullopt);
	if (!isNew)
		return cache->second;
#if _DEBUG
	int _debug = 0;
#endif

	const auto getActorAnimation = [&](FullAnimGroupID animGroupId) -> std::optional<AnimationResult>
	{
		std::optional<AnimationResult> result;
		std::optional<AnimationResult> modIndexResult;

		const auto firstPerson = animData == g_thePlayer->firstPersonAnimData;
		auto& map = GetMap(firstPerson);
		auto* actor = animData->actor;
		auto& modIndexMap = GetModIndexMap(firstPerson);
		const auto getFormAnimation = [&](TESForm* form) -> std::optional<AnimationResult>
		{
			if (auto lResult = GetAnimationFromMap(map, form->refID, animGroupId, animData))
				return lResult;
			// mod index
			if (!modIndexResult.has_value())
				modIndexResult = GetAnimationFromMap(modIndexMap, form->GetModIndex(), animGroupId, animData);
			return std::nullopt;
		};
		// actor ref ID
		if ((result = getFormAnimation(actor)))
			return result;
		// weapon form ID
		if (auto* weaponInfo = actor->baseProcess->GetWeaponInfo(); weaponInfo && weaponInfo->weapon && (result = getFormAnimation(weaponInfo->weapon)))
			return result;
		// base form ID
		if (auto* baseForm = actor->baseForm; baseForm && ((result = GetAnimationFromMap(map, baseForm->refID, animGroupId, animData))))
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
	// first try base group id based anim
	std::optional<AnimationResult> result = std::nullopt;
	result = getActorAnimation(animGroupId);
	cache->second = result;
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

AnimGroupID SimpleGroupNameToId(std::string_view name)
{
	static std::unordered_map<std::string_view, AnimGroupID, CaseInsensitiveHash, CaseInsensitiveEqual> s_animGroupNameToIDMap;
	if (const auto underscorePos = name.find('_'); underscorePos != std::string_view::npos)
	{
		name = name.substr(0, underscorePos);
	}
	if (const auto spacePos = name.find(' '); spacePos != std::string_view::npos)
	{
		name = name.substr(0, spacePos);
	}
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
	const auto findFn = [&](const std::shared_ptr<SavedAnims>& a)
	{
		return ra::any_of(a->anims, _L(const auto& s, sv::equals_ci(path, s->path)));
	};
	
	if (!data.enable)
	{
		bool erased = false;
		// remove from stack
		if (const auto it = ra::find_if(stack, findFn); it != stack.end())
		{
			std::erase_if(g_timeTrackedGroups, _L(auto& it2, it2.first.first == &**it));
			stack.erase(it);
			erased = true;
		}
		return erased;
	}
	// check if stack already contains path
	if (const auto iter = std::ranges::find_if(stack, findFn); iter != stack.end())
	{
		// move iter to the top of stack
		std::rotate(iter, std::next(iter), stack.end());
		return true;
	}

	if (RegisterCustomAnimGroupAnim(path))
	{
		// we do not want to add custom anim group anims to the stack or otherwise they'll play
		return true;
	}

	std::function<bool(const Actor*)> folderCondition;
	if (FindStringCI(path, R"(\mod1\)"))
		folderCondition = [&](const Actor* actor) { return actor->HasWeaponWithMod(1); };
	else if (FindStringCI(path, R"(\mod2\)"))
		folderCondition = [&](const Actor* actor) { return actor->HasWeaponWithMod(2); };
	else if (FindStringCI(path, R"(\mod3\)"))
		folderCondition = [&](const Actor* actor) { return actor->HasWeaponWithMod(3); };
	else if (FindStringCI(path, R"(\hurt\)"))
		folderCondition = [&](const Actor* actor) { return actor->HasCrippledLegs(); };
	else if (FindStringCI(path, R"(\human\)"))
		folderCondition = [&](const Actor* actor) { return actor == g_thePlayer || IS_ID(actor->baseForm, TESNPC); };
	else if (FindStringCI(path, R"(\male\)"))
		folderCondition = [&](const Actor* actor) { return !actor->IsFemale(); };
	else if (FindStringCI(path, R"(\female\)"))
		folderCondition = [&](const Actor* actor) { return actor->IsFemale(); };
	
	// if not inserted before, treat as variant; else add to stack as separate set
	auto [_, newItem] = data.groupIdFillSet.emplace(groupId);
	if (newItem || stack.empty() || !FunctionCompare(folderCondition, stack.back()->folderCondition))
		stack.emplace_back(std::make_shared<SavedAnims>());
	
	auto& anims = *stack.back();
	anims.anims.emplace_back(std::make_unique<AnimPath>(path));
	anims.matchBaseGroupId = data.matchBaseGroupId;
	anims.conditionScript = data.conditionScript;
	anims.pollCondition = data.pollCondition;
	anims.conditionScriptText = data.conditionScriptText;
	anims.folderCondition = std::move(folderCondition);
	
	return true;
}

bool OverrideFormAnimation(AnimOverrideData& data, bool firstPerson)
{
	if (!data.identifier)
	{
		data.identifier = 0xFF;
		return OverrideModIndexAnimation(data, firstPerson);
	}
	auto& map = GetMap(firstPerson);
	return SetOverrideAnimation(data, map);
}

bool OverrideModIndexAnimation(AnimOverrideData& data, bool firstPerson)
{
	auto& map = GetModIndexMap(firstPerson);
	return SetOverrideAnimation(data, map);
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

std::unordered_map<UInt32, ReloadHandler> g_reloadTracker;

void SubscribeOnActorReload(Actor* actor, ReloadSubscriber subscriber)
{
	auto& handler = g_reloadTracker[actor->refID];
	handler.subscribers.emplace(subscriber, false);
	// auto* ammoInfo = actor->baseProcess->GetAmmoInfo();
}

bool DidActorReload(Actor* actor, ReloadSubscriber subscriber)
{
	if (IsGodMode())
		return false;
	const auto iter = g_reloadTracker.find(actor->refID);
	if (iter == g_reloadTracker.end())
		return false;
	auto* didReload = &iter->second.subscribers[subscriber];
	const auto result = *didReload;
	g_executionQueue.emplace_back([=]()
	{
		*didReload = false;
	});
	return result;
}

void HandleOnActorReload()
{
	for (auto iter = g_reloadTracker.begin(); iter != g_reloadTracker.end();)
	{
		auto& [actorId, handler] = *iter;
		auto* actor = static_cast<Actor*>(LookupFormByRefID(actorId));
		if (!actor || !actor->IsTypeActor() || actor->IsDeleted() || actor->IsDying(true) || !actor->baseProcess)
		{
			iter = g_reloadTracker.erase(iter);
			continue;
		}
		auto* curAmmoInfo = actor->baseProcess->GetAmmoInfo();
		if (!curAmmoInfo)
		{
			++iter;
			continue;
		}
		const bool isAnimActionReload = actor->IsAnimActionReload();
		if (isAnimActionReload)
		{
			ra::for_each(handler.subscribers, _L(auto &p, p.second = false));
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

ScopedList<char> GetDirectoryAnimPaths(char* path)
{
	const std::string searchPath = FormatString(R"(Data\Meshes\%s\*.kf)", path);
	const std::string renamePath = FormatString("%s\\", path);
	return FileFinder::FindFiles(searchPath.c_str(), renamePath.c_str(), ARCHIVE_TYPE_MESHES);
}

template <typename F>
bool OverrideAnimsFromScript(char* path, const F& overrideAnim)
{
	if (g_animFileThread.joinable())
		g_animFileThread.join();

	auto overrideAnims = [=]
	{
		if (!sv::get_file_extension(path).empty()) // single file
			return overrideAnim(path);
	
		// directory
		const auto animPaths = GetDirectoryAnimPaths(path);

		if (animPaths.Empty())
			return false;

		size_t numAnims = 0;
		for (const auto* animPath : animPaths)
		{
			numAnims += overrideAnim(animPath);
		}
		return numAnims != 0;
	};
	if (GetCurrentThreadId() == OSGlobals::GetSingleton()->mainThreadID)
		return overrideAnims();
	ScopedLock lock(g_executionQueueCS);
	g_synchronizedExecutionQueue.emplace_back(std::move(overrideAnims));
	return true; // assume success
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
		*result = OverrideAnimsFromScript(path, [&](const char* animPath)
		{
			const auto pooledPath = AddStringToPool(ToLower(animPath));
			AnimOverrideData animOverrideData = {
				.path = pooledPath,
				.identifier = actor->refID,
				.enable = static_cast<bool>(enable),
				.conditionScript = conditionScript,
				.pollCondition = static_cast<bool>(pollCondition),
				.matchBaseGroupId = static_cast<bool>(matchBaseGroupId)
			};
			return OverrideFormAnimation(animOverrideData, static_cast<bool>(firstPerson));
		});
	}
	catch (std::exception& e)
	{
		ShowRuntimeError(scriptObj, "SetWeaponAnimationPath: %s", e.what());
	}
	return true;
}

bool Cmd_PlayAnimationPath_Execute(COMMAND_ARGS)
{
	*result = 0;
	char path[0x400];
	auto playerPov = 0;
	
	if (!ExtractArgs(EXTRACT_ARGS, &path, &playerPov))
		return true;

	auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
	if (!actor)
		return true;

	auto* animData = GetAnimDataForPov(playerPov, actor);

	if (const auto anim = FindOrLoadAnim(animData, path))
	//if (const auto anim = GetAnimationByPath(path))
	{
		GameFuncs::TransitionToSequence(animData, anim, anim->animGroup->groupID, -1);
		*result = 1;
	}
	return true;
}

bool Cmd_kNVSEReset_Execute(COMMAND_ARGS)
{
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
	if (GameFuncs::PlayAnimGroup(actor->GetAnimData(), groupId, flags, -1, -1))
		*result = 1;
	return true;
}

#if _DEBUG

bool Cmd_kNVSETest_Execute(COMMAND_ARGS)
{
	ThisStdCall(0x9231D0, g_thePlayer->baseProcess, false, g_thePlayer->validBip01Names, g_thePlayer->baseProcess->GetAnimData(), g_thePlayer);
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

NVSEArrayVarInterface::Array* CreateAnimationObjectArray(BSAnimGroupSequence* anim, Script* callingScript, const AnimData* animData, bool includeDynamic = true)
{
	NVSEStringMapBuilder builder;

	builder.Add("sequenceName", anim->m_kName.CStr());
	builder.Add("seqWeight", anim->m_fSeqWeight);
	builder.Add("cycleType", anim->m_eCycleType);
	builder.Add("frequency", anim->m_fFrequency);
	builder.Add("beginKeyTime", anim->m_fBeginKeyTime);
	builder.Add("endKeyTime", anim->m_fEndKeyTime);
	if (includeDynamic)
	{
		builder.Add("lastTime", anim->m_fLastTime);
		builder.Add("weightedLastTime", anim->m_fWeightedLastTime);
		builder.Add("lastScaledTime", anim->m_fLastScaledTime);
		builder.Add("state", anim->m_eState);
		builder.Add("offset", anim->m_fOffset);
		builder.Add("startTime", anim->m_fStartTime);
		builder.Add("endTime", anim->m_fEndTime);
		if (animData)
		{
			builder.Add("calculatedTime", GetAnimTime(animData, anim));
			if (anim->animGroup)
			{
				builder.Add("multiplier", GetAnimMult(animData, anim->animGroup->groupID));

			}
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

	const auto* textKeyData = anim->m_spTextKeys;
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
	if (animationResult && animationResult->parent && !animationResult->parent->anims.empty())
	{
		const auto& animPaths = animationResult->parent->anims;
		for (const auto& animPath : animPaths)
		{
			result.push_back(animPath->path);
		}

	}
	return result;
}


AnimData* GetAnimData(Actor* actor, int firstPerson)
{
	if (!actor)
		return nullptr;
	switch (firstPerson)
	{
	case -1:
		if (actor != g_thePlayer)
			return actor->GetAnimData();
		return !g_thePlayer->IsThirdPerson() ? g_thePlayer->firstPersonAnimData : g_thePlayer->baseProcess->GetAnimData();
	case 0:
		return actor->baseProcess->GetAnimData();
	case 1:
	default:
		if (actor == g_thePlayer)
			return g_thePlayer->firstPersonAnimData;
		return actor->GetAnimData();
	}
}

std::unordered_set<BaseProcess*> g_allowedNextAnims;

BSAnimGroupSequence* FindActiveAnimationByPath(AnimData* animData, std::string_view path, const UInt32 groupId)
{
	if (const auto iter = g_cachedAnimMap.find(std::make_pair(path, animData)); iter != g_cachedAnimMap.end())
	{
		return iter->second.anim;
	}
	if (const auto customAnim = GetActorAnimation(groupId, animData))
	{
		const auto& animPaths = customAnim->parent->anims;
		auto iter = ra::find_if(animPaths, _L(const auto& p, p->path == path));
		if (iter != animPaths.end())
		{
			const auto animCtx = LoadCustomAnimation((*iter)->path, animData);
			if (animCtx)
			{
				BSAnimGroupSequence* anim = animCtx->anim;
				customAnim->parent->linkedSequences.insert(anim);
				return anim;
			}
			return nullptr;
		}
	}
	if (auto* sequence = animData->controllerManager->m_kSequenceMap.Lookup(path.data()))
	{
		return static_cast<BSAnimGroupSequence*>(sequence);
	}
	return nullptr;
}

BSAnimGroupSequence* FindActiveAnimationForActor(Actor* actor, BSAnimGroupSequence* baseAnim)
{
	auto* animData = actor->baseProcess->GetAnimData();
	const char* path = baseAnim->m_kName;
	const auto groupId = baseAnim->animGroup->groupID;
	if (!animData)
		return nullptr;
	baseAnim = FindActiveAnimationByPath(animData, path, groupId);
	if (!baseAnim && actor == g_thePlayer)
	{
		baseAnim = FindActiveAnimationByPath(g_thePlayer->firstPersonAnimData, path, groupId);
	}
	return baseAnim;
}

BSAnimGroupSequence* FindActiveAnimationForActor(Actor* actor, const char* path)
{
	auto* baseAnim = GetAnimationByPath(path);
	if (!baseAnim)
		return nullptr;
	return FindActiveAnimationForActor(actor, baseAnim);
}

BSAnimGroupSequence* FindActiveAnimationForRef(TESObjectREFR* thisObj, const char* path)
{
	if (!thisObj)
		return GetAnimationByPath(path);
	if (thisObj->IsActor())
		return FindActiveAnimationForActor(static_cast<Actor*>(thisObj), path);
	auto* ninode = thisObj->GetNiNode();
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
	if (auto* anim = animData->controllerManager->m_kSequenceMap.Lookup(path))
		return static_cast<BSAnimGroupSequence*>(anim);
	const auto& ctx = LoadCustomAnimation(path, animData);
	if (ctx)
		return ctx->anim;
	return nullptr;
}

BSAnimGroupSequence* FindOrLoadAnim(Actor* actor, const char* path, bool firstPerson)
{
	auto* animData = firstPerson ? g_thePlayer->firstPersonAnimData : actor->baseProcess->GetAnimData();
	if (!animData)
		return nullptr;
	return FindOrLoadAnim(animData, path);
}

float GetIniFloat(UInt32 addr)
{
	return ThisStdCall<SettingT::Info*>(0x403E20, reinterpret_cast<void*>(addr))->f;
}

float GetDefaultBlendTime(const BSAnimGroupSequence* destSequence, const BSAnimGroupSequence* sourceSequence)
{
	const auto defaultBlend = GetIniFloat(0x11C56FC);
	const auto blendMult = GetIniFloat(0x11C5724);

	const float sourceBlendOut = sourceSequence ? max(destSequence->animGroup->blend, destSequence->animGroup->blendOut) : 0;
	const float destBlendIn = max(destSequence->animGroup->blend, destSequence->animGroup->blendIn);

	float blendValue;

	if (destBlendIn > sourceBlendOut)
		blendValue = destBlendIn;
	else if (sourceBlendOut > 0)
		blendValue = sourceBlendOut;
	else
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
	return nearestGroupId;
}

NiControllerSequence::InterpArrayItem* FindAnimInterp(TESObjectREFR* thisObj, const char* animPath, const char* interpName)
{
	auto* anim = FindActiveAnimationForRef(thisObj, animPath);
	if (!anim)
		return nullptr;
	return anim->GetControlledBlock(interpName);
}

void CreateCommands(NVSECommandBuilder& builder)
{
	constexpr auto getAnimBySequenceTypeParams = {
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
		*result = reinterpret_cast<UInt32>(CreateAnimationObjectArray(anim, scriptObj, animData, true));
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
		if (!ExtractArgs(EXTRACT_ARGS, &path))
			return true;
		auto* anim = GetAnimationByPath(path);
		if (!anim)
			return true;
		const AnimData* animData = nullptr;
		if (thisObj)
		{
			if (auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor))
			{
				anim = FindActiveAnimationForActor(actor, anim);
				animData = actor->baseProcess->GetAnimData();
			}
		}
		if (!anim)
			return true;
		*result = reinterpret_cast<UInt32>(CreateAnimationObjectArray(anim, scriptObj, animData, thisObj != nullptr));
		return true;
	}, nullptr, "GetAnimAttrs");

	constexpr auto getAnimsByAnimGroupParams = {
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

	builder.Create("SetAnimTextKeys", kRetnType_Default, { ParamInfo{"anim path", kNVSEParamType_String, false}, ParamInfo{"text key times", kNVSEParamType_Array, false}, ParamInfo{"text key values", kNVSEParamType_Array, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		PluginExpressionEvaluator eval(PASS_COMMAND_ARGS);
		if (!eval.ExtractArgs() || eval.NumArgs() != 3)
			return true;
		auto* path = eval.GetNthArg(0)->GetString();
		auto* textKeyTimesArray = eval.GetNthArg(1)->GetArrayVar();
		auto* textKeyValuesArray = eval.GetNthArg(2)->GetArrayVar();
		if (!path || !textKeyTimesArray || !textKeyValuesArray)
			return true;
		const auto* kfModel = ModelLoader::LoadKFModel(path);
		auto* anim = kfModel->controllerSequence;
		if (thisObj && anim && anim->animGroup)
		{
			auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
			if (!actor)
				return true;
			anim = FindActiveAnimationForActor(actor, anim);
		}
		
		if (!anim)
		{
			ERROR_LOG("SetAnimationTextKeys: animation not found");
			return true;
		}
		if (anim->m_eState != NiControllerSequence::INACTIVE)
		{
			ERROR_LOG("SetAnimationKeys: you can not call this function while the animation is playing.");
			return true;
		}

		const auto textKeyTimesVector= NVSEArrayToVector<double>(g_arrayVarInterface, textKeyTimesArray);
		const auto textKeyValuesVector = NVSEArrayToVector<BSString>(g_arrayVarInterface, textKeyValuesArray);

		if (textKeyTimesVector.size() != textKeyValuesVector.size())
		{
			ERROR_LOG("SetAnimationTextKeys: text key times and values arrays must be the same size");
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
		
		NiPointer oldAnimGroup = anim->animGroup;
		const auto* animGroupInfo = oldAnimGroup->GetGroupInfo();

		// let the game parse the text keys
		auto oldKeyArray = std::move(anim->m_spTextKeys->m_kKeyArray);
		anim->m_spTextKeys->m_kKeyArray = std::move(newKeyArray);
		anim->animGroup = nullptr;
		NiFixedString oldSequenceName = anim->m_kName;
		anim->m_kName = animGroupInfo->name; // yes, the game initially has the sequence name set to the anim group name
		NiPointer animGroup = TESAnimGroup::Init(anim, kfModel->path);
		anim->m_kName = oldSequenceName;

		if (!animGroup)
		{
			anim->m_spTextKeys->m_kKeyArray = std::move(oldKeyArray);
			anim->animGroup = oldAnimGroup;
			
			ERROR_LOG("SetAnimationTextKeys: Game failed to parse text key data, see falloutnv_error.log");
			*result = 0;
			return true;
		}
		anim->animGroup = animGroup;
		*result = 1;
		return true;
	}, Cmd_Expression_Plugin_Parse);

	builder.Create("AllowAttack", kRetnType_Default, {}, true, [](COMMAND_ARGS)
	{
		*result = 0;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
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
		float time;
		if (!ExtractArgs(EXTRACT_ARGS, &path, &time))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindActiveAnimationForActor(actor, path);
		if (!anim)
			return true;
		AnimData* animData;
		if (actor == g_thePlayer && anim->m_pkOwner == g_thePlayer->firstPersonAnimData->controllerManager)
			animData = g_thePlayer->firstPersonAnimData;
		else
			animData = actor->baseProcess->GetAnimData();
		if (anim->m_eState != NiControllerSequence::ANIMATING)
		{
			*result = 0;
			return true;
		}

		anim->m_fOffset = time - animData->timePassed;
		const auto animTime = GetAnimTime(animData, anim);
		if (animTime < anim->m_fBeginKeyTime)
		{
			anim->m_fOffset = anim->m_fEndKeyTime - (anim->m_fBeginKeyTime - anim->m_fOffset);
		}
		else if (animTime > anim->m_fEndKeyTime)
		{
			anim->m_fOffset = anim->m_fBeginKeyTime + (anim->m_fOffset - anim->m_fEndKeyTime);
		}
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
		
		for (auto* map : {&g_animGroupFirstPersonMap, &g_animGroupThirdPersonMap})
		{
			if (const auto iter = map->find(fromForm->refID); iter != map->end())
			{
				auto& entry = iter->second;
				for (auto& stacks : entry.stacks | std::views::values)
				{
					auto& stack = stacks.anims;
					for (auto& anims : stack)
					{
						std::unordered_set<UInt16> variants;
						for (const auto& anim : anims->anims)
						{
							AnimOverrideData animOverrideData = {
								.path = anim->path,
								.identifier = toForm->refID,
								.enable = false,
								.conditionScript = *anims->conditionScript,
								.pollCondition = anims->pollCondition,
								.matchBaseGroupId = false
							};
							SetOverrideAnimation(animOverrideData, *map);
							animOverrideData.enable = true;
							SetOverrideAnimation(animOverrideData, *map);
							*result = 1;
						}
					}
				}
			}
		}
		return true;
	});


	builder.Create("IsAnimSequencePlaying", kRetnType_Default, { ParamInfo{"path", kParamType_String, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char path[0x400];
		if (!ExtractArgs(EXTRACT_ARGS, &path))
			return true;
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

	constexpr auto activateAnimParams = {
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
		int firstPerson = -1;
		int priority = 0;
		int startOver = 1;
		float weight = FLT_MIN;
		float easeInTime = 0.0f;
		char timeSyncSequence[0x400];
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &firstPerson, &priority, &startOver, &weight, &easeInTime, &timeSyncSequence))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
		{
			firstPerson = IsPlayerInFirstPerson(actor);
		}
		auto* anim = FindOrLoadAnim(actor, sequencePath, firstPerson);
		if (!anim)
			return true;
		if (weight == FLT_MIN)
			weight = anim->m_fSeqWeight;
		BSAnimGroupSequence* timeSyncSeq = nullptr;
		if (timeSyncSequence[0])
		{
			timeSyncSeq = FindOrLoadAnim(actor, timeSyncSequence, firstPerson);
			if (!timeSyncSeq)
				return true;
		}
		auto* manager = anim->m_pkOwner;

		if (anim->m_eState != kAnimState_Inactive)
			// Deactivate sequence
			GameFuncs::DeactivateSequence(manager, anim, 0.0);

		GameFuncs::ActivateSequence(manager, anim, priority, startOver, weight, easeInTime, timeSyncSeq);
		return true;
	});

	/*
	 NiControllerManager::DeactivateSequence(
        NiControllerManager *unused,
        NiControllerSequence *pkSequence,
        float fEaseOutTime)
	 */
	constexpr auto deactivateAnimParams = {
			ParamInfo{"anim sequence path", kParamType_String, false},
			ParamInfo{"fEaseOutTime", kParamType_Float, true}
	};
	builder.Create("DeactivateAnim", kRetnType_Default, deactivateAnimParams, true, [](COMMAND_ARGS)
	{
		*result = 0;
		char sequencePath[0x400];
		float easeOutTime = 0.0f;
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &easeOutTime))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindActiveAnimationForActor(actor, sequencePath);
		if (!anim)
			return true;
		auto* manager = anim->m_pkOwner;
		*result = GameFuncs::DeactivateSequence(manager, anim, easeOutTime);
		return true;
	});

	constexpr auto crossFadeParams = {
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
		float easeInTime = FLT_MIN;
		int priority = 0;
		float weight = FLT_MIN;
		int startOver = 1;
		if (!ExtractArgs(EXTRACT_ARGS, &sourceSequence, &destSequence, &firstPerson, &easeInTime, &priority, &startOver, &weight, &timeSyncSequence))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);
		auto* sourceAnim = FindOrLoadAnim(actor, sourceSequence, firstPerson);
		auto* destAnim = FindOrLoadAnim(actor, destSequence, firstPerson);
		BSAnimGroupSequence* timeSyncAnim = nullptr;

		if (strlen(timeSyncSequence))
		{
			timeSyncAnim = FindOrLoadAnim(actor, timeSyncSequence, firstPerson);
			if (!timeSyncAnim)
				return true;
		}

		if (!sourceAnim || !destAnim)
			return true;

		if (weight == FLT_MIN)
			weight = destAnim->m_fSeqWeight;

		if (easeInTime == FLT_MIN)
			easeInTime = GetDefaultBlendTime(destAnim, sourceAnim);

		auto* manager = sourceAnim->m_pkOwner;

		if (destAnim->m_eState != kAnimState_Inactive)
			GameFuncs::DeactivateSequence(destAnim->m_pkOwner, destAnim, 0.0f);

		*result = GameFuncs::CrossFade(
			manager,
			sourceAnim,
			destAnim,
			easeInTime,
			priority,
			startOver,
			weight,
			timeSyncAnim
		);
		return true;
	});

	constexpr auto blendToAnimSequenceParams = {
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
		float destFrame = 0.0f;
		float duration = FLT_MIN;
		int priority = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &firstPerson, &duration, &destFrame, &priority, &timeSyncSequence))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		if (firstPerson == -1)
			firstPerson = IsPlayerInFirstPerson(actor);
		auto* anim = FindOrLoadAnim(actor, sequencePath, firstPerson);
		if (!anim)
			return true;
		BSAnimGroupSequence* timeSyncAnim = nullptr;
		if (strlen(timeSyncSequence))
		{
			timeSyncAnim = FindOrLoadAnim(actor, timeSyncSequence, firstPerson);
			if (!timeSyncAnim)
				return true;
		}
		if (duration == FLT_MIN)
			duration = GetDefaultBlendTime(anim);
		auto* manager = anim->m_pkOwner;
		if (anim->m_eState != kAnimState_Inactive)
			GameFuncs::DeactivateSequence(manager, anim, 0.0f);
		*result = GameFuncs::BlendFromPose(
			manager,
			anim,
			destFrame,
			duration,
			priority,
			timeSyncAnim
		);
		return true;
	});

	constexpr auto blendFromAnimSequenceParams = {
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
		int firstPerson = -1;
		float duration = FLT_MIN;
		float destFrame = 0.0f;
		float sourceWeight = FLT_MIN;
		float destWeight = FLT_MIN;
		int priority = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &sourceSequence, &destSequence, &firstPerson, &duration, &destFrame, &sourceWeight,
		                 &destWeight, &priority, &timeSyncSequence))
			return true;
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
		*result = GameFuncs::StartBlend(sourceAnim, destAnim, duration, destFrame, priority, sourceWeight, destWeight, timeSyncAnim);
		return true;
	});

	builder.Create("SetAnimWeight", kRetnType_Default, { ParamInfo{"anim sequence path", kParamType_String, false }, ParamInfo{"weight", kParamType_Float, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char sequencePath[0x400];
		float weight = 0.0f;
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &weight) || !thisObj)
			return true;
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
		if (!ExtractArgs(EXTRACT_ARGS, &path))
			return true;
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
		int firstPerson = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &firstPerson))
			return true;
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
		if (!ExtractArgs(EXTRACT_ARGS, &animPath))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
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
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &interpName))
			return true;
		auto* interp = FindAnimInterp(thisObj, animPath, interpName);
		if (!interp)
			return true;
		*result = interp->priority;
		return true;
	});

	builder.Create("SetAnimInterpPriority", kRetnType_Default, { ParamInfo{"sAnimPath", kParamType_String, false}, ParamInfo{"sInterpName", kParamType_String, false}, ParamInfo{"iPriority", kParamType_Integer, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char animPath[0x400];
		char interpName[0x400];
		UInt32 priority = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &interpName, &priority) || priority > 255)
			return true;
		auto* interp = FindAnimInterp(thisObj, animPath, interpName);
		if (!interp)
			return true;
		interp->priority = static_cast<UInt8>(priority);
		*result = 1;
		return true;
	});

	builder.Create("AddAnimTextKey", kRetnType_Default, { ParamInfo{"sAnimPath", kParamType_String, false}, ParamInfo{"fTime", kParamType_Float, false}, ParamInfo{"sTextKey", kParamType_String, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char animPath[0x400];
		float time;
		char textKey[0x400];
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &time, &textKey))
			return true;
		const auto* anim = FindActiveAnimationForRef(thisObj, animPath);
		if (!anim)
			return true;
		auto* textKeyData = anim->m_spTextKeys;
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
		UInt32 index;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &index))
			return true;
		const auto* anim = FindActiveAnimationForRef(thisObj, animPath);
		if (!anim)
			return true;
		auto* textKeyData = anim->m_spTextKeys;
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
		if (!ExtractArgs(EXTRACT_ARGS, &sourceAnimPath, &destAnimPath) || !thisObj)
			return true;
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
				dstControlledBlock->priority = srcControlledBlock.priority;
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
		const auto* animData = actor->GetAnimData();
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
		float destFrame;
		int firstPerson = false;
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &destFrame, &firstPerson) || !thisObj)
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindOrLoadAnim(actor, animPath, firstPerson);
		if (!anim)
			return true;
		anim->m_spTextKeys->SetOrAddKey("fDestFrame", destFrame);
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
		if (!ExtractArgs(EXTRACT_ARGS, &animPath))
			return true;
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
		if (!ExtractArgs(EXTRACT_ARGS, &animPath, &textKey))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindActiveAnimationForActor(actor, animPath);
		if (!anim || !anim->animGroup)
			return true;
		auto* textKeyData = anim->m_spTextKeys;
		if (!textKeyData)
			return true;
		const NiTextKey* key = textKeyData->FindFirstByName(textKey);
		if (!key)
			return true;
		*result = key->m_fTime;
		return true;
	});

#if _DEBUG
	
	builder.Create("EachFrame", kRetnType_Default, {ParamInfo{"sScript", kParamType_String, false}}, false, [](COMMAND_ARGS)
	{
		if (!IsConsoleMode())
		{
			Console_Print("Do not call EachFrame from a script");
			return true;
		}
		*result = 0;
		char text[0x400];
		if (!ExtractArgs(EXTRACT_ARGS, &text) )
			return true;
		g_eachFrameScriptLines.push_back(text);
		*result = 1;
		return true;
	});

	builder.Create("ClearEachFrame", kRetnType_Default, {}, false, [](COMMAND_ARGS)
	{
		*result = 0;
		if (!IsConsoleMode())
		{
			Console_Print("Do not call ClearEachFrame from a script");
			return true;
		}
		g_eachFrameScriptLines.clear();
		*result = 1;
		return true;
	});

	builder.Create("ForceAttack", kRetnType_Default, { ParamInfo{"animGroup", kParamType_AnimationGroup, false} }, true, [](COMMAND_ARGS)
	{
		*result = 0;
		UInt32 animGroupId = -1;
		if (!ExtractArgs(EXTRACT_ARGS, &animGroupId) || !thisObj)
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor || !actor->GetAnimData())
			return true;
		auto* baseProcess = actor->baseProcess;
		if (!baseProcess || !baseProcess->animData)
			return true;
		g_allowedNextAnims.insert(baseProcess);
		ScopedLock lock(g_executionQueueCS);
		g_executionQueue.emplace_back([=]
		{
			g_allowedNextAnims.erase(baseProcess);
		});
		*result = GameFuncs::Actor_Attack(actor, animGroupId);
		return true;
	});


	static std::initializer_list<ParamInfo> kParams_ThisCall = {
		{ "address", kNVSEParamType_Number, 0 },
		{"arg0", kNVSEParamType_FormOrNumber, 1},
		{"arg1", kNVSEParamType_FormOrNumber, 1},
		{"arg2", kNVSEParamType_FormOrNumber, 1},
		{"arg3", kNVSEParamType_FormOrNumber, 1},
		{"arg4", kNVSEParamType_FormOrNumber, 1},
		{"arg5", kNVSEParamType_FormOrNumber, 1},

	};
	builder.Create("ThisCall", kRetnType_Default, kParams_ThisCall, true, [](COMMAND_ARGS)
	{
		*result = 0;
		if (PluginExpressionEvaluator eval(PASS_COMMAND_ARGS); eval.ExtractArgs() && thisObj)
		{
			const auto address = static_cast<UInt32>(eval.GetNthArg(0)->GetInt());
			const auto* ref = thisObj;
			std::vector<unsigned int> args;
			for (int i = 1; i < eval.NumArgs(); ++i)
			{
				const auto arg = eval.GetNthArg(i);
				if (arg->GetType() == kTokenType_Number)
					args.push_back(arg->GetInt());
				else if (arg->GetType() == kTokenType_Form)
					args.push_back(reinterpret_cast<UInt32>(arg->GetTESForm()));
				else
					return true;
			}
			*result = 1;
			switch (args.size())
			{
			case 0:
				*result = ThisStdCall<UInt32>(address, ref);
				break;
			case 1:
				*result = ThisStdCall<UInt32>(address, ref, args[0]);
				break;
			case 2:
				*result = ThisStdCall<UInt32>(address, ref, args[0], args[1]);
				break;
			case 3:
				*result = ThisStdCall<UInt32>(address, ref, args[0], args[1], args[2]);
				break;
			case 4:
				*result = ThisStdCall<UInt32>(address, ref, args[0], args[1], args[2], args[3]);
				break;
			case 5:
				*result = ThisStdCall<UInt32>(address, ref, args[0], args[1], args[2], args[3], args[4]);
				break;
			default:
				*result = 0;
				break;
			}
		}
		return true;
	}, Cmd_Expression_Plugin_Parse);
#endif
}



#if 0
bool Cmd_GetOverrideAnimations_Execute(COMMAND_ARGS)
{
	auto animGroupID = static_cast<AnimGroupID>(0);
	UInt32 firstPerson = false;
	if (!ExtractArgs(EXTRACT_ARGS, &animGroupID, &firstPerson) || !thisObj)
		return true;
	auto& maps = g_firstPersonMaps;
	for (auto iter = maps.rbegin(); iter != maps.rend(); ++iter)
	{
		auto& map = *iter;
		if (auto groupMapIter = map.find(thisObj->refID); groupMapIter != map.end())
		{
			auto& groupMap = groupMapIter->second.stacks;
			if (auto stacksIter = groupMap.find(animGroupID); stacksIter != groupMap.end())
			{
				auto& stacks = stacksIter->second;
				//stacks.GetCustom()
			}
		}

	}
	return true;
}
#endif
// SetWeaponAnimationPath WeapNV9mmPistol 1 1 "characters\_male\idleanims\weapons\1hpAttackRight.kf"
// SetWeaponAnimationPath WeapNVHuntingShotgun 1 1 "characters\_male\idleanims\weapons\2hrAttack7.kf"
// SetActorAnimationPath 0 1 "characters\_male\idleanims\sprint\Haughty.kf"
// SetWeaponAnimationPath WeapNV9mmPistol 1 1 "characters\_male\idleanims\weapons\pistol.kf"



