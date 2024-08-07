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

#include "anim_fixes.h"
#include "nihooks.h"
#include "NiNodes.h"
#include "NiObjects.h"
#include "NiTypes.h"
#include "ScriptUtils.h"

std::span<TESAnimGroup::AnimGroupInfo> g_animGroupInfos = { reinterpret_cast<TESAnimGroup::AnimGroupInfo*>(0x11977D8), 245 };
JSONAnimContext g_jsonContext;

AnimOverrideMap g_animGroupThirdPersonMap;
AnimOverrideMap g_animGroupFirstPersonMap;


// mod index map
AnimOverrideMap g_animGroupModIdxThirdPersonMap;
AnimOverrideMap g_animGroupModIdxFirstPersonMap;

std::unordered_map<AnimData*, GameAnimMap*> g_customMaps;


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
std::string ExtractFolderPath(const std::string& path, std::string& fileName)
{
	const auto lastSlash = path.find_last_of('\\');
	auto result = path.substr(0, lastSlash);
	fileName = path.substr(lastSlash + 1);
	return result;
}

std::string GetFileNameNoExtension(const std::string& fileName, std::string& extension)
{
	const auto pos = fileName.find_last_of('.');
	auto result = fileName.substr(0, pos);
	extension = fileName.substr(pos + 1);
	return result;
}


std::vector<std::string> GetAnimationVariantPaths(const std::string& kfFilePath)
{
	std::string fileName;
	const auto folderPath = ExtractFolderPath(kfFilePath, fileName);
	std::string extension;
	const auto animName = GetFileNameNoExtension(fileName, extension);
	if (_stricmp(extension.c_str(), "kf") != 0)
	{
		throw std::exception("Animation file does not end with .KF");
	}
	std::vector<std::string> result;
	const auto fullFolderPath = FormatString("Data\\Meshes\\%s", folderPath.c_str());
	for (std::filesystem::directory_iterator iter(fullFolderPath), end; iter != end; ++iter)
	{
		std::string iterExtension;
		if (_stricmp(iter->path().extension().string().c_str(), ".kf") == 0)
		{
			const auto& iterFileName = iter->path().filename().string();
			if (iterFileName.rfind(animName + '_', 0) == 0)
			{
				result.push_back(iterFileName.substr(strlen("Data\\Meshes\\")));
			}
		}
	}
	return result;
}

static ModelLoader** g_modelLoader = reinterpret_cast<ModelLoader**>(0x011C3B3C);

GameAnimMap* CreateGameAnimMap()
{
	// see 0x48F9F1
	auto* alloc = static_cast<GameAnimMap*>(FormHeap_Allocate(0x10));
	return GameFuncs::NiTPointerMap_Init(alloc, 0x65);
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

KFModel* GetKFModel(const char* path)
{
	return GameFuncs::LoadKFModel(*g_modelLoader, path);
}

BSAnimGroupSequence* GetAnimationByPath(const char* path)
{
	const auto* kfModel = GetKFModel(path);
	return kfModel ? kfModel->controllerSequence : nullptr;
}

std::map<std::pair<std::string, AnimData*>, BSAnimationContext> g_cachedAnimMap;

void HandleOnSequenceDestroy(BSAnimGroupSequence* anim)
{
	g_timeTrackedAnims.erase(anim);
	std::erase_if(g_timeTrackedGroups, _L(auto& iter, iter.second->anim == anim));
	std::erase_if(g_burstFireQueue, _L(auto& p, p.anim == anim));
	std::erase_if(g_cachedAnimMap, _L(auto & p, p.second.anim == anim));
}

NiTPointerMap_t<const char*, NiAVObject>::Entry entry;

// causes crashes
void HandleGarbageCollection()
{
	for (const auto& iter : g_customMaps)
	{
		std::vector<UInt32> keysToRemove;
		auto* map = iter.second;
		auto* animData = iter.first;
		for (auto* entry : *map)
		{
			auto* base = entry->data;
			auto* anim = base->GetSequenceByIndex(-1);
			if (!anim) 
				continue;

			if (anim->state == kAnimState_Inactive)
			{
				GameFuncs::NiControllerManager_RemoveSequence(animData->controllerManager, anim);
				HandleOnSequenceDestroy(anim); // This should be called automatically in destructor
				base->Destroy(true);
				keysToRemove.push_back(entry->key);
			}
		}
		for (const auto toRemove : keysToRemove)
		{
			GameFuncs::NiTPointerMap_RemoveKey(map, toRemove);
		}
	}
}


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
		iter->second.base->Destroy(true);
		// refcount will be 1 and sequence not destroyed but still better to end any time tracked anims

		HandleOnSequenceDestroy(iter->second.anim);
		// g_cachedAnimMap.erase is called from HandleOnSequenceDestroy
	}
	if (auto iter = g_customMaps.find(animData); iter != g_customMaps.end())
	{
		// delete custom anim map created by us
		GameFuncs::NiTPointerMap_Delete(iter->second, true);
		g_customMaps.erase(iter);
	}
	std::erase_if(g_timeTrackedGroups, _L(auto & iter, iter.second->animData == animData));
}

