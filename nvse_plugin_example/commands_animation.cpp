#include "commands_animation.h"

#include <algorithm>
#include <unordered_set>
#include <filesystem>
#include "GameForms.h"
#include "GameAPI.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
#include "utility.h"
#include "common/IDirectoryIterator.h"

// Per ref ID there is a stack of animation variants per group ID
class AnimOverrideStruct
{
public:
	std::unordered_map<UInt32, AnimStacks> stacks;
	GameAnimMap* map = nullptr;

	~AnimOverrideStruct()
	{
		if (map)
		{
			map->~NiTPointerMap<AnimSequenceBase>();
			FormHeap_Free(map);
		}
	}
};

using AnimOverrideMap = std::unordered_map<UInt32, AnimOverrideStruct>;
AnimOverrideMap g_animGroupThirdPersonMap;
AnimOverrideMap g_animGroupFirstPersonMap;

bool Cmd_ForcePlayIdle_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;
	auto* actor = reinterpret_cast<Actor*>(thisObj);
	if (!ExtractArgs(EXTRACT_ARGS, &form) || !actor->IsActor() || !actor->baseProcess)
		return true;
	auto* idle = DYNAMIC_CAST(form, TESForm, TESIdleForm);
	if (!idle)
		return true;
	SafeWrite8(0x497FA7 + 1, 1);
	GameFuncs::PlayIdle(actor->GetAnimData(), idle, actor, idle->data.groupFlags & 0x3F, 3);
	SafeWrite8(0x497FA7 + 1, 0);
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
			{
				return anim;
			}
			seqBase->Destroy(true);
			GameFuncs::NiTPointerMap_RemoveKey(animData->mapAnimSequenceBase, groupId);
		}
		if (GameFuncs::LoadAnimation(animData, kfModel, false))
		{
			if (auto* base = animData->mapAnimSequenceBase->Lookup(groupId))
			{
				BSAnimGroupSequence* seq;
				if (base && ((seq = base->GetSequenceByIndex(0))))
				{
					return seq;
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
	return nullptr;
}

BSAnimGroupSequence* LoadCustomAnimation(const std::string& path, AnimData* animData, GameAnimMap*& customMap)
{
	if (!customMap)
	{
		customMap = CreateGameAnimMap();
	}
	auto* defaultMap = animData->mapAnimSequenceBase;
	animData->mapAnimSequenceBase = customMap;
	auto* result = LoadAnimation(path, animData);
	animData->mapAnimSequenceBase = defaultMap;
	return result;
}

BSAnimGroupSequence* GetAnimationFromMap(AnimOverrideMap& map, UInt32 id, UInt32 animGroupId, AnimData* animData, const char* prevPath = nullptr)
{
	const auto mapIter = map.find(id);
	if (mapIter != map.end())
	{
		const auto stacksIter = mapIter->second.stacks.find(animGroupId);
		if (stacksIter != mapIter->second.stacks.end())
		{
			auto animCustom = AnimCustom::None;
			std::vector<SavedAnims>* stack = nullptr;
			bool exclusiveAnim = false;
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
			if (auto* weaponInfo = animData->actor->baseProcess->GetWeaponInfo(); weaponInfo && weaponInfo->GetExtraData())
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
				auto& anims = stack->back();
				if (!anims.anims.empty())
				{
					std::string* savedAnim;
					if (anims.order == -1)
					{
						// pick random variant
						const auto rand = GetRandomUInt(anims.anims.size());
						savedAnim = &anims.anims.at(rand);
					}
					else
					{
						// ordered
						savedAnim = &anims.anims.at(anims.order++ % anims.anims.size());
					}

					auto* anim = LoadCustomAnimation(*savedAnim, animData, mapIter->second.map);
					if (anim)
						return anim;
				}
			}
		}
	}
	return nullptr;
}

AnimOverrideMap& GetMap(bool firstPerson)
{
	return firstPerson ? g_animGroupFirstPersonMap : g_animGroupThirdPersonMap;
}

BSAnimGroupSequence* GetWeaponAnimation(TESObjectWEAP* weapon, UInt32 animGroupId, bool firstPerson, AnimData* animData)
{
	auto& map = GetMap(firstPerson);
	if (auto* result = GetAnimationFromMap(map, weapon->refID, animGroupId, animData))
		return result;
	return GetAnimationFromMap(map, weapon->GetModIndex(), animGroupId, animData);
}

BSAnimGroupSequence* GetActorAnimation(Actor* actor, UInt32 animGroupId, bool firstPerson, AnimData* animData, const char* prevPath)
{
	auto& map = GetMap(firstPerson);
	if (auto* result = GetAnimationFromMap(map, actor->refID, animGroupId, animData, prevPath))
		return result;
	if (auto* baseForm = actor->baseForm)
		return GetAnimationFromMap(map, baseForm->refID, animGroupId, animData, prevPath);
	TESNPC* npc = nullptr; TESRace* race = nullptr;
	if (((npc = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC))) && ((race = npc->race.race)))
		return GetAnimationFromMap(map, race->refID, animGroupId, animData, prevPath);
	return GetAnimationFromMap(map, actor->GetModIndex(), animGroupId, animData, prevPath);
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


void SetOverrideAnimation(const UInt32 refId, std::string path, AnimOverrideMap& map, bool enable, bool append, int* outGroupId = nullptr)
{
	std::replace(path.begin(), path.end(), '/', '\\');
	const auto groupId = GetAnimGroupId(path);
	if (outGroupId)
		*outGroupId = groupId;
	if (groupId == -1)
		throw std::exception(FormatString("Failed to resolve file '%s'", path.c_str()).c_str());
	auto& animGroupMap = map[refId];
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
		for (const auto& s : a.anims)
			if (_stricmp(path.c_str(), s.c_str()) == 0)
				return true;
		return false;
	};
	
	if (!enable)
	{
		// remove from stack
		const auto iter = std::remove_if(stack.begin(), stack.end(), findFn);
		stack.erase(iter, stack.end());
		return;
	}
	// check if stack already contains path
	if (const auto iter = std::find_if(stack.begin(), stack.end(), findFn); iter != stack.end())
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

	const auto realPath = std::filesystem::path(path);
	if (FindStringCI(realPath.filename().string(), "_order_"))
	{
		anims.order = 0;
		// sort alphabetically
		std::sort(anims.anims.begin(), anims.anims.end(), [&](const std::string& a, const std::string& b) {return a < b; });
		Log("Detected _order_ in file name; animation variants for this anim group will be played sequentially");
	}
}

