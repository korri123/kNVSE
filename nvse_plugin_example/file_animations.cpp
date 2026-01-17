#include <filesystem>
#include "GameData.h"
#include "GameAPI.h"
#include "GameRTTI.h"
#include "utility.h"
#include "commands_animation.h"
#include "file_animations.h"
#include "lib/json/json.h"
#include <fstream>
#include <ranges>
#include <utility>
#include <type_traits>

#include "string_view_util.h"
#include "bethesda/bethesda_types.h"

namespace fs = std::filesystem;

std::string_view AddStringToPool(const std::string_view str)
{
	auto* handle = NiGlobalStringTable::AddString(str.data());
	return { handle, str.size() };
}

struct JSONEntry
{
	std::string folderName;
	const TESForm* form;
	std::string_view condition;
	int loadPriority;
	bool pollCondition;
	bool matchBaseGroupId;
	std::string_view bsa;

	JSONEntry(std::string folderName, const TESForm* form, std::string_view condition, bool pollCondition, int priority, bool matchBaseGroupId, std::string_view bsa)
		: folderName(std::move(folderName)), form(form), condition(condition), loadPriority(priority), pollCondition(pollCondition),
	matchBaseGroupId(matchBaseGroupId), bsa(bsa)
	{
	}
};

void LoadPathsForType(const fs::path& dirPath, const UInt32 identifier, bool firstPerson, bool isModIndex, JSONEntry* jsonEntry = nullptr)
{
	AnimOverrideData animOverrideData = {
		.identifier = identifier,
		.enable = true,
		.conditionScript = nullptr,
		.pollCondition = false,
		.matchBaseGroupId = false,
	};
	if (jsonEntry)
	{
		animOverrideData.conditionScriptText = jsonEntry->condition;
		animOverrideData.pollCondition = jsonEntry->pollCondition;
		animOverrideData.matchBaseGroupId = jsonEntry->matchBaseGroupId;
	}
	for (const auto& iter : fs::recursive_directory_iterator(dirPath))
	{
		if (!sv::equals_ci(iter.path().extension().string(), ".kf"))
			continue;
		const auto& relPath = GetRelativePath(iter.path(), "AnimGroupOverride");
		const std::string_view path = AddStringToPool(ToLower(relPath.string()));
		animOverrideData.path = path;
		try
		{
			if (isModIndex)
				OverrideModIndexAnimation(animOverrideData, firstPerson);
			else
				OverrideFormAnimation(animOverrideData, firstPerson);
		}
		catch (std::exception& e)
		{
			ERROR_LOG(FormatString("AnimGroupOverride Error: %s", e.what()));
		}
	}
}

void LoadPathsForPOV(const fs::path& path, UInt32 identifier, bool isModIndex, JSONEntry* jsonEntry = nullptr)
{
	for (const auto& iter : fs::directory_iterator(path))
	{
		if (!iter.is_directory()) continue;
		const auto& str = iter.path().filename().string();
		if (_stricmp(str.c_str(), "_male") == 0)
			LoadPathsForType(iter.path(), identifier, false, isModIndex, jsonEntry);
		else if (_stricmp(str.c_str(), "_1stperson") == 0)
			LoadPathsForType(iter.path(), identifier, true, isModIndex, jsonEntry);
	}
}

void LoadPathsForList(const fs::path& path, const BGSListForm* listForm, JSONEntry* jsonEntry = nullptr);

bool LoadForForm(const fs::path& iterPath, const TESForm* form, JSONEntry* jsonEntry = nullptr)
{
	if (const auto* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP))
		LoadPathsForPOV(iterPath, weapon->refID, false, jsonEntry);
	else if (const auto* actor = DYNAMIC_CAST(form, TESForm, Actor))
		LoadPathsForPOV(iterPath, actor->refID, false, jsonEntry);
	else if (const auto* list = DYNAMIC_CAST(form, TESForm, BGSListForm))
		LoadPathsForList(iterPath, list, jsonEntry);
	else if (const auto* race = DYNAMIC_CAST(form, TESForm, TESRace))
		LoadPathsForPOV(iterPath, race->refID, false, jsonEntry);
	else
		LoadPathsForPOV(iterPath, form->refID, false, jsonEntry);
	return true;
}

void LoadPathsForList(const fs::path& path, const BGSListForm* listForm, JSONEntry* jsonEntry)
{
	for (auto iter = listForm->list.Begin(); !iter.End(); ++iter)
	{
		LoadForForm(path, *iter, jsonEntry);
	}
}

