#include "Commands_Animation.h"

#include <algorithm>
#include <unordered_set>



#include "GameForms.h"
#include "GameAPI.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
#include "common/IDirectoryIterator.h"



// Per ref ID there are animation variants per group ID
using AnimOverrideMap = std::unordered_map<UInt32, std::unordered_map<UInt32, Anims>>;
AnimOverrideMap g_refWeaponAnimGroupThirdPersonMap;
AnimOverrideMap g_refWeaponAnimGroupFirstPersonMap;

AnimOverrideMap g_refActorAnimGroupThirdPersonMap;
AnimOverrideMap g_refActorAnimGroupFirstPersonMap;

AnimOverrideMap g_oldAnimations;

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


bool ReadFile(char* bsStream, const char* path, BSFile* file)
{
	__try
	{
		GameFuncs::BSStream_SetFileAndName(bsStream, path, file);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static ModelLoader** g_modelLoader = reinterpret_cast<ModelLoader**>(0x011C3B3C);

KFModel* LoadAnimation(const std::string& path, AnimData* animData, UInt32& animGroup)
{
	auto pathStr = FormatString("Meshes\\%s", path.c_str());
	std::replace(pathStr.begin(), pathStr.end(), '/', '\\');

	auto* kfModel = GameFuncs::LoadKFModel(*g_modelLoader, path.c_str());
	if (kfModel && kfModel->animGroup && animData)
	{
		animGroup = kfModel->animGroup->groupID;
		static std::unordered_set<KFModel*> s_loaded;
		if (s_loaded.find(kfModel) == s_loaded.end())
		{
			kfModel->animGroup->groupID = 0x2;
			GameFuncs::LoadAnimation(animData, kfModel, false);
			s_loaded.emplace(kfModel);
		}
		return kfModel;
	}
	return nullptr;
}

SavedAnim* GetAnimationFromMap(AnimOverrideMap& map, UInt32 refId, UInt32 animGroupId)
{
	const auto mapIter = map.find(refId);
	if (mapIter != map.end())
	{
		const auto animationsIter = mapIter->second.find(animGroupId);
		if (animationsIter != mapIter->second.end())
		{
			auto& animations = animationsIter->second;
			if (animations.enabled)
				return !animations.anims.empty() ? &animations.anims.at(GetRandomUInt(animations.anims.size())) : nullptr;
		}
	}
	return nullptr;
}

BSAnimGroupSequence* GetWeaponAnimation(UInt32 refId, UInt32 animGroupId, bool firstPerson)
{
	auto& map = firstPerson ? g_refWeaponAnimGroupFirstPersonMap : g_refWeaponAnimGroupThirdPersonMap;
	auto* result = GetAnimationFromMap(map, refId, animGroupId);
	if (result)
		return result->model->controllerSequence;
	return nullptr;
}

BSAnimGroupSequence* GetActorAnimation(UInt32 refId, UInt32 animGroupId, bool firstPerson)
{
	auto& map = firstPerson ? g_refActorAnimGroupFirstPersonMap : g_refActorAnimGroupThirdPersonMap;
	auto* result = GetAnimationFromMap(map, refId, animGroupId);
	//if (result)
	//	return result->single->anim;
	
	return nullptr;
}

SavedAnim* GetWeaponAnimationSingle(UInt32 refId, UInt32 animGroupId, bool firstPerson)
{
	auto& map = firstPerson ? g_refWeaponAnimGroupFirstPersonMap : g_refWeaponAnimGroupThirdPersonMap;
	auto* result = GetAnimationFromMap(map, refId, animGroupId);
	if (result)
		return result;
	return nullptr;
}

SavedAnim* GetActorAnimationSingle(UInt32 refId, UInt32 animGroupId, bool firstPerson)
{
	auto& map = firstPerson ? g_refActorAnimGroupFirstPersonMap : g_refActorAnimGroupThirdPersonMap;
	auto* result = GetAnimationFromMap(map, refId, animGroupId);
	if (result)
		return result;
	return nullptr;
}

void SetOverrideAnimation(const UInt32 refId, const std::string& path, AnimOverrideMap& map, AnimData* animData, bool enable)
{
	auto paths = GetAnimationVariantPaths(path);
	for (auto& pathIter : paths)
	{
		UInt32 realAnimGroupId;
		auto* kfModel = LoadAnimation(pathIter, animData, realAnimGroupId);
		if (kfModel && kfModel->animGroup)
		{
			auto* single = animData->mapAnimSequenceBase->Lookup(kfModel->animGroup->groupID);
			auto& animGroupMap = map[refId];
			auto& animations = animGroupMap[realAnimGroupId];
			if (enable)
			{
				animations.enabled = true;
				animations.anims.emplace_back(kfModel, (AnimSequenceSingle*)single, kfModel->animGroup->groupID);
			}
			else
				animations.enabled = false;
		}
		else
			throw std::exception(FormatString("Cannot resolve file '%s'", pathIter.c_str()).c_str());
	}
}

void OverrideActorAnimation(Actor* actor, const std::string& path, bool firstPerson)
{
	auto& map = firstPerson ? g_refActorAnimGroupFirstPersonMap : g_refActorAnimGroupThirdPersonMap;
	if (firstPerson && actor != *g_thePlayer)
		throw std::exception("Cannot apply first person animations on actors other than player!");
	auto* animData = firstPerson ? (*g_thePlayer)->firstPersonAnimData : actor->baseProcess->GetAnimData();
	SetOverrideAnimation(actor->refID, path, map, animData, true);
}

void OverrideWeaponAnimation(TESObjectWEAP* weapon, const std::string& path, bool firstPerson)
{
	auto& map = firstPerson ? g_refWeaponAnimGroupFirstPersonMap : g_refWeaponAnimGroupThirdPersonMap;
	auto* animData = firstPerson ? (*g_thePlayer)->firstPersonAnimData : (*g_thePlayer)->baseProcess->GetAnimData();
	SetOverrideAnimation(weapon->refID, path, map, animData, true);
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
		OverrideWeaponAnimation(weapon, path, firstPerson);
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
		OverrideActorAnimation(actor, path, firstPerson);
		*result = 1;
	}
	catch (std::exception& e)
	{
		ShowRuntimeError(scriptObj, "SetWeaponAnimationPath: %s", e.what());
	}
	return true;
}

// SetWeaponAnimationPath WeapNV9mmPistol 32 1 1 "characters\_male\idleanims\weapons\pistol_1hpAttackRight.kf"
// SetActorAnimationPath player 7 0 1 "characters\_male\idleanims\sprint\Haughty.kf"
// SetWeaponAnimationPath WeapNV9mmPistol 24 1 1 "characters\_male\idleanims\weapons\1hpEquip.kf"



