#include <filesystem>
#include "GameData.h"
#include "GameAPI.h"
#include "GameRTTI.h"
#include "utility.h"
#include "commands_animation.h"
#include "file_animations.h"


template <typename T>
void LoadPathsForType(const std::filesystem::path& path, T identifier, bool firstPerson)
{
	for (std::filesystem::recursive_directory_iterator iter(path), end; iter != end; ++iter)
	{
		const auto fullPath = iter->path().string();
		auto str = fullPath.substr(fullPath.find("AnimGroupOverride\\"));
		Log("Loading animation path " + str + "...");
		try
		{
			if constexpr (std::is_same<T, TESObjectWEAP*>())
				OverrideWeaponAnimation(identifier, str, firstPerson, true);
			else if constexpr (std::is_same<T, Actor*>())
				OverrideActorAnimation(identifier, str, firstPerson, true);
			else if constexpr (std::is_same<T, UInt8>())
				OverrideModIndexWeaponAnimation(identifier, str, firstPerson, true);
		}
		catch (std::exception& e)
		{
			Log(FormatString("AnimGroupOverride Error: %s", e.what()));
		}
	}
}

template <typename T>
bool LoadPathsForPOV(const std::filesystem::path& path, T identifier)
{
	auto success = false;
	for (const auto& pair : {std::make_pair("\\_male", false), std::make_pair("\\_1stperson", true)})
	{
		auto iterPath = path.string() + std::string(pair.first);
		if (std::filesystem::exists(iterPath))
		{
			LoadPathsForType(iterPath, identifier, pair.second);
			success = true;
		}
		else
			Log("Could not detect path " + iterPath);
	}
	return success;
}

void LoadModAnimPaths(const std::filesystem::path& path, const ModInfo* mod)
{
	LoadPathsForPOV(path, mod->modIndex);
	for (std::filesystem::directory_iterator iter(path), end; iter != end; ++iter)
	{
		Log("Loading form ID " + iter->path().string());
		if (iter->is_directory())
		{
			const auto& iterPath = iter->path();
			try
			{
				const auto& folderName = iterPath.filename().string();
				char* p;
				const auto id = strtoul(folderName.c_str(), &p, 16) & 0x00FFFFFF;
				if (*p == 0) 
				{
					const auto formId = id + (mod->modIndex << 24);
					auto* form = LookupFormByID(formId);
					if (form)
					{
						if (const auto* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP))
							LoadPathsForPOV(iterPath, weapon);
						else if (const auto* actor = DYNAMIC_CAST(form, TESForm, Actor))
							LoadPathsForPOV(iterPath, actor);
						else
							Log(FormatString("Unsupported form type for %X", form->refID));
					}
					else
					{
						Log(FormatString("kNVSE Animation: Form %X not found!", formId));
					}
				}
			}
			catch (std::exception&) {}
		}
		else
		{
			Log("Skipping as path is not a directory...");
		}
	}
}

void LoadFileAnimPaths()
{
	Log("Loading file anims");
	const auto dir = GetCurPath() + R"(\Data\Meshes\AnimGroupOverride)";
	if (std::filesystem::exists(dir))
	{
		for (std::filesystem::directory_iterator iter(dir.c_str()), end; iter != end; ++iter)
		{
			if (iter->is_directory())
			{
				Log(iter->path().string() + " found");
				const auto& path = iter->path();
				const auto* mod = DataHandler::Get()->LookupModByName(path.filename().string().c_str());
				if (mod)
				{
					LoadModAnimPaths(path, mod);
				}
				else
				{
					Log(FormatString("%s is not a mod!", iter->path().string().c_str()));
				}
			}
		}
	}
	else
	{
		Log(dir + " does not exist.");
	}
}
