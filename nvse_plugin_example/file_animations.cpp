#include <filesystem>
#include "GameData.h"
#include "GameAPI.h"
#include "GameRTTI.h"
#include "utility.h"
#include "commands_animation.h"
#include "file_animations.h"

std::vector<FileAnimation> g_fileAnimations;

void LoadPathsForForm(const std::filesystem::path& path, TESForm* form)
{
	for (const auto& pair : {std::make_pair("\\_male", false), std::make_pair("\\_1stperson", true)})
	{
		auto iterPath = path.string() + std::string(pair.first);
		if (std::filesystem::exists(iterPath))
		{
			for (std::filesystem::recursive_directory_iterator iter(iterPath), end; iter != end; ++iter)
			{
				auto type = FileAnimType::Null;
				const auto fullPath = iter->path().string();
				auto str = fullPath.substr(fullPath.find("AnimGroupOverride\\"));
				Log("Loading " + str + "...");
				try
				{
					if (DYNAMIC_CAST(form, TESForm, TESObjectWEAP))
						OverrideWeaponAnimation((TESObjectWEAP*)form, str, pair.second, true);
					else if (DYNAMIC_CAST(form, TESForm, Actor))
						OverrideActorAnimation((Actor*)form, str, pair.second, true);
					else
						Log(FormatString("Form %X is neither a weapon or actor!", form->refID));
				}
				catch (std::exception& e)
				{
					Log(FormatString("AnimGroupOverride Error: %s", e.what()));
				}
				
			}
		}
		else
		{
			Log("Could not detect path " + iterPath);
		}
	}
}

void LoadModAnimPaths(const std::filesystem::path& path, const ModInfo* mod)
{
	for (std::filesystem::directory_iterator iter(path), end; iter != end; ++iter)
	{
		Log("Loading form ID " + iter->path().string());
		if (iter->is_directory())
		{
			const auto& iterPath = iter->path();
			try
			{
				char* p;
				const auto id = strtoul(iterPath.filename().string().c_str(), &p, 16);
				if (*p == 0) 
				{
					const auto formId = id + (mod->modIndex << 24);
					auto* form = LookupFormByID(formId);
					if (form)
					{
						LoadPathsForForm(iterPath, form);
					}
					else
					{
						Log(FormatString("kNVSE Animation: Form %X not found!", formId));
					}
				}
				else
				{
					Log(FormatString("Path %s could not be converted to a form ID number! it needs to be in hexadecimal form without mod index", iterPath.filename().string().c_str()));
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