void LoadModAnimPaths(const fs::path& path, const ModInfo* mod)
{
	LoadPathsForPOV(path, mod->modIndex, true);
	for (fs::directory_iterator iter(path), end; iter != end; ++iter)
	{
		const auto& iterPath = iter->path();
		if (iter->is_directory())
		{
			const auto& folderName = iterPath.filename().string();
			if (folderName[0] == '_') // _1stperson, _male
				continue;
			const auto id = HexStringToInt(folderName);
			if (id != -1) 
			{
				const auto formId = (id & 0x00FFFFFF) + (mod->modIndex << 24);
				auto* form = LookupFormByID(formId);
				if (form)
					LoadForForm(iterPath, form);
				else
					ERROR_LOG(FormatString("Form %X not found!", formId));
			}
			else
				ERROR_LOG("Failed to convert " + folderName + " to a form ID");
		}
		
	}
}

struct ScopedTimer
{
	ScopedTimer(const char* name)
	{
		this->name = name;
		this->timer = std::chrono::high_resolution_clock::now();
	}
	
	~ScopedTimer()
	{
		const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - timer);
		ERROR_LOG(FormatString("%s in %d ms", name, diff.count()));
	}
	std::chrono::high_resolution_clock::time_point timer;
	const char* name;
};

void HandleJson(const fs::path& path, std::vector<JSONEntry>& jsonEntries)
{
	LOG("\nReading from JSON file " + path.string());
	const auto strToFormID = [](const std::string& formIdStr)
	{
		const auto formId = HexStringToInt(formIdStr);
		if (formId == -1)
		{
			ERROR_LOG("Form field was incorrectly formatted, got " + formIdStr);
		}
		return formId;
	};
	try
	{
		std::ifstream i(path);
		if (i.peek() == std::ifstream::traits_type::eof())
			return;
		nlohmann::json j;
		i >> j;
		if (j.is_array())
		{
			for (auto& elem : j)
			{
				if (!elem.is_object())
				{
					ERROR_LOG("JSON error: expected object with mod, form and folder fields");
					continue;
				}
				int priority = 0;
				if (elem.contains("priority"))
					priority = elem["priority"].get<int>();
				auto modName = elem.contains("mod") ? elem["mod"].get<std::string>() : "";
				const auto* mod = !modName.empty() ? DataHandler::Get()->LookupModByName(modName.c_str()) : nullptr;
				if (!mod && !modName.empty())
				{
					continue;
				}
				const auto& folder = elem["folder"].get<std::string>();
				std::vector<int> formIds;
				auto* formElem = elem.contains("form") ? &elem["form"] : nullptr;
				if (formElem)
				{
					if (formElem->is_array())
						std::ranges::transform(*formElem, std::back_inserter(formIds), [&](auto& i) {return strToFormID(i.template get<std::string>()); });
					else
						formIds.push_back(strToFormID(formElem->get<std::string>()));
					if (std::ranges::find(formIds, -1) != formIds.end())
						continue;
				}
				std::string_view condition;
				auto pollCondition = false;
				if (elem.contains("condition"))
				{
					condition = AddStringToPool(elem["condition"].get<std::string_view>());
					if (elem.contains("pollCondition"))
						pollCondition = elem["pollCondition"].get<bool>();
				}
				
				std::string_view bsa;
				if (elem.contains("bsa"))
				{
					bsa = AddStringToPool(elem["bsa"].get<std::string_view>());
				}
				
				auto matchBaseGroupId = false;
				if (elem.contains("matchBaseAnimGroup"))
				{
					matchBaseGroupId = elem["matchBaseAnimGroup"].get<bool>();
				}
				if (mod && !formIds.empty())
				{
					for (auto formId : formIds)
					{
						formId = (mod->modIndex << 24) + (formId & 0x00FFFFFF);
						auto* form = LookupFormByID(formId);
						if (!form)
						{
							LOG(FormatString("Form %X was not found", formId));
							continue;
						}
						//LOG(FormatString("Registered form %X for folder %s", formId, folder.c_str()));
						jsonEntries.emplace_back(folder, form, condition, pollCondition, priority, matchBaseGroupId, bsa);
					}
				}
				else
				{
					jsonEntries.emplace_back(folder, nullptr, condition, pollCondition, priority, matchBaseGroupId, bsa);
				}
			}
		}
		else
			ERROR_LOG(path.string() + " does not start as a JSON array");
	}
	catch (nlohmann::json::exception& e)
	{
		ERROR_LOG("The JSON is incorrectly formatted! It will not be applied. Path: " + path.string());
		ERROR_LOG(FormatString("JSON error: %s\n", e.what()));
	}
}

