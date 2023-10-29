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
#include "NiNodes.h"
#include "NiTypes.h"


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

BSAnimGroupSequence* GetAnimationByPath(const char* path)
{
	const auto* kfModel = GameFuncs::LoadKFModel(*g_modelLoader, path);
	return kfModel ? kfModel->controllerSequence : nullptr;
}

struct RefCountHack
{
	BSAnimGroupSequence* sequence;
	UInt32 oldRefCount;

	explicit RefCountHack(BSAnimGroupSequence* sequence)
		: sequence(sequence)
	{
		this->oldRefCount = sequence->m_uiRefCount;
		sequence->m_uiRefCount = 1;
	}

	~RefCountHack()
	{
		this->sequence->m_uiRefCount = oldRefCount + (this->sequence->m_uiRefCount - 1);
	}
};

std::map<std::pair<std::string, AnimData*>, BSAnimationContext> g_cachedAnimMap;

void HandleOnSequenceDestroy(BSAnimGroupSequence* anim)
{
	g_timeTrackedAnims.erase(anim);
	std::erase_if(g_timeTrackedGroups, _L(auto& iter, iter.second.anim == anim));
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

			if (anim->state == NiControllerSequence::kAnimState_Inactive)
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
}

std::optional<BSAnimationContext> LoadAnimation(const std::string& path, AnimData* animData)
{
	const auto key = std::make_pair(path, animData);
	if (const auto iter = g_cachedAnimMap.find(key); iter != g_cachedAnimMap.end())
	{
		const auto ctx = iter->second;
		return ctx;
	}
	auto* kfModel = GameFuncs::LoadKFModel(*g_modelLoader, path.c_str());
	if (kfModel && kfModel->animGroup && animData)
	{
		
		//kfModel->animGroup->groupID = 0xF5; // use a free anim group slot
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
					// anim->destFrame = kfModel->controllerSequence->destFrame;
					const auto& [entry, success] = g_cachedAnimMap.emplace(key, BSAnimationContext(anim, base));
					return entry->second;
				}
				DebugPrint("Map returned null anim");
			}
			else
			{
				DebugPrint("Failed to lookup anim");
			}
		}
	}
	DebugPrint("Failed to load KF Model " + path);
	return std::nullopt;
}

AnimTime::~AnimTime()
{
	Revert3rdPersonAnimTimes(*this, this->anim);
}

std::optional<BSAnimationContext> LoadCustomAnimation(const std::string& path, AnimData* animData)
{
	auto*& customMap = g_customMaps[animData];
	if (!customMap)
		customMap = CreateGameAnimMap();
	auto* defaultMap = animData->mapAnimSequenceBase;
	animData->mapAnimSequenceBase = customMap;
	auto result = LoadAnimation(path, animData);
	animData->mapAnimSequenceBase = defaultMap;
	return result;
}

extern NVSEScriptInterface* g_script;

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

		return &anims.anims.at(s_lastRandomId);
	}
	return nullptr;
}

std::list<BurstFireData> g_burstFireQueue;

std::map<BSAnimGroupSequence*, std::shared_ptr<AnimTime>> g_timeTrackedAnims;


std::map<ActorSequenceKey, SavedAnimsTime> g_timeTrackedGroups;

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