std::optional<BSAnimationContext> LoadCustomAnimation(const std::string& path, AnimData* animData)
{
	const auto key = std::make_pair(path, animData);
	if (const auto iter = g_cachedAnimMap.find(key); iter != g_cachedAnimMap.end())
	{
		const auto ctx = iter->second;
		return ctx;
	}

	const auto tryCreateAnimation = [&]() -> std::optional<BSAnimationContext>
	{
		auto* kfModel = GameFuncs::LoadKFModel(*g_modelLoader, path.c_str());
		if (kfModel && kfModel->animGroup && animData)
		{
			const auto groupId = kfModel->animGroup->groupID;

			if (auto* base = animData->mapAnimSequenceBase->Lookup(groupId))
			{
				// fix memory leak, can't previous anim in map since it might be blending
				auto* anim = base->GetSequenceByIndex(-1);
				if (anim && _stricmp(anim->sequenceName, path.c_str()) == 0)
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
						// anim->destFrame = kfModel->controllerSequence->destFrame;
						const auto& [entry, success] = g_cachedAnimMap.emplace(key, BSAnimationContext(anim, base));
						return entry->second;
					}
					DebugPrint("Map returned null anim");
				}
				else
					DebugPrint("Failed to lookup anim");
			}
			else
				DebugPrint(FormatString("Failed to load anim %s for anim data of actor %X", path.c_str(), animData->actor->refID));
		}
		else
			DebugPrint("Failed to load KF Model " + path);
		return std::nullopt;
	};

	auto*& customMap = g_customMaps[animData];
	if (!customMap)
		customMap = CreateGameAnimMap();

	auto* defaultMap = animData->mapAnimSequenceBase;

	animData->mapAnimSequenceBase = customMap;
	auto result = tryCreateAnimation();
	animData->mapAnimSequenceBase = defaultMap;
	
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

std::map<BSAnimGroupSequence*, std::shared_ptr<AnimTime>> g_timeTrackedAnims;


std::map<std::pair<SavedAnims*, AnimData*>, std::shared_ptr<SavedAnimsTime>> g_timeTrackedGroups;

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

std::unordered_map<std::string, TimedExecution<Script*>> g_scriptLineExecutions;
std::unordered_map<std::string, TimedExecution<Script*>> g_scriptCallExecutions;
std::unordered_map<std::string, TimedExecution<Sound>> g_scriptSoundExecutions;