bool OverrideBSAPathAnim(AnimOverrideData& animOverrideData, const std::string_view path)
{
	const auto firstPerson = sv::contains_ci(path, "_1stperson");
	const auto thirdPerson = sv::contains_ci(path, "_male");
	if (!thirdPerson && !firstPerson)
		return false;
	animOverrideData.path = AddStringToPool(path);
	if (animOverrideData.identifier == 0xFF)
		return OverrideModIndexAnimation(animOverrideData, firstPerson);
	return OverrideFormAnimation(animOverrideData, firstPerson);
}

int OverrideBSAPathAnimationsForRange(AnimOverrideData& animOverrideData, std::ranges::forward_range auto&& thisModsPaths)
{
	int numFound = 0;
	for (auto path : thisModsPaths)
	{
		if (OverrideBSAPathAnim(animOverrideData, path))
			++numFound;
	}
	return numFound;
}

int OverrideBSAPathAnimationsForList(AnimOverrideData& animOverrideData, std::string_view basePath)
{
	const auto list = ArchiveManager::GetDirectoryAnimPaths(basePath);
	if (list.Empty())
		return 0;
	int numFound = 0;
	for (const char* pPath : list)
	{
		const std::string_view path(pPath);
		if (!sv::starts_with_ci(path, basePath))
			continue;
		if (OverrideBSAPathAnim(animOverrideData, path))
			++numFound;
	}
	return numFound;
}

bool LoadDataFolderBSAPaths(JSONEntry& entry)
{
	const auto basePath = "meshes\\animgroupoverride\\" + ToLower(entry.folderName) + "\\";
	const auto subfolders = {"_1stperson\\", "_male\\"};
	const auto childFolders = {"mod1\\", "mod2\\", "mod3\\", "hurt\\", "human\\", "male\\", "female\\"};
	int numFound = 0;

	if (!entry.bsa.empty())
	{
		sv::stack_string<0x400> bsaPath("DATA\\%s", entry.bsa.data());
		auto* bsaArchive = ArchiveManager::OpenArchive(bsaPath.data(), ARCHIVE_TYPE_MESHES, false);
		if (!bsaArchive)
		{
			bsaPath = sv::stack_string<0x400>(R"(DATA\Meshes\AnimGroupOverride\%s)", entry.bsa.data());
			bsaArchive = ArchiveManager::OpenArchive(bsaPath.data(), ARCHIVE_TYPE_MESHES, false);
		}
		if (!bsaArchive)
			ERROR_LOG(FormatString("BSA not found: %s", entry.bsa));
	}

	AnimOverrideData animOverrideData = {
		.identifier = entry.form ? entry.form->refID : 0xFF,
		.enable = true,
		.conditionScriptText = entry.condition,
		.pollCondition = entry.pollCondition,
		.matchBaseGroupId = entry.matchBaseGroupId,
	};

	for (const auto& subfolder : subfolders)
	{
		std::string path = basePath + subfolder;
		numFound += OverrideBSAPathAnimationsForList(animOverrideData, path);
		for (const auto& childFolder : childFolders)
		{
			std::string childPath = basePath + subfolder + childFolder;
			numFound += OverrideBSAPathAnimationsForList(animOverrideData, childPath);
		}
	}

	return numFound != 0;
}

bool LoadJSONInBSAPaths(const std::vector<std::string_view>& bsaAnimPaths, JSONEntry& entry)
{
	const auto jsonFolderPath = "meshes\\animgroupoverride\\" + ToLower(entry.folderName) + "\\";
	auto thisModsPaths = bsaAnimPaths | ra::views::filter([&](const std::string_view& bsaPath)
	{
		return sv::starts_with_ci(bsaPath, jsonFolderPath);
	});
	if (ra::empty(thisModsPaths))
		return false;
	
	AnimOverrideData animOverrideData = {
		.identifier = entry.form ? entry.form->refID : 0xFF,
		.enable = true,
		.conditionScriptText = entry.condition,
		.pollCondition = entry.pollCondition,
		.matchBaseGroupId = entry.matchBaseGroupId,
	};
	
	OverrideBSAPathAnimationsForRange(animOverrideData, std::move(thisModsPaths));
	return true;
}

