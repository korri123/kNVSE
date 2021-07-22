#include "commands_animation.h"

#include <algorithm>
#include <unordered_set>
#include <filesystem>
#include <span>

#include "file_animations.h"
#include "GameForms.h"
#include "GameAPI.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "hooks.h"
#include "SafeWrite.h"
#include "utility.h"
#include "common/IDirectoryIterator.h"
#include "PluginAPI.h"

// Per ref ID there is a stack of animation variants per group ID
class AnimOverrideStruct
{
public:
	std::unordered_map<UInt32, AnimStacks> stacks;
};

using AnimOverrideMap = std::unordered_map<UInt32, AnimOverrideStruct>;
AnimOverrideMap g_animGroupThirdPersonMap;
AnimOverrideMap g_animGroupFirstPersonMap;

// mod index map
AnimOverrideMap g_animGroupModIdxThirdPersonMap;
AnimOverrideMap g_animGroupModIdxFirstPersonMap;

// for those with no ref ID specified
AnimOverrideStruct g_globalAnimOverrides;

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
	for (IDirectoryIterator iter(FormatString("Data\\Meshes\\%s", folderPath.c_str()).c_str()); !iter.Done(); iter.Next())
	{
		std::string iterExtension;
		auto iterFileName = GetFileNameNoExtension(iter.GetFileName(), iterExtension);
		if (_stricmp(iterExtension.c_str(), "kf") == 0)
		{
			if (iterFileName.rfind(animName + '_', 0) == 0)
			{
				auto fullPath = iter.GetFullPath();
				result.push_back(fullPath.substr(strlen("Data\\Meshes\\")));
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

static std::unordered_set<BSAnimGroupSequence*> g_customAnims;

bool IsCustomAnim(BSAnimGroupSequence* sequence)
{
	return g_customAnims.contains(sequence);
}

BSAnimGroupSequence* LoadAnimation(const std::string& path, AnimData* animData)
{
	auto* kfModel = GameFuncs::LoadKFModel(*g_modelLoader, path.c_str());
	if (kfModel && kfModel->animGroup && animData)
	{
		//kfModel->animGroup->groupID = 0xF5; // use a free anim group slot
		const auto groupId = kfModel->animGroup->groupID;
		
		// delete an animation if it's already using up our slot
		//if (GameFuncs::LoadAnimation(animData, kfModel, false)) return kfModel->controllerSequence;
		auto* seqBase = animData->mapAnimSequenceBase->Lookup(groupId);
		if (seqBase)
		{
			auto* anim = seqBase->GetSequenceByIndex(0);
			if (anim && _stricmp(anim->sequenceName, path.c_str()) == 0)
 				return anim;
			seqBase->Destroy(true);
			GameFuncs::NiTPointerMap_RemoveKey(animData->mapAnimSequenceBase, groupId);
		}
		if (GameFuncs::LoadAnimation(animData, kfModel, false))
		{
			if (auto* base = animData->mapAnimSequenceBase->Lookup(groupId))
			{
				BSAnimGroupSequence* anim;
				if (base && ((anim = base->GetSequenceByIndex(0))))
					return anim;
				DebugPrint("Map returned null anim");
			}
			else
			{
				DebugPrint("Failed to lookup anim");
			}
		}
	}
	DebugPrint("Failed to load KF Model " + path);
	return nullptr;
}

std::unordered_map<AnimData*, GameAnimMap*> g_customMaps;

BSAnimGroupSequence* LoadCustomAnimation(const std::string& path, AnimData* animData)
{
	auto*& customMap = g_customMaps[animData];
	if (!customMap)
		customMap = CreateGameAnimMap();
	auto* defaultMap = animData->mapAnimSequenceBase;
	animData->mapAnimSequenceBase = customMap;
	auto* result = LoadAnimation(path, animData);
	animData->mapAnimSequenceBase = defaultMap;
	return result;
}

extern NVSEScriptInterface* g_script;

// Make sure that Aim, AimUp and AimDown all use the same index
std::string* HandleAimUpDownRandomness(UInt32 animGroupId, AnimList& anims)
{
	UInt32 baseId;
	if (const auto animGroupMinor = animGroupId & 0xFF; animGroupMinor >= kAnimGroup_Aim && animGroupMinor <= kAnimGroup_AimISDown && (baseId = kAnimGroup_Aim)
		|| animGroupMinor >= kAnimGroup_AttackLeft && animGroupMinor <= kAnimGroup_AttackSpin2ISDown && (baseId = kAnimGroup_AttackLeft) 
		|| animGroupMinor >= kAnimGroup_PlaceMine && animGroupMinor <= kAnimGroup_AttackThrow8ISDown && (baseId = kAnimGroup_PlaceMine))
	{
		static unsigned int s_lastRandomId = 0;
		if ((animGroupMinor - baseId) % 3 == 0 || s_lastRandomId >= anims.size())
			s_lastRandomId = GetRandomUInt(anims.size());

		return &anims.at(s_lastRandomId);
	}
	return nullptr;
}

std::list<BurstFireData> g_burstFireQueue;
std::map<BSAnimGroupSequence*, AnimTime> g_firstPersonAnimTimes;

void HandleExtraOperations(AnimData* animData, BSAnimGroupSequence* sequence)
{
	std::span textKeys{ sequence->textKeyData->m_pKeys, sequence->textKeyData->m_uiNumKeys };

	if (sequence->animGroup->IsAttack() && std::ranges::any_of(textKeys, [](NiTextKey& key) {return _stricmp(key.m_kText.data, "burstFire") == 0;}))
	{
		std::vector<NiTextKey*> hitKeys;
		bool skippedFirst = false;
		for (auto& key : textKeys)
		{
			if (_stricmp(key.m_kText.data, "hit") == 0)
			{
				if (!skippedFirst)
				{
					// engine handles first key
					skippedFirst = true;
					continue;
				}
				hitKeys.push_back(&key);
			}
		}
		if (!hitKeys.empty())
		{
			g_burstFireQueue.emplace_back(animData, sequence, 0, std::move(hitKeys), 0.0);
		}
	}
	if (animData == g_thePlayer->firstPersonAnimData && std::ranges::any_of(textKeys, [](NiTextKey& key) {return _stricmp(key.m_kText.data, "respectEndKey") == 0;}))
		g_firstPersonAnimTimes[sequence] = AnimTime();
	if (std::ranges::any_of(textKeys, [](NiTextKey& key) {return _stricmp(key.m_kText.data, "interruptLoop") == 0; }))
	{
		*reinterpret_cast<UInt8*>(&g_animationHookContext.GroupID()) = kAnimGroup_AttackLoopIS;
		if (animData == g_thePlayer->firstPersonAnimData)
		{
			g_thePlayer->baseProcess->GetAnimData()->groupIDs[kSequence_Weapon] = kAnimGroup_AttackLoopIS;
		}
	}
}

BSAnimGroupSequence* PickAnimation(AnimOverrideStruct& overrides, UInt32 animGroupId, AnimData* animData, const char* prevPath = nullptr)
{
	if (const auto stacksIter = overrides.stacks.find(animGroupId); stacksIter != overrides.stacks.end())
	{
		auto* actor = animData->actor;
		auto animCustom = AnimCustom::None;
		std::vector<SavedAnims>* stack = nullptr;
		auto exclusiveAnim = false;
		if (prevPath)
		{
			if (FindStringCI(prevPath, R"(\male\)"))
				animCustom = AnimCustom::Male;
			else if (FindStringCI(prevPath, R"(\female\)"))
				animCustom = AnimCustom::Female;
			else if (FindStringCI(prevPath, R"(\hurt\)"))
			{
				exclusiveAnim = true; // don't want hurt anim fall-backing to normal animation
				animCustom = AnimCustom::Hurt;
			}
		}
		if (auto* weaponInfo = actor->baseProcess->GetWeaponInfo(); weaponInfo && weaponInfo->GetExtraData())
		{
			auto* xData = weaponInfo->GetExtraData();
			auto* modFlags = static_cast<ExtraWeaponModFlags*>(xData->GetByType(kExtraData_WeaponModFlags));
			if (modFlags && modFlags->flags)
			{
				if (modFlags->flags & 1 && !stacksIter->second.mod1Anims.empty())
					animCustom = AnimCustom::Mod1;
				if (modFlags->flags & 2 && !stacksIter->second.mod2Anims.empty())
					animCustom = AnimCustom::Mod2;
				if (modFlags->flags & 4 && !stacksIter->second.mod3Anims.empty())
					animCustom = AnimCustom::Mod3;
			}
		}
		if (animCustom != AnimCustom::None)
			stack = &stacksIter->second.GetCustom(animCustom);
		if (!stack || stack->empty() && !exclusiveAnim)
			stack = &stacksIter->second.GetCustom(AnimCustom::None);

		if (!stack->empty())
		{
			auto& ctx = stack->back();
			auto& [order, anims, conditionScript] = ctx;
			if (conditionScript)
			{
				NVSEArrayVarInterface::Element result;
				if (!g_script->CallFunction(conditionScript, actor, nullptr, &result, 1) || result.Number() == 0.0)
					return nullptr;
			}
			if (!anims.empty())
			{
				std::string* savedAnimPath;
				if (order == -1)
				{
					// Make sure that Aim, AimUp and AimDown all use the same index
					if (auto* path = HandleAimUpDownRandomness(animGroupId, anims))
						savedAnimPath = path;
					else
						// pick random variant
						savedAnimPath = &anims.at(GetRandomUInt(anims.size()));
				}
				else
				{
					// ordered
					savedAnimPath = &anims.at(order++ % anims.size());
				}

				if (auto* anim = LoadCustomAnimation(*savedAnimPath, animData); anim)
				{
					HandleExtraOperations(animData, anim);
					return anim;
				}
			}
		}
	}
	return nullptr;
}

BSAnimGroupSequence* GetAnimationFromMap(AnimOverrideMap& map, UInt32 id, UInt32 animGroupId, AnimData* animData, const char* prevPath = nullptr)
{
	if (const auto mapIter = map.find(id); mapIter != map.end())
	{
		return PickAnimation(mapIter->second, animGroupId, animData, prevPath);
	}
	return nullptr;
}

AnimOverrideMap& GetMap(bool firstPerson)
{
	return firstPerson ? g_animGroupFirstPersonMap : g_animGroupThirdPersonMap;
}

AnimOverrideMap& GetModIndexMap(bool firstPerson)
{
	return firstPerson ? g_animGroupModIdxFirstPersonMap : g_animGroupModIdxThirdPersonMap;
}

BSAnimGroupSequence* GetActorAnimation(UInt32 animGroupId, bool firstPerson, AnimData* animData, const char* prevPath)
{
	BSAnimGroupSequence* result;
	auto& map = GetMap(firstPerson);
	auto* actor = animData->actor;
	auto& modIndexMap = GetModIndexMap(firstPerson);
	// weapon form ID
	if (auto* weaponInfo = actor->baseProcess->GetWeaponInfo(); weaponInfo && weaponInfo->weapon)
	{
		auto* weapon = weaponInfo->weapon;
		if (weapon)
		{
			if ((result = GetAnimationFromMap(map, weapon->refID, animGroupId, animData, prevPath)))
				return result;
			// weapon mod index
			if ((result = GetAnimationFromMap(modIndexMap, weapon->GetModIndex(), animGroupId, animData, prevPath)))
				return result;
		}
	}
	// actor ref ID
	if ((result = GetAnimationFromMap(map, actor->refID, animGroupId, animData, prevPath)))
		return result;
	// base form ID
	if (auto* baseForm = actor->baseForm; baseForm && ((result = GetAnimationFromMap(map, baseForm->refID, animGroupId, animData, prevPath))))
		return result;
	// race form ID
	TESNPC* npc; TESRace* race;
	if (((npc = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC))) && ((race = npc->race.race)))
	{
		if ((result = GetAnimationFromMap(map, race->refID, animGroupId, animData, prevPath)))
			return result;
		// race mod index
		if ((result = GetAnimationFromMap(modIndexMap, race->GetModIndex(), animGroupId, animData, prevPath)))
			return result;
	}
	// mod index
	if ((result = GetAnimationFromMap(modIndexMap, actor->GetModIndex(), animGroupId, animData, prevPath)))
		return result;
	// non-form ID dependent animations (global replacers)
	if (!firstPerson && ((result = PickAnimation(g_globalAnimOverrides, animGroupId, animData, prevPath))))
		return result;
	return nullptr;
}

int GetAnimGroupId(const std::string& path)
{
	UInt32 animGroupId;
	static std::unordered_map<std::string, UInt16> s_animGroupIds;
	const auto iter = s_animGroupIds.find(path);
	if (iter != s_animGroupIds.end())
	{
		animGroupId = iter->second;
	}
	else
	{
		auto* kfModel = GameFuncs::LoadKFModel(*g_modelLoader, path.c_str());
		if (kfModel && kfModel->animGroup)
			animGroupId = kfModel->animGroup->groupID;
		else
		{
			if (kfModel && !kfModel->animGroup)
				Log("KF file is missing AnimGroup data!");
			return -1;
		}
		s_animGroupIds[path] = animGroupId;
	}
	return animGroupId;
}


void SetOverrideAnimation(const UInt32 refId, std::string path, AnimOverrideMap& map, bool enable, bool append, int* outGroupId = nullptr, Script* conditionScript = nullptr)
{
	std::ranges::replace(path, '/', '\\');
	const auto groupId = GetAnimGroupId(path);
	if (outGroupId)
		*outGroupId = groupId;
	if (groupId == -1)
		throw std::exception(FormatString("Failed to resolve file '%s'", path.c_str()).c_str());

	auto& animGroupMap = refId != -1 ? map[refId] : g_globalAnimOverrides;
	auto& stacks = animGroupMap.stacks[groupId];

	// condition based animations
	auto animCustom = AnimCustom::None;
	if (FindStringCI(path, R"(\male\)"))
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
		return std::ranges::any_of(a.anims, [&](const std::string& s) { return _stricmp(path.c_str(), s.c_str()) == 0; });
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
		return;
	}
	if (!append || stack.empty())
		stack.emplace_back();

	auto& anims = stack.back();
	Log(FormatString("AnimGroup %X for form %X will be overridden with animation %s\n", groupId, refId, path.c_str()));
	anims.anims.emplace_back(path);
	if (conditionScript)
	{
		Log("Got a condition script, this animation will now only fire under this condition!");
		anims.conditionScript = conditionScript;
	}

	const auto realPath = std::filesystem::path(path);
	if (FindStringCI(realPath.filename().string(), "_order_"))
	{
		anims.order = 0;
		// sort alphabetically
		std::ranges::sort(anims.anims, [&](const std::string& a, const std::string& b) {return a < b; });
		Log("Detected _order_ in filename; animation variants for this anim group will be played sequentially");
	}
}

void OverrideActorAnimation(const Actor* actor, const std::string& path, bool firstPerson, bool enable, bool append, int* outGroupId, Script* conditionScript)
{
	auto& map = GetMap(firstPerson);
	if (firstPerson && actor != g_thePlayer)
		throw std::exception("Cannot apply first person animations on actors other than player!");
	SetOverrideAnimation(actor ? actor->refID : -1, path, map, enable, append, outGroupId, conditionScript);
}

void OverrideWeaponAnimation(const TESObjectWEAP* weapon, const std::string& path, bool firstPerson, bool enable, bool append)
{
	auto& map = GetMap(firstPerson);
	SetOverrideAnimation(weapon->refID, path, map, enable, append);
}

void OverrideModIndexAnimation(const UInt8 modIdx, const std::string& path, bool firstPerson, bool enable, bool append)
{
	auto& map = GetModIndexMap(firstPerson);
	SetOverrideAnimation(modIdx, path, map, enable, append);
}

void OverrideRaceAnimation(const TESRace* race, const std::string& path, bool firstPerson, bool enable, bool append)
{
	auto& map = GetMap(firstPerson);
	SetOverrideAnimation(race->refID, path, map, enable, append);
}

float GetTimePassed()
{
	const auto isMenuMode = CdeclCall<bool>(0x702360);
	if (isMenuMode)
		return 0.0;
	return g_timeGlobal->secondsPassed * static_cast<float>(ThisStdCall<double>(0x9C8CC0, (void*)0x11F2250));
}

void LogScript(Script* scriptObj, TESForm* form, const std::string& funcName)
{
	Log(FormatString("Script %s %X from mod %s has called %s", scriptObj->GetName(), scriptObj->refID, GetModName(scriptObj), funcName.c_str()));
	if (form)
		Log(FormatString("\ton form %s %X", form->GetName(), form->refID));
	else
		Log("\tGLOBALLY on any form");
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
		LogScript(scriptObj, weapon, "SetWeaponAnimationPath");
		OverrideWeaponAnimation(weapon, path, firstPerson, enable, false);
		if (enable)
		{
			auto paths = GetAnimationVariantPaths(path);
			for (auto& pathIter : paths)
			{
				OverrideWeaponAnimation(weapon, pathIter, firstPerson, true, true);
			}
		}
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
	auto firstPerson = false;
	auto enable = 0;
	char path[0x1000];
	int playImmediately = 0;
	Script* conditionScript;
	if (!ExtractArgs(EXTRACT_ARGS, &firstPerson, &enable, &path, &playImmediately, &conditionScript))
		return true;
	Actor* actor = nullptr;
	if (thisObj)
	{
		actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
		if (!actor)
		{
			Log("ERROR: SetActorAnimationPath received an invalid reference as actor");
			return true;
		}
	}
	else if (firstPerson) // first person means it can only be the player
		actor = g_thePlayer;

	LogScript(scriptObj, actor, "SetActorAnimationPath");
	
	if (conditionScript && !IS_ID(conditionScript, Script))
	{
		Log("ERROR: SetActorAnimationPath received an invalid script/lambda as parameter.");
		conditionScript = nullptr;
	}
	try
	{
		int groupId;
		OverrideActorAnimation(actor, path, firstPerson, enable, false, &groupId, conditionScript);
		if (enable)
		{
			auto paths = GetAnimationVariantPaths(path);
			for (auto& pathIter : paths)
			{
				OverrideActorAnimation(actor, pathIter, firstPerson, true, true, nullptr, conditionScript);
			}
			if (actor && playImmediately)
			{
				if (auto* animData = actor->GetAnimData())
				{
					GameFuncs::PlayAnimGroup(animData, groupId, 1, 0, -1);
				}
			}
		}
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
	auto firstPerson = -1;
	auto type = -1;
	
	if (!ExtractArgs(EXTRACT_ARGS, &path, &type, &firstPerson))
		return true;

	if (type < 0)
		return true;

	if (thisObj != g_thePlayer && firstPerson)
	{
		Log("Can't play first person animation on an actor that's not the player");
		return true;
	}

	auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
	if (!actor)
		return true;

	auto* animData = actor->baseProcess->GetAnimData();
	if (firstPerson)
		animData = g_thePlayer->firstPersonAnimData;

	auto* kfModel = GameFuncs::LoadKFModel(*g_modelLoader, path);

	if (!kfModel)
		return true;

	if (!GameFuncs::LoadAnimation(animData, kfModel, false))
		return true;
	
	const auto animGroup = 0xFE;
	GameFuncs::MorphToSequence(animData, kfModel->controllerSequence, animGroup, type);
	
	*result = 1;
	return true;
}

bool Cmd_kNVSEReset_Execute(COMMAND_ARGS)
{
	g_animGroupFirstPersonMap.clear();
	g_animGroupThirdPersonMap.clear();
	g_globalAnimOverrides = AnimOverrideStruct();
	LoadFileAnimPaths();
	return true;
}


//bool Cmd_GetPlayingAnimationPath_Execute(COMMAND_ARGS)
//{
//	int type
//}


// SetWeaponAnimationPath WeapNV9mmPistol 1 1 "characters\_male\idleanims\weapons\1hpAttackRight.kf"
// SetWeaponAnimationPath WeapNVHuntingShotgun 1 1 "characters\_male\idleanims\weapons\2hrAttack7.kf"
// SetActorAnimationPath 0 1 "characters\_male\idleanims\sprint\Haughty.kf"
// SetWeaponAnimationPath WeapNV9mmPistol 1 1 "characters\_male\idleanims\weapons\pistol.kf"