bool HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* anim)
{
	if (!anim || !anim->animGroup)
		return false;
	auto applied = false;
	std::span textKeys{ anim->textKeyData->m_pKeys, anim->textKeyData->m_uiNumKeys };
	auto* actor = animData->actor;
	AnimTime* animTimePtr = nullptr;
	const auto getAnimTimeStruct = [&]() -> AnimTime&
	{
		if (!animTimePtr)
		{
			const auto iter = g_timeTrackedAnims.emplace(anim, std::make_shared<AnimTime>(actor, anim));
			animTimePtr = iter.first->second.get();
		}
		return *animTimePtr;
	};
	const auto createTimedExecution = [&]<typename T>(std::unordered_map<std::string, T>& map)
	{
		const auto iter = map.emplace(anim->sequenceName, T());
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
					DebugPrint(FormatString("Text key contains invalid script %s", edid.c_str()));
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

		animTime.soundPathsBase = TimedExecution<Sound>(textKeys, [&](const char* key, Sound& result)
		{
			if (!StartsWith(key, "SoundPath:"))
				return false;
			const auto line = GetTextAfterColon(key);
			if (line.empty())
				return false;
			const auto path = "Data\\Sound\\" + line;
			result = Sound::InitByFilename(path.c_str());
			if (!result.soundID)
				return false;
			return true;
		});
		animTime.soundPaths = animTime.soundPathsBase->CreateContext();
	}
	if (hasKey({"blendToReloadLoop"}))
	{
		LoopingReloadPauseFix::g_reloadStartBlendFixes.insert(anim->sequenceName);
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
					result = Script::CompileFromText("AllowAttack", "ScriptLineKey");
					animTime.allowAttack = true;
					if (!result)
					{
						DebugPrint("Failed to compile script in allowAttack key");
						return false;
					}
					return true;
				}
				if (!StartsWith(key, "scriptLine:"))
					return false;
				auto line = GetTextAfterColon(key);
				if (line.empty())
					return false;
				auto formattedLine = ReplaceAll(line, "%R", "\r\n");
				formattedLine = ReplaceAll(formattedLine, "%r", "\r\n");
				result = Script::CompileFromText(formattedLine, "ScriptLineKey");
				if (!result)
				{
					DebugPrint("Failed to compile script in scriptLine key: " + line);
					return false;
				}
				return true;
			});
		}
		animTime.scriptLines = scriptLineKeys->CreateContext();
	}

	const auto basePath = GetAnimBasePath(anim->sequenceName);
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

		StackVector<AnimCustom, static_cast<size_t>(AnimCustom::Max) + 1> animCustomStack;
		animCustomStack->push_back(AnimCustom::None);

		auto* npc = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC);
		if (actor == g_thePlayer || npc)
			animCustomStack->push_back(AnimCustom::Human);
		if (npc)
		{
			if (npc && !npc->baseData.IsFemale())
				animCustomStack->push_back(AnimCustom::Male);
			else
				animCustomStack->push_back(AnimCustom::Female);
		}
		if (actor->avOwner.GetActorValueInt(kAVCode_LeftMobilityCondition) <= 0 || actor->avOwner.GetActorValueInt(kAVCode_RightMobilityCondition) <= 0 && IsAnimGroupMovement(groupId))
		{
			BSAnimGroupSequence* toReplaceAnim = nullptr;
			if (auto* base = animData->mapAnimSequenceBase->Lookup(groupId))
				toReplaceAnim = base->GetSequenceByIndex(-1);
			if (toReplaceAnim && FindStringCI(toReplaceAnim->sequenceName, "\\hurt\\")) // there really is no state on an animation which indicates it's a crippled anim
			{
				animCustomStack->clear();
				animCustomStack->push_back(AnimCustom::Hurt);
			}
		}
		if (auto* weaponInfo = actor->baseProcess->GetWeaponInfo(); weaponInfo && weaponInfo->GetExtraData())
		{
			const auto* xData = weaponInfo->GetExtraData();
			auto* modFlags = static_cast<ExtraWeaponModFlags*>(xData->GetByType(kExtraData_WeaponModFlags));
			if (modFlags && modFlags->flags)
			{
				if (modFlags->flags & 1 && !animStacks.mod1Anims.empty())
					animCustomStack->push_back(AnimCustom::Mod1);
				if (modFlags->flags & 2 && !animStacks.mod2Anims.empty())
					animCustomStack->push_back(AnimCustom::Mod2);
				if (modFlags->flags & 4 && !animStacks.mod3Anims.empty())
					animCustomStack->push_back(AnimCustom::Mod3);
			}
		}
		for (auto& animCustomIter : ra::reverse_view(*animCustomStack))
		{
			auto& stack = animStacks.GetCustom(animCustomIter);
			for (auto& ctx : ra::reverse_view(stack))
			{
				const auto initAnimTime = [&](SavedAnims* savedAnims)
				{
					auto& animTime = g_timeTrackedGroups[std::make_pair(savedAnims, animData)];
					if (!animTime)
						animTime = std::make_shared<SavedAnimsTime>();
					animTime->conditionScript = **savedAnims->conditionScript;
					animTime->groupId = groupId;
					animTime->actorId = animData->actor->refID;
					animTime->animData = animData;
					if (const auto animCtx = LoadCustomAnimation(*savedAnims, groupId, animData))
						animTime->anim = animCtx->anim;
					return &animTime;
				};
				SavedAnimsTime* animsTime = nullptr;
				if (ctx->conditionScript)
				{
					if (ctx->pollCondition)
						animsTime = initAnimTime(&*ctx)->get(); // init'd here so conditions can activate despite not being overridden
					NVSEArrayVarInterface::Element result;
#if _DEBUG
					ctx->decompiledScriptText = DecompileScript(**ctx->conditionScript);
#endif
					if (!CallFunction(**ctx->conditionScript, actor, nullptr, &result) || result.GetNumber() == 0.0)
						continue;
				}
				if (!ctx->anims.empty())
				{
					return AnimationResult(ctx, animsTime, &animStacks);
				}
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
		auto* anim = animCtx->anim;
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
	DebugPrint(FormatString("Failed to load animation for group %X", groupId));
	return nullptr;
}

// clear this cache every frame
AnimationResultCache g_animationResultCache;
ICriticalSection g_getActorAnimationCS;

std::optional<AnimationResult> GetActorAnimation(UInt32 animGroupId, AnimData* animData)
{
	ScopedLock lock(g_getActorAnimationCS);
	if (!animData || !animData->actor || !animData->actor->baseProcess)
		return std::nullopt;
	auto [cache, isNew] = g_animationResultCache.emplace(std::make_pair(animGroupId, animData), std::nullopt);
	if (!isNew)
		return cache->second;
#if _DEBUG
	int _debug = 0;
#endif

	const auto getActorAnimation = [&](UInt32 animGroupId) -> std::optional<AnimationResult>
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
		TESNPC* npc; TESRace* race;
		if (((npc = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC))) && ((race = npc->race.race)) && (result = getFormAnimation(race)))
			return result;
		// creature
		TESCreature* creature;
		if ((creature = static_cast<TESCreature*>(actor->GetActorBase())) && IS_ID(creature, Creature) && (result = getFormAnimation(creature)))
			return result;

		// equipped TODO

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
	if (const auto lResult = getActorAnimation(animGroupId & 0xFF); lResult && lResult->parent->matchBaseGroupId)
		result = lResult;
	else
		result = getActorAnimation(animGroupId);
	cache->second = result;
	return result;
}

MemoizedMap<const char*, int> s_animGroupNameToIDMap;

std::span<const char*> s_moveTypeNames{ reinterpret_cast<const char**>(0x1197798), 3 };
std::span<const char*> s_handTypeNames{ reinterpret_cast<const char**>(0x11977A8), 11 };

std::string GetBaseAnimGroupName(const std::string& name)
{
	std::string oName = name;
	if (StartsWith(oName.c_str(), "pa"))
		oName = oName.substr(2);
	for (auto* moveTypeName : s_moveTypeNames)
	{
		if (StartsWith(oName.c_str(), moveTypeName))
		{
			oName = oName.substr(strlen(moveTypeName));
			break;
		}
	}
	if (StartsWith(oName.c_str(), "mt"))
		oName = oName.substr(2);
	else
	{
		for (auto* handTypeName : s_handTypeNames)
		{
			if (StartsWith(oName.c_str(), handTypeName))
			{
				oName = oName.substr(strlen(handTypeName));
				break;
			}
		}
	}

	return oName;
}

int SimpleGroupNameToId(const char* name)
{
	return s_animGroupNameToIDMap.Get(name, [](const char* name)
	{
		std::string alt;
		if (auto* underscorePos = strchr(name, '_'))
		{
			alt = std::string(name, underscorePos - name);
			name = alt.c_str();
		}
		if (auto* spacePos = strchr(name, ' '))
		{
			alt = std::string(name, spacePos - name);
			name = alt.c_str();
		}
		const auto iter = ra::find_if(g_animGroupInfos, _L(TESAnimGroup::AnimGroupInfo & i, _stricmp(i.name, name) == 0));
		if (iter == g_animGroupInfos.end())
			return -1;
		return iter - g_animGroupInfos.begin();
	});
}