void LoadJsonEntries(std::vector<JSONEntry>& jsonEntries, const std::vector<std::string_view>& bsaAnimPaths)
{
	ra::sort(jsonEntries, [&](const JSONEntry& entry1, const JSONEntry& entry2)
	{
		return entry1.loadPriority < entry2.loadPriority;
	});
	for (auto& entry : jsonEntries)
	{
		if (entry.form)
			LOG(FormatString("JSON: Loading animations for form %X in path %s", entry.form->refID, entry.folderName.c_str()));
		else
			LOG("JSON: Loading animations for global override in path " + entry.folderName);
		const auto path = R"(data\meshes\animgroupoverride\)" + entry.folderName;
		if (!fs::exists(path))
		{
			bool success = false;
			success |= LoadJSONInBSAPaths(bsaAnimPaths, entry);
			success |= LoadDataFolderBSAPaths(entry);
			if (!success)
				LOG(FormatString("Path %s does not exist yet it is present in JSON", path.c_str()));
			continue;
		}
		if (!entry.form) // global
			LoadPathsForPOV(path, 0xFF, true, &entry);
		else if (LoadForForm(path, entry.form, &entry))
			LOG(FormatString("Loaded from JSON folder %s to form %X", path.c_str(), entry.form->refID));
	}
}

void LoadAnimPathsFromBSA(const fs::path& path, std::vector<std::string_view>& animPaths)
{
	auto* archive = ArchiveManager::OpenArchive(path.string().c_str(), ARCHIVE_TYPE_MESHES, false);
	
	const char* dirStrings = archive->pDirectoryStringArray;
	const char* fileStrings = archive->pFileNameStringArray;
	
	if (!dirStrings || !fileStrings)
		return;
        
	for (UInt32 i = 0; i < archive->kArchive.uiDirectories; ++i)
	{
		const size_t dirOffset = archive->pDirectoryStringOffsets[i];
		std::string_view directory(dirStrings + dirOffset);

		if (!directory.starts_with("meshes\\animgroupoverride\\"))
		{
			LOG("Skipping BSA directory entry " + std::string(directory) + " as it is not meshes\\animgroupoverride");
			continue;
		}

		for (UInt32 j = 0; j < archive->kArchive.pDirectories[i].uiFiles; ++j)
		{
			const size_t fileOffset = archive->pFileNameStringOffsets[i][j];
			std::string_view fileName(fileStrings + fileOffset);
			if (sv::get_file_extension(fileName) != ".kf")
				continue;
			char buffer[0x400]; 
			if (const auto result = sprintf_s(buffer, "%s\\%s", directory.data(), fileName.data()); result != -1)
				animPaths.emplace_back(AddStringToPool({buffer, static_cast<size_t>(result)}));
			else [[unlikely]]
				ERROR_LOG("Failed to format path: " + std::string(directory) + "\\" + std::string(fileName));
		}
	}
}

void LoadFileAnimPaths()
{
	ScopedTimer timer("Loaded AnimGroupOverride");
	LOG("Loading file anims");

	const fs::path dir = R"(Data\Meshes\AnimGroupOverride)";
	std::vector<std::string_view> bsaAnimPaths;
	std::vector<JSONEntry> jsonEntries;
	if (exists(dir))
	{
		for (const auto& iter : fs::directory_iterator(dir))
		{
			const auto& path = iter.path();
			const auto ext = path.extension().string();
			if (iter.is_directory())
			{
				if (sv::equals_ci(ext, ".esp") || sv::equals_ci(ext, ".esm"))
				{
					const auto fileName = path.filename().string();
					if (const auto* mod = DataHandler::Get()->LookupModByName(fileName.c_str()))
						LoadModAnimPaths(path, mod);
					else
						ERROR_LOG(FormatString("Mod with name %s is not loaded!", fileName.c_str()));
				}
			}
			else if (sv::equals_ci(ext, ".json"))
				HandleJson(path, jsonEntries);
			else if (sv::equals_ci(ext, ".bsa"))
				LoadAnimPathsFromBSA(path, bsaAnimPaths);
		}
	}
	else
	{
		LOG(dir.string() + " does not exist.");
	}
	LoadJsonEntries(jsonEntries, bsaAnimPaths);
}


