#include "Commands_Animation.h"

#include <algorithm>


#include "GameForms.h"
#include "GameAPI.h"
#include "GameObjects.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
#include "common/IDirectoryIterator.h"

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

KFModel* LoadAnimation(const char* path, AnimData* animData)
{
	char bsStream[1492] = { 0 };
	auto pathStr = FormatString("Meshes\\%s", path);
	std::replace(pathStr.begin(), pathStr.end(), '/', '\\');

	auto* file = GameFuncs::GetFilePtr(pathStr.c_str(), 0, -1, 1);
	if (file)
	{
		GameFuncs::BSStream_Init(bsStream);
		if (!ReadFile(bsStream, path, file))
		{
			throw std::exception(FormatString("Critical failure parsing file data of '%s'", path).c_str());
		}
		auto* kfModel = static_cast<KFModel*>(FormHeap_Allocate(sizeof(KFModel)));
		memset(kfModel, 0xCD, sizeof(KFModel));
		GameFuncs::KFModel_Init(kfModel, path, bsStream);
		//kfModel->animGroup->groupID = 0xF5; // first free id, make sure not to overwrite any slots
		GameFuncs::LoadAnimation(animData, kfModel, true);
		return kfModel;
	}
	return nullptr;
}

WeaponAnimMap g_weaponAnimMaps[static_cast<int>(AnimType::kAnimType_Max) - 1];

WeaponAnimMap& GetWeaponAnimMap(AnimType a)
{
	return g_weaponAnimMaps[static_cast<int>(a) - 1];
}

// Per ref ID there are animation variants per group ID
using AnimOverrideMap = std::unordered_map<UInt32, std::unordered_map<UInt32, std::vector<KFModel*>>>;
AnimOverrideMap g_refWeaponAnimGroupThirdPersonMap;
AnimOverrideMap g_refWeaponAnimGroupFirstPersonMap;

AnimOverrideMap g_refActorAnimGroupThirdPersonMap;
AnimOverrideMap g_refActorAnimGroupFirstPersonMap;

BSAnimGroupSequence* GetAnimationFromMap(AnimOverrideMap& map, UInt32 refId, UInt32 animGroupId)
{
	const auto mapIter = map.find(refId);
	if (mapIter != map.end())
	{
		const auto animationsIter = mapIter->second.find(animGroupId);
		if (animationsIter != mapIter->second.end())
		{
			const auto& animations = animationsIter->second;
			return !animations.empty() ? animations.at(GetRandomUInt(animations.size()))->controllerSequence : nullptr;
		}
	}
	return nullptr;
}


BSAnimGroupSequence* GetWeaponAnimation(UInt32 refId, UInt32 animGroupId, bool firstPerson)
{
	auto& map = firstPerson ? g_refWeaponAnimGroupFirstPersonMap : g_refWeaponAnimGroupThirdPersonMap;
	return GetAnimationFromMap(map, refId, animGroupId);
}

BSAnimGroupSequence* GetActorAnimation(UInt32 refId, UInt32 animGroupId, bool firstPerson)
{
	auto& map = firstPerson ? g_refActorAnimGroupFirstPersonMap : g_refActorAnimGroupThirdPersonMap;
	return GetAnimationFromMap(map, refId, animGroupId);
}

void SetOverrideAnimation(UInt32 refId, const std::string& path, UInt32 animGroup, AnimOverrideMap& map, AnimData* animData, bool enable)
{
	auto& animGroupMap = map[refId];
	auto& animations = animGroupMap[animGroup];
	if (enable)
	{
		auto paths = GetAnimationVariantPaths(path);
		for (auto& pathIter : paths)
		{
			auto* kfModel = LoadAnimation(pathIter.c_str(), animData);
			if (kfModel)
				animations.push_back(kfModel);
			else
				throw std::exception(FormatString("Cannot resolve file '%s'", pathIter.c_str()).c_str());
		}
	}
	else
	{
		for (auto* anim : animations)
			FormHeap_Free(anim);

		animations.clear();
		animGroupMap.erase(animGroup);
	}
}

bool Cmd_SetWeaponAnimationPath_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* weaponForm = nullptr;
	auto animGroup = 0;
	auto firstPerson = 0;
	auto enable = 0;
	char path[0x1000];
	if (!ExtractArgs(EXTRACT_ARGS, &weaponForm, &animGroup, &firstPerson, &enable, &path))
		return true;
	if (!weaponForm)
		return true;
	auto* weapon = DYNAMIC_CAST(weaponForm, TESForm, TESObjectWEAP);
	if (!weapon)
		return true;
	try
	{
		auto& map = firstPerson ? g_refWeaponAnimGroupFirstPersonMap : g_refWeaponAnimGroupThirdPersonMap;
		auto* animData = firstPerson ? (*g_thePlayer)->firstPersonAnimData : (*g_thePlayer)->baseProcess->GetAnimData();
		SetOverrideAnimation(weapon->refID, path, animGroup, map, animData, enable);
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
	TESForm* actorForm = nullptr;
	auto animGroup = 0;
	auto firstPerson = 0;
	auto enable = 0;
	char path[0x1000];
	if (!ExtractArgs(EXTRACT_ARGS, &actorForm, &animGroup, &firstPerson, &enable, &path))
		return true;
	if (!actorForm)
		return true;
	auto* actor = DYNAMIC_CAST(actorForm, TESForm, Actor);
	if (!actor)
		return true;

	if (firstPerson && actor != *g_thePlayer)
		return true;
	try
	{
		auto& map = firstPerson ? g_refActorAnimGroupFirstPersonMap : g_refActorAnimGroupThirdPersonMap;

		auto* animData = firstPerson ? (*g_thePlayer)->firstPersonAnimData : actor->baseProcess->GetAnimData();
		SetOverrideAnimation(actorForm->refID, path, animGroup, map, animData, enable);

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