int GroupNameToId(const std::string& name)
{
	int moveType = 0;
	int animHandType = 0;
	int isPowerArmor = 0;
	CdeclCall(0x5F38D0, ('\\' + name).c_str(), &moveType, &animHandType, &isPowerArmor); // GetMoveHandAndPowerArmorTypeFromAnimName
	const auto& baseName = GetBaseAnimGroupName(name);
	const auto groupId = SimpleGroupNameToId(baseName.c_str());
	if (groupId == -1)
		return -1;
	return groupId + (moveType << 12) + (isPowerArmor << 15) + (animHandType << 8);
}

bool TESAnimGroup::IsLoopingReloadStart() const
{
	switch (this->GetBaseGroupID())
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

int GetAnimGroupId(const std::filesystem::path& path)
{
	const auto toFullId = [&](UInt8 groupId)
	{
		int moveType = 0;
		int animHandType = 0;
		int isPowerArmor = 0;
		CdeclCall(0x5F38D0, path.string().c_str(), &moveType, &animHandType, &isPowerArmor); // GetMoveHandAndPowerArmorTypeFromAnimName
		return groupId + (moveType << 12) + (isPowerArmor << 15) + (animHandType << 8);
	};

	const auto& name = path.stem().string();
	const auto& baseName = GetBaseAnimGroupName(name);
	if (const auto id = SimpleGroupNameToId(baseName.c_str()); id != -1)
		return toFullId(id);
	char bsStream[1492];
	auto pathStr = FormatString("Meshes\\%s", path.string().c_str());
	std::ranges::replace(pathStr, '/', '\\');

	auto* file = GameFuncs::GetFilePtr(pathStr.c_str(), 0, -1, 1);
	if (file)
	{
		GameFuncs::BSStream_Init(bsStream);
		if (!GameFuncs::BSStream_SetFileAndName(bsStream, pathStr.c_str(), file))
			throw std::exception(FormatString("Failure parsing file data of '%s'", pathStr.c_str()).c_str());
		NiRefObject* ref;
		ThisStdCall(0x633C90, &ref, 0); // NiRefObject__NiRefObject
		CdeclCall(0xA35700, bsStream, 0, &ref); // Read from file
		auto* anim = static_cast<NiControllerSequence*>(ref);
		const auto groupId = SimpleGroupNameToId(anim->sequenceName);
		if (groupId == -1)
			return -1;
		//ref->Destructor(true);
		GameFuncs::BSStream_Clear(bsStream);
		return toFullId(groupId);
	}
	return -1;
}

std::unordered_map<std::string, std::unordered_set<std::string>> g_customAnimGroupPaths;

std::string GetAnimBasePath(const std::string& path)
{
	for (auto& it : {"_1stperson", "_male"})
	{
		auto basePath = ExtractUntilStringMatches(path, it, true);
		if (!basePath.empty())
			return basePath;
	}
	return "";
}

std::string ExtractCustomAnimGroupName(const std::filesystem::path& path)
{
	const auto stem = path.stem().string();
	const auto pos = stem.find("__");
	if (pos != std::string::npos && pos + 2 < stem.size()) {
		return stem.substr(pos + 2);
	}
	return "";
}

bool RegisterCustomAnimGroupAnim(const std::filesystem::path& path)
{
	if (!ExtractCustomAnimGroupName(path).empty())
	{
		const auto basePath = GetAnimBasePath(path.string());
		if (!basePath.empty())
		{
			g_customAnimGroupPaths[basePath].insert(path.string());
			return true;
		}
	}
	return false;
}

void SetOverrideAnimation(const UInt32 refId,
	const std::filesystem::path& fPath,
	AnimOverrideMap& map,
	bool enable, std::unordered_set<UInt16>& groupIdFillSet,
	Script* conditionScript = nullptr,
	bool pollCondition = false,
	bool matchBaseGroupId = false)
{
	if (!conditionScript && g_jsonContext.script)
	{
		conditionScript = g_jsonContext.script;
		pollCondition = g_jsonContext.pollCondition;
	}
	if (g_jsonContext.matchBaseGroupId)
		matchBaseGroupId = true;
	const auto& path = fPath.string();
	const auto groupId = GetAnimGroupId(fPath);
	if (groupId == -1)
		throw std::exception(FormatString("Failed to resolve file '%s'", path.c_str()).c_str());
	auto& animGroupMap = map[refId];
	auto& stacks = animGroupMap.stacks[groupId];

	// condition based animations
	auto animCustom = AnimCustom::None;
	if (FindStringCI(path, R"(\human\)"))
		animCustom = AnimCustom::Human;
	else if (FindStringCI(path, R"(\male\)"))
		animCustom = AnimCustom::Male;
	else if (FindStringCI(path, R"(\female\)"))
		animCustom = AnimCustom::Female;
	else if (FindStringCI(path, R"(\hurt\)"))
		animCustom = AnimCustom::Hurt;
	else if (FindStringCI(path, R"(\mod1\)"))
		animCustom = AnimCustom::Mod1;
	else if (FindStringCI(path, R"(\mod2\)"))
		animCustom = AnimCustom::Mod2;
	else if (FindStringCI(path, R"(\mod3\)"))
		animCustom = AnimCustom::Mod3;

	auto& stack = stacks.GetCustom(animCustom);
	const auto findFn = [&](const std::shared_ptr<SavedAnims>& a)
	{
		return ra::any_of(a->anims, _L(const auto& s, _stricmp(path.c_str(), s->path.c_str()) == 0));
	};
	
	if (!enable)
	{
		// remove from stack
		if (const auto it = ra::find_if(stack, findFn); it != stack.end())
		{
			std::erase_if(g_timeTrackedGroups, _L(auto& it2, it2.first.first == &**it));
			stack.erase(it);
		}
		return;
	}
	// check if stack already contains path
	if (const auto iter = std::ranges::find_if(stack, findFn); iter != stack.end())
	{
		auto& anims = **iter;
		anims.matchBaseGroupId = matchBaseGroupId;
		if (conditionScript) // in case of hot reload
		{
			anims.conditionScript = conditionScript;
			anims.pollCondition = pollCondition;
#if _DEBUG
			anims.decompiledScriptText = DecompileScript(conditionScript);
#endif
		}
		// move iter to the top of stack
		std::rotate(iter, std::next(iter), stack.end());
		return;
	}

	const auto isCustomAnimGroupAnim = RegisterCustomAnimGroupAnim(fPath);
	if (isCustomAnimGroupAnim)
	{
		Log("Found custom anim group animation");
		// we do not want to add custom anim group anims to the stack or otherwise they'll play
		return;
	}

	// if not inserted before, treat as variant; else add to stack as separate set
	auto [_, newItem] = groupIdFillSet.emplace(groupId);
	if (newItem || stack.empty())
		stack.emplace_back(std::make_shared<SavedAnims>());

	const auto& fileName = fPath.filename().string();

	auto& anims = *stack.back();

	Log(FormatString("AnimGroup %X for form %X will be overridden with animation %s\n", groupId, refId, path.c_str()));
	auto& lastAnim = *anims.anims.emplace_back(std::make_unique<AnimPath>(path));
	anims.matchBaseGroupId = matchBaseGroupId;
	if (conditionScript)
	{
		Log("Got a condition script, this animation will now only fire under this condition!");
		anims.conditionScript = conditionScript;
		anims.pollCondition = pollCondition;
#if _DEBUG
		anims.decompiledScriptText = DecompileScript(conditionScript);
#endif
	}
	if (FindStringCI(fileName, "_order_"))
	{
		anims.hasOrder = true;
		// sort alphabetically
		std::ranges::sort(anims.anims, [&](const auto& a, const auto& b) {return a->path < b->path; });
		Log("Detected _order_ in filename; animation variants for this anim group will be played sequentially");
	}
	if (FindStringCI(fileName, "_partial"))
	{
		lastAnim.partialReload = true;
		Log("Partial reload detected");
	}
}

void OverrideFormAnimation(const TESForm* form, const std::filesystem::path& path, bool firstPerson,
	bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript, bool pollCondition, bool matchBaseGroupId)
{
	if (!form)
		return OverrideModIndexAnimation(0xFF, path, firstPerson, enable, groupIdFillSet, conditionScript, pollCondition, matchBaseGroupId);
	auto& map = GetMap(firstPerson);
	SetOverrideAnimation(form ? form->refID : -1, path, map, enable, groupIdFillSet, conditionScript, pollCondition, matchBaseGroupId);
}

void OverrideModIndexAnimation(const UInt8 modIdx, const std::filesystem::path& path, bool firstPerson,
	bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript, bool pollCondition, bool matchBaseGroupId)
{
	auto& map = GetModIndexMap(firstPerson);
	SetOverrideAnimation(modIdx, path, map, enable, groupIdFillSet, conditionScript, pollCondition, matchBaseGroupId);
}

//Export to pNVSE
__declspec(dllexport) void Exported_OverrideFormAnimation(const TESForm* form, const char* path, bool firstPerson, bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript, bool pollCondition) {
	OverrideFormAnimation(form, path, firstPerson, enable, groupIdFillSet, conditionScript, pollCondition);
}

bool CopyAnimationsToForm(TESForm* fromForm, TESForm* toForm) {
	if (fromForm->refID == toForm->refID) {
		return false;
	}
	bool result = false;
	for (auto* map : { &g_animGroupFirstPersonMap, &g_animGroupThirdPersonMap })
	{
		if (const auto iter = map->find(fromForm->refID); iter != map->end())
		{
			auto& entry = iter->second;
			for (auto& stacks : entry.stacks | std::views::values)
			{
				for (auto i = static_cast<int>(AnimCustom::None); i < static_cast<int>(AnimCustom::Max) + 1; ++i)
				{
					auto& stack = stacks.GetCustom(static_cast<AnimCustom>(i));
					for (auto& anims : stack)
					{
						std::unordered_set<UInt16> variants;
						for (auto& anim : anims->anims)
						{
							Script* condition = nullptr;
							if (anims->conditionScript)
								condition = **anims->conditionScript;
							SetOverrideAnimation(toForm->refID, anim->path, *map, false, variants, condition, anims->pollCondition);
							SetOverrideAnimation(toForm->refID, anim->path, *map, true, variants, condition, anims->pollCondition);
							result = true;
						}
					}
				}
			}
		}
	}
	return result;
}

bool RemoveFormAnimations(TESForm* form) {

	bool result = false;
	for (auto* map : { &g_animGroupFirstPersonMap, &g_animGroupThirdPersonMap })
	{
		if (const auto iter = map->find(form->refID); iter != map->end())
		{
			map->erase(iter);
			result = true;
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

bool IsAnimGroupReload(UInt8 animGroupId)
{
	return animGroupId >= kAnimGroup_ReloadWStart && animGroupId <= kAnimGroup_ReloadZ;
}

bool IsAnimGroupMovement(UInt8 animGroupId)
{
	return GetSequenceType(animGroupId) == kSequence_Movement;
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
	Log(FormatString("Script %s %X from mod %s has called %s", scriptObj->GetName(), scriptObj->refID, GetModName(scriptObj), funcName.c_str()));
	if (form)
		Log(FormatString("\ton form %s %X", form->GetName(), form->refID));
	else
		Log("\tGLOBALLY on any form");
}

template <typename F>
void OverrideAnimsFromScript(const char* path, const bool enable, F&& overrideAnim)
{
	const auto folderPath = FormatString("Data\\Meshes\\%s", path);
	if (std::filesystem::is_directory(folderPath))
	{
		for (std::filesystem::recursive_directory_iterator iter(folderPath), end; iter != end; ++iter)
		{
			if (iter->is_directory())
				continue;
			const auto& extension = iter->path().extension().string();
			if (_stricmp(extension.c_str(), ".kf") != 0)
				continue;
			constexpr size_t len = sizeof "Data\\Meshes\\";		
			overrideAnim(iter->path().string().c_str() + (len-1));
		}
	}
	else
	{
		overrideAnim(path);
	}
}

bool Cmd_SetWeaponAnimationPath_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* weaponForm = nullptr;
	auto firstPerson = 0;
	auto enable = 0;
	char path[0x1000];
	if (!ExtractArgs(EXTRACT_ARGS, &weaponForm, &firstPerson, &enable, &path))
		return true;
	if (!weaponForm)
		return true;
	auto* weapon = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
	if (!weapon)
		return true;
	try
	{
		std::unordered_set<UInt16> animGroupVariantSet;
		LogScript(scriptObj, weapon, "SetWeaponAnimationPath");
		const auto overrideAnim = [&](const std::string& animPath)
		{
			OverrideFormAnimation(weapon, animPath, firstPerson, enable, animGroupVariantSet);
		};
		OverrideAnimsFromScript(path, enable, overrideAnim);
		*result = 1;
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
			DebugPrint("ERROR: SetActorAnimationPath received an invalid reference as actor");
			return true;
		}
	}
	else if (firstPerson) // first person means it can only be the player
		actor = g_thePlayer;

	LogScript(scriptObj, actor, "SetActorAnimationPath");
	
	if (conditionScript && !IS_ID(conditionScript, Script))
	{
		DebugPrint("ERROR: SetActorAnimationPath received an invalid script/lambda as parameter.");
		conditionScript = nullptr;
	}
	try
	{
		std::unordered_set<UInt16> animGroupVariantSet;
		OverrideAnimsFromScript(path, enable, [&](const char* animPath)
		{
			OverrideFormAnimation(actor, animPath, firstPerson, enable, animGroupVariantSet,
				conditionScript, pollCondition, matchBaseGroupId);
		});
		*result = 1;
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
	g_customMaps.clear();
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

	builder.Add("sequenceName", anim->sequenceName);
	builder.Add("seqWeight", anim->seqWeight);
	builder.Add("cycleType", anim->cycleType);
	builder.Add("frequency", anim->frequency);
	builder.Add("beginKeyTime", anim->beginKeyTime);
	builder.Add("endKeyTime", anim->endKeyTime);
	if (includeDynamic)
	{
		builder.Add("lastTime", anim->lastTime);
		builder.Add("weightedLastTime", anim->weightedLastTime);
		builder.Add("lastScaledTime", anim->lastScaledTime);
		builder.Add("state", anim->state);
		builder.Add("offset", anim->offset);
		builder.Add("startTime", anim->startTime);
		builder.Add("endTime", anim->endTime);
		if (animData)
		{
			builder.Add("calculatedTime", GetAnimTime(animData, anim));
			if (anim->animGroup)
			{
				builder.Add("multiplier", GetAnimMult(animData, anim->animGroup->groupID));

			}
		}
	}
	builder.Add("accumRootName", anim->accumRootName);
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

	const auto* textKeyData = anim->textKeyData;
	if (textKeyData)
	{
		NVSEArrayBuilder textKeyTimeBuilder;
		NVSEArrayBuilder textKeyValueBuilder;

		for (int i = 0; i < textKeyData->m_uiNumKeys; ++i)
		{
			auto& key = textKeyData->m_pKeys[i];
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

BSAnimGroupSequence* FindActiveAnimationByPath(AnimData* animData, const std::string& path, const UInt32 groupId)
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
				auto* anim = animCtx->anim;
				customAnim->parent->linkedSequences.insert(anim);
				return anim;
			}
			return nullptr;
		}
	}
	if (auto* sequence = animData->controllerManager->m_kSequenceMap.Lookup(path.c_str()))
	{
		return static_cast<BSAnimGroupSequence*>(sequence);
	}
	return nullptr;
}

BSAnimGroupSequence* FindActiveAnimationForActor(Actor* actor, BSAnimGroupSequence* baseAnim)
{
	auto* animData = actor->baseProcess->GetAnimData();
	const auto* path = baseAnim->sequenceName;
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

struct SettingT
{
	union Info
	{
		unsigned int uint;
		int i;
		float f;
		char* str;
		bool b;
		UInt16 us;
	};

	virtual ~SettingT();
	Info uValue;
	const char* pKey;
};

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

NiControllerSequence::ControlledBlock* FindAnimInterp(BSAnimGroupSequence* anim, const char* interpName)
{
	std::span idTags(anim->IDTagArray, anim->numControlledBlocks);
	std::span interps(anim->controlledBlocks, anim->numControlledBlocks);
	const auto iter = ra::find_if(idTags, [&](const auto& idTag)
	{
		return _stricmp(idTag.m_kAVObjectName.CStr(), interpName) == 0;
	});
	if (iter == idTags.end())
		return nullptr;
	const auto index = std::distance(idTags.begin(), iter);
	return &interps[index];
}

NiControllerSequence::ControlledBlock* FindAnimInterp(TESObjectREFR* thisObj, const char* animPath, const char* interpName)
{
	auto* anim = FindActiveAnimationForRef(thisObj, animPath);
	if (!anim)
		return nullptr;
	return FindAnimInterp(anim, interpName);
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
					arr.Add(anim->sequenceName);
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
		ParamInfo{"anim group id", kParamType_Integer, false},
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
				builder.Add(anim->sequenceName);
				*result = reinterpret_cast<UInt32>(builder.Build(g_arrayVarInterface, scriptObj));
				return true;
			}
			const auto* multi = static_cast<AnimSequenceMultiple*>(base);
			multi->anims->ForEach([&](BSAnimGroupSequence* sequence)
			{
				builder.Add(sequence->sequenceName);
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
		const auto* kfModel = GetKFModel(path);
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
			DebugPrint("SetAnimationTextKeys: animation not found");
			return true;
		}
		if (anim->state != kAnimState_Inactive)
		{
			DebugPrint("SetAnimationKeys: you can not call this function while the animation is playing.");
			return true;
		}

		const auto textKeyTimesVector= NVSEArrayToVector<double>(g_arrayVarInterface, textKeyTimesArray);
		const auto textKeyValuesVector = NVSEArrayToVector<std::string>(g_arrayVarInterface, textKeyValuesArray);

		if (textKeyTimesVector.size() != textKeyValuesVector.size())
		{
			DebugPrint("SetAnimationTextKeys: text key times and values arrays must be the same size");
			return true;
		}

		std::vector<NiTextKey> textKeyVector;
		for (auto i = 0; i < textKeyTimesVector.size(); ++i)
		{
			const auto key = textKeyTimesVector.at(i);
			const auto& value = textKeyValuesVector.at(i);
			auto* newStr = GameFuncs::BSFixedString_CreateFromPool(value.c_str());
			textKeyVector.emplace_back(key, NiFixedString(newStr));
		}

		const auto textKeySize = textKeyVector.size() * sizeof(NiTextKey);
		auto* textKeys = AllocateNiArray<NiTextKey>(textKeyVector.size());
		memcpy_s(textKeys, textKeySize, textKeyVector.data(), textKeySize);

		auto* textKeyData = CdeclCall<NiTextKeyExtraData*>(0xA46B70); // NiTextKeyExtraData::Create

		auto* oldTextKeyData = anim->textKeyData;

		textKeyData->m_uiNumKeys = textKeyVector.size();
		textKeyData->m_pKeys = textKeys;

		++oldTextKeyData->m_uiRefCount;
		GameFuncs::NiRefObject_Replace(&anim->textKeyData, textKeyData);

		auto* oldAnimGroup = anim->animGroup;

		const auto* animGroupInfo = oldAnimGroup->GetGroupInfo();

		anim->animGroup = nullptr;
		auto* oldSequenceName = anim->sequenceName;
		anim->sequenceName = animGroupInfo->name; // yes, the game initially has the sequence name set to the anim group name
		auto* animGroup = GameFuncs::InitAnimGroup(anim, kfModel->path);
		anim->sequenceName = oldSequenceName;

		if (!animGroup)
		{
			DebugPrint("SetAnimationTextKeys: Game failed to parse text key data, see falloutnv_error.log");
			GameFuncs::NiRefObject_Replace(&anim->textKeyData, oldTextKeyData);
			GameFuncs::NiRefObject_Replace(&anim->animGroup, oldAnimGroup);
			*result = 0;
			return true;
		}


		GameFuncs::NiRefObject_Replace(&anim->animGroup, animGroup);
		GameFuncs::NiRefObject_DecRefCount_FreeIfZero(oldAnimGroup); // required since the replace above will replace null
		GameFuncs::NiRefObject_DecRefCount_FreeIfZero(oldTextKeyData);
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
		if (actor == g_thePlayer && anim->owner == g_thePlayer->firstPersonAnimData->controllerManager)
			animData = g_thePlayer->firstPersonAnimData;
		else
			animData = actor->baseProcess->GetAnimData();
		if (anim->state != kAnimState_Animating)
		{
			*result = 0;
			return true;
		}

		anim->offset = time - animData->timePassed;
		const auto animTime = GetAnimTime(animData, anim);
		if (animTime < anim->beginKeyTime)
		{
			anim->offset = anim->endKeyTime - (anim->beginKeyTime - anim->offset);
		}
		else if (animTime > anim->endKeyTime)
		{
			anim->offset = anim->beginKeyTime + (anim->offset - anim->endKeyTime);
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
					for (auto i = static_cast<int>(AnimCustom::None); i < static_cast<int>(AnimCustom::Max) + 1; ++i)
					{
						auto& stack = stacks.GetCustom(static_cast<AnimCustom>(i));
						for (auto& anims : stack)
						{
							std::unordered_set<UInt16> variants;
							for (auto& anim : anims->anims)
							{
								Script* condition = nullptr;
								if (anims->conditionScript)
									condition = **anims->conditionScript;
								SetOverrideAnimation(toForm->refID, anim->path, *map, false, variants, condition, anims->pollCondition);
								SetOverrideAnimation(toForm->refID, anim->path, *map, true, variants, condition, anims->pollCondition);
								*result = 1;
							}
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
		if (anim->state != kAnimState_Inactive && anim->state != kAnimState_EaseOut) 
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
		char sequencePath[0x400]{};
		int firstPerson = -1;
		int priority = 0;
		int startOver = 1;
		float weight = FLT_MIN;
		float easeInTime = 0.0f;
		char timeSyncSequence[0x400]{};
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
			weight = anim->seqWeight;
		BSAnimGroupSequence* timeSyncSeq = nullptr;
		if (timeSyncSequence[0])
		{
			timeSyncSeq = FindOrLoadAnim(actor, timeSyncSequence, firstPerson);
			if (!timeSyncSeq)
				return true;
		}
		auto* manager = anim->owner;

		if (anim->state != kAnimState_Inactive)
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
		char sequencePath[0x400]{};
		float easeOutTime = 0.0f;
		if (!ExtractArgs(EXTRACT_ARGS, &sequencePath, &easeOutTime))
			return true;
		auto* actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
		if (!actor)
			return true;
		auto* anim = FindActiveAnimationForActor(actor, sequencePath);
		if (!anim)
			return true;
		auto* manager = anim->owner;
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
		char sourceSequence[0x400]{};
		char destSequence[0x400]{};
		char timeSyncSequence[0x400]{};
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
			weight = destAnim->seqWeight;

		if (easeInTime == FLT_MIN)
			easeInTime = GetDefaultBlendTime(destAnim, sourceAnim);

		auto* manager = sourceAnim->owner;

		if (destAnim->state != kAnimState_Inactive)
			GameFuncs::DeactivateSequence(destAnim->owner, destAnim, 0.0f);

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
		char sequencePath[0x400]{};
		char timeSyncSequence[0x400]{};
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
		auto* manager = anim->owner;
		if (anim->state != kAnimState_Inactive)
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
		char sourceSequence[0x400]{};
		char destSequence[0x400]{};
		char timeSyncSequence[0x400]{};
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
			sourceWeight = sourceAnim->seqWeight;
		if (destWeight == FLT_MIN)
			destWeight = destAnim->seqWeight;
		if (destAnim->state != kAnimState_Inactive)
			GameFuncs::DeactivateSequence(destAnim->owner, destAnim, 0.0f);
		*result = GameFuncs::StartBlend(sourceAnim, destAnim, duration, destFrame, priority, sourceWeight, destWeight, timeSyncAnim);
		return true;
	});

	builder.Create("SetAnimWeight", kRetnType_Default, { ParamInfo{"anim sequence path", kParamType_String, false }, ParamInfo{"weight", kParamType_Float, false} }, false, [](COMMAND_ARGS)
	{
		*result = 0;
		char sequencePath[0x400]{};
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
		anim->seqWeight = weight;
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
		AnimData* animData;
		if (actor == g_thePlayer && anim->owner == g_thePlayer->firstPersonAnimData->controllerManager)
			animData = g_thePlayer->firstPersonAnimData;
		else
			animData = actor->baseProcess->GetAnimData();
		*result = GetAnimTime(animData, anim);
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
		if (animData->controllerManager != anim->owner)
		{
			if (actor != g_thePlayer || anim->owner != g_thePlayer->firstPersonAnimData->controllerManager)
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
		auto* textKeyData = anim->textKeyData;
		if (!textKeyData)
			return true;
		auto* newStr = GameFuncs::BSFixedString_CreateFromPool(textKey);

		std::span _v(textKeyData->m_pKeys, textKeyData->m_uiNumKeys);
		std::vector<NiTextKey> textKeys;
		
		textKeys.assign(_v.begin(), _v.end());
		textKeys.emplace_back(time, NiFixedString(newStr));

		const auto textKeySize = textKeys.size() * sizeof(NiTextKey);
		auto* newTextKeys = AllocateNiArray<NiTextKey>(textKeys.size());

		FormHeap_Free(textKeyData->m_pKeys);
		memcpy_s(newTextKeys, textKeySize, textKeys.data(), textKeySize);

		textKeyData->m_uiNumKeys = textKeys.size();
		textKeyData->m_pKeys = newTextKeys;
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
		auto* textKeyData = anim->textKeyData;
		if (!textKeyData)
			return true;
		std::vector<NiTextKey> textKeys;
		std::span _v(textKeyData->m_pKeys, textKeyData->m_uiNumKeys);
		textKeys.assign(_v.begin(), _v.end());
		if (index == -1)
			index = textKeys.size() - 1;
		if (index >= textKeys.size())
			return true;
		textKeys.erase(textKeys.begin() + index);
		const auto textKeySize = textKeys.size() * sizeof(NiTextKey);
		auto* newTextKeys = AllocateNiArray<NiTextKey>(textKeys.size());

		FormHeap_Free(textKeyData->m_pKeys);
		memcpy_s(newTextKeys, textKeySize, textKeys.data(), textKeySize);

		textKeyData->m_uiNumKeys = textKeys.size();
		textKeyData->m_pKeys = newTextKeys;
		*result = 1;
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
		const std::span srcControlledBlocks(sourceAnim->controlledBlocks, sourceAnim->numControlledBlocks);
		int idx = 0;
		for (const auto& srcControlledBlock : srcControlledBlocks)
		{
			auto& tag = sourceAnim->IDTagArray[idx];
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
		if (anim->animGroup->GetBaseGroupID() == groupId && anim->state != kAnimState_Inactive)
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
		anim->destFrame = destFrame;
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
		g_stringVarInterface->Assign(PASS_COMMAND_ARGS, anim->sequenceName);
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

#if _DEBUG
	
	builder.Create("EachFrame", kRetnType_Default, {ParamInfo{"sScript", kParamType_String, false}}, false, [](COMMAND_ARGS)
	{
		if (!IsConsoleMode())
		{
			Console_Print("Do not call EachFrame from a script");
			return true;
		}
		*result = 0;
		char text[0x400]{};
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



