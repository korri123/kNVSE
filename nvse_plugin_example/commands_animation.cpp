#include "commands_animation.h"

#include <algorithm>
#include <unordered_set>

#include "GameForms.h"
#include "GameAPI.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "utility.h"
#include "common/IDirectoryIterator.h"



// Per ref ID there are animation variants per group ID
using AnimOverrideMap = std::unordered_map<UInt32, std::unordered_map<UInt32, Anims>>;
AnimOverrideMap g_refWeaponAnimGroupThirdPersonMap;
AnimOverrideMap g_refWeaponAnimGroupFirstPersonMap;

AnimOverrideMap g_refActorAnimGroupThirdPersonMap;
AnimOverrideMap g_refActorAnimGroupFirstPersonMap;

AnimOverrideMap g_refModIdxWeaponThirdPersonMap;
AnimOverrideMap g_refModIdxWeaponFirstPersonMap;

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
	//SafeWrite8(0x497FA7 + 1, 1);
	GameFuncs::PlayIdle(actor->GetAnimData(), idle, actor, idle->data.groupFlags & 0x3F, 3);
	//SafeWrite8(0x497FA7 + 1, 0);
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
	result.push_back(kfFilePath);
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

KFModel* LoadAnimation(const std::string& path, AnimData* animData)
{
	auto* kfModel = GameFuncs::LoadKFModel(*g_modelLoader, path.c_str());
	if (kfModel && kfModel->animGroup && animData)
	{
		kfModel->animGroup->groupID = 0xF5; // use a free anim group slot
		GameFuncs::LoadAnimation(animData, kfModel, false);
		return kfModel;
	}
	return nullptr;
}

BSAnimGroupSequence* GetAnimationFromMap(AnimOverrideMap& map, UInt32 refId, UInt32 animGroupId, AnimData* animData)
{
	const auto mapIter = map.find(refId);
	if (mapIter != map.end())
	{
		const auto animationsIter = mapIter->second.find(animGroupId);
		if (animationsIter != mapIter->second.end())
		{
			auto& animations = animationsIter->second;
			if (!animations.anims.empty())
			{
				auto& savedAnim = animations.anims.at(GetRandomUInt(animations.anims.size()));
				const auto* model = LoadAnimation(savedAnim.path, animData);
				if (model)
					return model->controllerSequence;
			}
		}
	}
	return nullptr;
}

BSAnimGroupSequence* GetWeaponAnimation(TESObjectWEAP* weapon, UInt32 animGroupId, bool firstPerson, AnimData* animData)
{
	auto& map = firstPerson ? g_refWeaponAnimGroupFirstPersonMap : g_refWeaponAnimGroupThirdPersonMap;
	if (auto* result = GetAnimationFromMap(map, weapon->refID, animGroupId, animData))
		return result;
	auto& modIdxMap = firstPerson ? g_refModIdxWeaponFirstPersonMap : g_refModIdxWeaponThirdPersonMap;
	return GetAnimationFromMap(modIdxMap, weapon->GetModIndex(), animGroupId, animData);
}

BSAnimGroupSequence* GetActorAnimation(Actor* actor, UInt32 animGroupId, bool firstPerson, AnimData* animData)
{
	auto& map = firstPerson ? g_refActorAnimGroupFirstPersonMap : g_refActorAnimGroupThirdPersonMap;
	if (auto* result = GetAnimationFromMap(map, actor->refID, animGroupId, animData))
		return result;
	if (auto* baseForm = actor->baseForm)
		return GetAnimationFromMap(map, baseForm->refID, animGroupId, animData);
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
			return -1;
		s_animGroupIds[path] = animGroupId;
	}
	return animGroupId;
}

void SetOverrideAnimation(const UInt32 refId, std::string path, AnimOverrideMap& map, bool enable)
{
	const auto groupId = GetAnimGroupId(path);
	if (groupId == -1)
		throw std::exception(FormatString("Cannot resolve file '%s'", path.c_str()).c_str());
	auto& animGroupMap = map[refId];
	auto& animations = animGroupMap[groupId];
	animations.anims.clear();
	if (enable)
	{
		auto paths = GetAnimationVariantPaths(path);
		for (auto& pathIter : paths)
		{
			if (GameFuncs::LoadKFModel(*g_modelLoader, pathIter.c_str()))
			{
				Log(FormatString("\tOverrode AnimGroup %X with path %s", groupId, pathIter.c_str()));
				std::replace(path.begin(), path.end(), '/', '\\');
				animations.anims.emplace_back(path);
			}
			else
				Log(FormatString("\tFailed to load animation variant '%s'", pathIter.c_str()));
		}
	}
	else
	{
		Log(FormatString("\tCleared animations for anim group %X", groupId));
	}
	
}

void OverrideActorAnimation(Actor* actor, const std::string& path, bool firstPerson, bool enable)
{
	auto& map = firstPerson ? g_refActorAnimGroupFirstPersonMap : g_refActorAnimGroupThirdPersonMap;
	if (firstPerson && actor != *g_thePlayer)
		throw std::exception("Cannot apply first person animations on actors other than player!");
	SetOverrideAnimation(actor->refID, path, map, enable);
}

void OverrideWeaponAnimation(TESObjectWEAP* weapon, const std::string& path, bool firstPerson, bool enable)
{
	auto& map = firstPerson ? g_refWeaponAnimGroupFirstPersonMap : g_refWeaponAnimGroupThirdPersonMap;
	SetOverrideAnimation(weapon->refID, path, map, enable);
}

void OverrideModIndexWeaponAnimation(UInt8 modIdx, const std::string& path, bool firstPerson, bool enable)
{
	auto& map = firstPerson ? g_refModIdxWeaponFirstPersonMap : g_refModIdxWeaponThirdPersonMap;
	SetOverrideAnimation(modIdx, path, map, enable);
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
		OverrideWeaponAnimation(weapon, path, firstPerson, enable);
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
	if (!ExtractArgs(EXTRACT_ARGS, &firstPerson, &enable, &path))
		return true;
	if (!thisObj)
		return true;
	auto* actor = DYNAMIC_CAST(thisObj, TESForm, Actor);
	if (!actor)
		return true;
	try
	{
		LogScript(scriptObj, actor, "SetActorAnimationPath");
		OverrideActorAnimation(actor, path, firstPerson, enable);
		*result = 1;
	}
	catch (std::exception& e)
	{
		ShowRuntimeError(scriptObj, "\tSetWeaponAnimationPath: %s", e.what());
	}
	return true;
}

// SetWeaponAnimationPath WeapNV9mmPistol 1 1 "characters\_male\idleanims\weapons\pistol_1hpAttackRight.kf"
// SetActorAnimationPath player 0 1 "characters\_male\idleanims\sprint\Haughty.kf"
// SetWeaponAnimationPath WeapNV9mmPistol 1 1 "characters\_male\idleanims\weapons\1hpEquip.kf"