bool HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* anim, AnimPath& ctx)
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
	const auto hasKey = [&](const std::initializer_list<const char*> keyTexts, AnimKeySetting& setting, KeyCheckType type = KeyCheckType::KeyEquals)
	{
		if (setting == AnimKeySetting::Set)
		{
			applied = true;
			return true;
		}
		if (setting == AnimKeySetting::NotSet)
			return false;
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
		setting = result ? AnimKeySetting::Set : AnimKeySetting::NotSet;
		if (result)
			applied = true;
		
		return result;
	};

	if (anim->animGroup->IsAttack() && hasKey({"burstFire"}, ctx.hasBurstFire))
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
	if (animData == g_thePlayer->firstPersonAnimData && hasKey({"respectEndKey", "respectTextKeys"}, ctx.hasRespectEndKey))
	{
		auto& animTime = getAnimTimeStruct();
		animTime.povState = POVSwitchState::NotSet;
		animTime.respectEndKey = true;
		animTime.finishedEndKey = false;
		animTime.actorWeapon = actor->GetWeaponForm();
	}
	const auto baseGroupID = anim->animGroup->GetBaseGroupID();

	if (hasKey({"interruptLoop"}, ctx.hasInterruptLoop) && (baseGroupID == kAnimGroup_AttackLoop || baseGroupID == kAnimGroup_AttackLoopIS))
	{
		// IS allowed so that anims can finish after releasing LMB (handled in hook)
		*reinterpret_cast<UInt8*>(g_animationHookContext.groupID) = kAnimGroup_AttackLoopIS;
		if (animData == g_thePlayer->firstPersonAnimData)
		{
			g_thePlayer->baseProcess->GetAnimData()->groupIDs[kSequence_Weapon] = kAnimGroup_AttackLoopIS;
		}
		g_lastLoopSequence = anim;
		g_startedAnimation = true;
	}
	if (hasKey({"noBlend"}, ctx.hasNoBlend))
	{
		g_animationHookContext.animData->noBlend120 = true;
	}
	if (hasKey({"Script:"}, ctx.hasCallScript, KeyCheckType::KeyStartsWith))
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
	if (hasKey({"SoundPath:"}, ctx.hasSoundPath, KeyCheckType::KeyStartsWith))
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
	if (hasKey({"blendToReloadLoop"}, ctx.hasBlendToReloadLoop))
	{
		g_reloadStartBlendFixes.insert(anim->sequenceName);
	}
	if (hasKey({"scriptLine:"}, ctx.hasScriptLine, KeyCheckType::KeyStartsWith))
	{
		auto [scriptLineKeys, uninitialized] = createTimedExecution(g_scriptLineExecutions);
		if (uninitialized)
		{
			*scriptLineKeys = TimedExecution<Script*>(textKeys, [&](const char* key, Script*& result)
			{
				if (!StartsWith(key, "scriptLine:"))
					return false;
				auto line = GetTextAfterColon(key);
				if (line.empty())
					return false;
				FormatScriptText(line);
				result = Script::CompileFromText(line, "ScriptLineKey");
				if (!result)
				{
					DebugPrint("Failed to compile script in scriptLine key: " + line);
					return false;
				}
				return true;
			});
		}
		auto& animTime = getAnimTimeStruct();
		animTime.scriptLines = scriptLineKeys->CreateContext();
	}
	if (hasKey({"replaceWithGroup:"}, ctx.hasReplaceWithGroup, KeyCheckType::KeyStartsWith))
	{
		const auto keyText = ra::find_if(textKeys, _L(NiTextKey &key, StartsWith(key.m_kText.CStr(), "replaceWithGroup:")))->m_kText.CStr();
		const auto& line = GetTextAfterColon(keyText);
		const auto newGroupId = GroupNameToId(line);
		if (newGroupId != -1)
		{
			*g_animationHookContext.groupID = newGroupId;
			if (*g_animationHookContext.sequenceId != -1)
			{
				*g_animationHookContext.sequenceId = static_cast<eAnimSequence>(GetSequenceType(newGroupId));
			}
			anim->animGroup->groupID = newGroupId;
		}
	}
	if (hasKey({"allowAttack"}, ctx.hasAllowAttack))
	{
		const auto iter = ra::find_if(textKeys, _L(NiTextKey &key, _stricmp(key.m_kText.CStr(), "allowAttack") == 0));
		if (iter != textKeys.end())
		{
			auto& animTime = getAnimTimeStruct();
			animTime.allowAttackTime = iter->m_fTime;
		}
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
	StackVector<AnimPath*, 0x100> reloads;
	if ((ammoInfo->count == 0 || DidActorReload(actor, ReloadSubscriber::Partial) || g_partialLoopReloadState == PartialLoopingReloadState::NotPartial) 
		&& g_partialLoopReloadState != PartialLoopingReloadState::Partial)
	{
		reloads = Filter<0x100, AnimPath>(ctx.anims, _L(AnimPath & p, !p.partialReload));
		if (IsLoopingReload(groupId) && g_partialLoopReloadState == PartialLoopingReloadState::NotSet)
			g_partialLoopReloadState = PartialLoopingReloadState::NotPartial;
	}
	else
	{
		reloads = Filter<0x100, AnimPath>(ctx.anims, _L(AnimPath & p, p.partialReload));
		if (IsLoopingReload(groupId) && g_partialLoopReloadState == PartialLoopingReloadState::NotSet)
			g_partialLoopReloadState = PartialLoopingReloadState::Partial;
	}
	if (reloads->empty())
		return nullptr;
	if (!ctx.hasOrder)
		return reloads->at(GetRandomUInt(reloads->size()));
	return reloads->at(g_actorAnimOrderMap[std::make_pair(actor->refID, &ctx)]++ % reloads->size());
}