void OverrideActorAnimation(const Actor* actor, const std::string& path, bool firstPerson, bool enable, bool append, int* outGroupId)
{
	auto& map = GetMap(firstPerson);
	if (firstPerson && actor != *g_thePlayer)
		throw std::exception("Cannot apply first person animations on actors other than player!");
	SetOverrideAnimation(actor->refID, path, map, enable, append, outGroupId);
}

void OverrideWeaponAnimation(const TESObjectWEAP* weapon, const std::string& path, bool firstPerson, bool enable, bool append)
{
	auto& map = GetMap(firstPerson);
	SetOverrideAnimation(weapon->refID, path, map, enable, append);
}

void OverrideModIndexAnimation(const UInt8 modIdx, const std::string& path, bool firstPerson, bool enable, bool append)
{
	auto& map = GetMap(firstPerson);
	SetOverrideAnimation(modIdx, path, map, enable, append);
}

void OverrideRaceAnimation(const TESRace* race, const std::string& path, bool firstPerson, bool enable, bool append)
{
	auto& map = GetMap(firstPerson);
	SetOverrideAnimation(race->refID, path, map, enable, append);
}

void LogScript(Script* scriptObj, TESForm* form, const std::string& funcName)
{
	Log(FormatString("Script %s %X from mod %s has called %s on form %s %X", scriptObj->GetName(), scriptObj->refID, GetModName(scriptObj), funcName.c_str(), form->GetName(), form->refID));
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
	auto firstPerson = 0;
	auto enable = 0;
	char path[0x1000];
	int playImmediately = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &firstPerson, &enable, &path, &playImmediately))
		return true;
	if (!thisObj)
		return true;
	auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
	if (!actor)
		return true;
	try
	{
		LogScript(scriptObj, actor, "SetActorAnimationPath");
		int groupId;
		OverrideActorAnimation(actor, path, firstPerson, enable, false, &groupId);
		if (enable)
		{
			auto paths = GetAnimationVariantPaths(path);
			for (auto& pathIter : paths)
			{
				OverrideActorAnimation(actor, pathIter, firstPerson, true, true);
			}
			if (playImmediately)
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

	if (thisObj != *g_thePlayer && firstPerson)
	{
		Log("Can't play first person animation on an actor that's not the player");
		return true;
	}

	auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
	if (!actor)
		return true;

	auto* animData = actor->baseProcess->GetAnimData();
	if (firstPerson)
		animData = (*g_thePlayer)->firstPersonAnimData;

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

//bool Cmd_GetPlayingAnimationPath_Execute(COMMAND_ARGS)
//{
//	int type
//}


// SetWeaponAnimationPath WeapNV9mmPistol 1 1 "characters\_male\idleanims\weapons\1hpAttackRight.kf"
// SetWeaponAnimationPath WeapNVHuntingShotgun 1 1 "characters\_male\idleanims\weapons\2hrAttack7.kf"
// SetActorAnimationPath 0 1 "characters\_male\idleanims\sprint\Haughty.kf"
// SetWeaponAnimationPath WeapNV9mmPistol 1 1 "characters\_male\idleanims\weapons\pistol.kf"