std::optional<AnimationResult> PickAnimation(AnimOverrideStruct& overrides, UInt16 groupId, AnimData* animData)
{
	if (const auto stacksIter = overrides.stacks.find(groupId); stacksIter != overrides.stacks.end())
	{
		auto& animStacks = stacksIter->second;
		auto* actor = animData->actor;
		auto* groupInfo = GetGroupInfo(groupId);

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
					const bool firstPerson = animData == g_thePlayer->firstPersonAnimData;
					const auto key = ActorSequenceKey(actor->refID, groupInfo->sequenceType, firstPerson);
					auto& animTime = g_timeTrackedGroups[key];
					animTime.conditionScript = **savedAnims->conditionScript;
					animTime.firstPerson = animData == g_thePlayer->firstPersonAnimData;
					animTime.groupId = groupId;
					animTime.actorId = animData->actor->refID;
					animTime.lastNiTime = -FLT_MAX;
					animTime.realGroupId = GetActorRealAnimGroup(animData->actor, groupId);
					return &animTime;
				};
				SavedAnimsTime* animsTime = nullptr;
				if (ctx.conditionScript)
				{
					if (ctx.pollCondition)
						animsTime = initAnimTime(&ctx); // init'd here so conditions can activate despite not being overridden
					NVSEArrayVarInterface::Element result;
					if (!g_script->CallFunction(**ctx.conditionScript, actor, nullptr, &result, 0) || result.Number() == 0.0)
						continue;
				}
				if (!ctx.anims.empty())
				{
					return AnimationResult(&ctx, animsTime);
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

AnimPath* GetAnimPath(const AnimationResult& animResult, UInt16 groupId, AnimData* animData)
{
	auto& ctx = *animResult.parent;
	AnimPath* savedAnimPath = nullptr;
	Actor* actor = animData->actor;

	if (IsAnimGroupReload(groupId) && ra::any_of(ctx.anims, _L(AnimPath & p, p.partialReload)))
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
			savedAnimPath = &ctx.anims.at(GetRandomUInt(ctx.anims.size()));
	}
	else
	{
		// ordered
		savedAnimPath = &ctx.anims.at(g_actorAnimOrderMap[std::make_pair(actor->refID, &ctx)]++ % ctx.anims.size());
	}
	return savedAnimPath;
}

void ClearSameSequenceTypeGroups(const BSAnimGroupSequence* anim, SavedAnimsTime* animsTime)
{
	const auto* groupInfo = GetGroupInfo(anim->animGroup->groupID);
	std::erase_if(g_timeTrackedGroups, [&](auto& p)
	{
		SavedAnimsTime& iterAnimsTime = p.second;
		if (&iterAnimsTime == animsTime)
			return false;
		const auto* iterInfo = GetGroupInfo(iterAnimsTime.groupId);
		return animsTime->actorId == iterAnimsTime.actorId && animsTime->firstPerson == iterAnimsTime.firstPerson && iterInfo->sequenceType == groupInfo->sequenceType;
	});
}

BSAnimGroupSequence* LoadAnimationPath(const AnimationResult& result, AnimData* animData, UInt16 groupId)
{
	auto& [ctx, animsTime] = result;
	auto* animPath = GetAnimPath(result, groupId, animData);
	if (!animPath)
	{
		DebugPrint(FormatString("Failed to find replacement anim for group %X", groupId));
		return nullptr;
	}
	if (const auto animCtx = LoadCustomAnimation(animPath->path, animData))
	{
		auto* anim = animCtx->anim;
		SubscribeOnActorReload(animData->actor, ReloadSubscriber::Partial);
		HandleExtraOperations(animData, anim, *animPath);
		if (ctx->conditionScript && animsTime)
		{
			animsTime->anim = anim;
			// ClearSameSequenceTypeGroups(anim, result.animsTime); // should no longer be needed since g_timeTrackedGroups is keyed on sequence type
		}
		return anim;
	}
	DebugPrint(FormatString("Game failed to load animation for group %X", groupId));
	return nullptr;
}

std::optional<AnimationResult> GetActorAnimation(UInt32 animGroupId, bool firstPerson, AnimData* animData)
{
	if (!animData || !animData->actor)
		return std::nullopt;
	std::optional<AnimationResult> result;
	std::optional<AnimationResult> modIndexResult;

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
}

bool Cmd_GetActorAnimation_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 animGroupId = -1;
	UInt32 firstPerson = -1;
	if (!ExtractArgs(EXTRACT_ARGS, &animGroupId, &firstPerson) || !thisObj || animGroupId == -1)
		return true;
	const auto actor = DYNAMIC_CAST(thisObj, TESObjectREFR, Actor);
	if (!actor)
		return true;
	if (firstPerson == -1)
	{
		firstPerson = actor == g_thePlayer && !g_thePlayer->IsThirdPerson();
	}
	const auto realGroupId = GetActorRealAnimGroup(actor, animGroupId);
	const auto animData = firstPerson ? g_thePlayer->firstPersonAnimData : actor->GetAnimData();
	if (!animData)
		return true;
	const auto animResult = GetActorAnimation(realGroupId, firstPerson, animData);
	if (animResult && animResult->parent && !animResult->parent->anims.empty())
	{
		const auto* path = animResult->parent->anims.front().path.c_str();
		*result = g_stringVarInterface->CreateString(path, scriptObj);
		return true;
	}
	auto* vanillaAnimBase = animData->mapAnimSequenceBase->Lookup(realGroupId);
	if (vanillaAnimBase)
	{
		const auto* anim = vanillaAnimBase->GetSequenceByIndex(-1);
		if (anim)
		{
			const auto* path = anim->sequenceName;
			*result = g_stringVarInterface->CreateString(path, scriptObj);
			return true;
		}
	}
	return true;
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

void SetOverrideAnimation(const UInt32 refId, const std::filesystem::path& fPath, AnimOverrideMap& map, bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript = nullptr, bool pollCondition = false)
{
	if (!conditionScript && g_jsonContext.script)
	{
		conditionScript = g_jsonContext.script;
		pollCondition = g_jsonContext.pollCondition;
	}
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
	const auto findFn = [&](const SavedAnims& a)
	{
		return ra::any_of(a.anims, _L(const auto& s, _stricmp(path.c_str(), s.path.c_str()) == 0));
	};
	
	if (!enable)
	{
		// remove from stack
		const auto iter = std::ranges::remove_if(stack, findFn).begin();
		stack.erase(iter, stack.end());
		return;
	}
	// check if stack already contains path
	if (const auto iter = std::ranges::find_if(stack, findFn); iter != stack.end())
	{
		// move iter to the top of stack
		std::rotate(iter, iter + 1, stack.end());
		if (conditionScript) // in case of hot reload
		{
			iter->conditionScript = conditionScript;
			iter->pollCondition = pollCondition;
		}
		return;
	}
	// if not inserted before, treat as variant; else add to stack as separate set
	auto [_, newItem] = groupIdFillSet.emplace(groupId);
	if (newItem || stack.empty())
		stack.emplace_back();

	const auto& fileName = fPath.filename().string();

	auto& anims = stack.back();

	Log(FormatString("AnimGroup %X for form %X will be overridden with animation %s\n", groupId, refId, path.c_str()));
	auto& lastAnim = anims.anims.emplace_back(path);
	if (conditionScript)
	{
		Log("Got a condition script, this animation will now only fire under this condition!");
		anims.conditionScript = conditionScript;
		anims.pollCondition = pollCondition;
	}
	if (FindStringCI(fileName, "_order_"))
	{
		anims.hasOrder = true;
		// sort alphabetically
		std::ranges::sort(anims.anims, [&](const auto& a, const auto& b) {return a.path < b.path; });
		Log("Detected _order_ in filename; animation variants for this anim group will be played sequentially");
	}
	if (FindStringCI(fileName, "_partial"))
	{
		lastAnim.partialReload = true;
		Log("Partial reload detected");
	}
}

void OverrideFormAnimation(const TESForm* form, const std::filesystem::path& path, bool firstPerson, bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript, bool pollCondition)
{
	if (!form)
		return OverrideModIndexAnimation(0xFF, path, firstPerson, enable, groupIdFillSet, conditionScript, pollCondition);
	auto& map = GetMap(firstPerson);
	SetOverrideAnimation(form ? form->refID : -1, path, map, enable, groupIdFillSet, conditionScript, pollCondition);
}

void OverrideModIndexAnimation(const UInt8 modIdx, const std::filesystem::path& path, bool firstPerson, bool enable, std::unordered_set<UInt16>& groupIdFillSet, Script* conditionScript, bool pollCondition)
{
	auto& map = GetModIndexMap(firstPerson);
	SetOverrideAnimation(modIdx, path, map, enable, groupIdFillSet, conditionScript, pollCondition);
}

float GetAnimMult(AnimData* animData, UInt8 animGroupID)
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
	auto* ammoInfo = actor->baseProcess->GetAmmoInfo();
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
	if (!ExtractArgs(EXTRACT_ARGS, &firstPerson, &enable, &path, &pollCondition, &conditionScript))
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
		const auto overrideAnim = [&](const char* animPath)
		{
			OverrideFormAnimation(actor, animPath, firstPerson, enable, animGroupVariantSet, conditionScript, pollCondition);
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

	if (const auto anim = LoadCustomAnimation(path, animData))
	//if (const auto anim = GetAnimationByPath(path))
	{
		g_doNotSwapAnims = true;
		GameFuncs::MorphToSequence(animData, anim->anim, anim->anim->animGroup->groupID, -1);
		g_doNotSwapAnims = false;
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

enum class AnimationTrait
{
	None = -1, StartTime = 0
};

bool Cmd_SetAnimationTraitNumeric_Execute(COMMAND_ARGS)
{

	char path[0x400];
	auto trait = AnimationTrait::None;
	float value = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &path, &trait, &value) || trait == AnimationTrait::None)
		return true;
	auto* anim = GetAnimationByPath(path);
	if (!anim)
		return true;
	switch (trait) {
	case AnimationTrait::StartTime:
	{
		anim->SetStartOffset(value);
		*result = 1;
		break;
	}
	default: break;
	}
	return true;
}

bool Cmd_GetAnimationTraitNumeric_Execute(COMMAND_ARGS)
{

	char path[0x400];
	auto trait = AnimationTrait::None;
	if (!ExtractArgs(EXTRACT_ARGS, &path, &trait) || trait == AnimationTrait::None)
		return true;
	auto* anim = GetAnimationByPath(path);
	if (!anim)
		return true;
	switch (trait) {
	case AnimationTrait::StartTime:
	{
		*result = anim->lastTime;
		break;
	}
	default: break;
	}
	return true;
}

NVSEArrayVarInterface::Array* CreateAnimationObjectArray(BSAnimGroupSequence* anim, Script* callingScript)
{
	NVSEStringMapBuilder builder;

	builder.Add("sequenceName", anim->sequenceName);
	builder.Add("seqWeight", anim->seqWeight);
	builder.Add("cycleType", anim->cycleType);
	builder.Add("frequency", anim->frequency);
	builder.Add("beginKeyTime", anim->beginKeyTime);
	builder.Add("endKeyTime", anim->endKeyTime);
	builder.Add("lastTime", anim->lastTime);
	builder.Add("weightedLastTime", anim->weightedLastTime);
	builder.Add("lastScaledTime", anim->lastScaledTime);
	builder.Add("state", anim->state);
	builder.Add("offset", anim->offset);
	builder.Add("startTime", anim->startTime);
	builder.Add("endTime", anim->endTime);
	builder.Add("destFrame", anim->destFrame);
	builder.Add("accumRootName", anim->accumRootName);


	const auto* textKeyData = anim->textKeyData;
	if (textKeyData)
	{
		NVSEMapBuilder textKeyMapBuilder;

		for (int i = 0; i < textKeyData->m_uiNumKeys; ++i)
		{
			auto& key = textKeyData->m_pKeys[i];
			textKeyMapBuilder.Add(key.m_fTime, key.m_kText.CStr());
		}

		auto* textKeyMap = textKeyMapBuilder.Build(g_arrayVarInterface, callingScript);

		builder.Add("textKeyData", textKeyMap);
	}
	

	return builder.Build(g_arrayVarInterface, callingScript);
}

bool Cmd_GetAnimationByPath_Execute(COMMAND_ARGS)
{
	*result = 0;
	char path[0x400];
	if (!ExtractArgs(EXTRACT_ARGS, &path))
		return true;
	if (auto* anim = GetAnimationByPath(path))
		*result = reinterpret_cast<UInt32>(CreateAnimationObjectArray(anim, scriptObj));
	return true;
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



