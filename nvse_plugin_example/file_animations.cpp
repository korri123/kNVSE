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

	JSONEntry(std::string folderName, const TESForm* form, std::string_view condition, bool pollCondition, int priority, bool matchBaseGroupId)
		: folderName(std::move(folderName)), form(form), condition(condition), loadPriority(priority), pollCondition(pollCondition),
	matchBaseGroupId(matchBaseGroupId)
	{
	}
};

void LoadPathsForType(const fs::path& dirPath, const UInt32 identifier, bool firstPerson, JSONEntry* jsonEntry = nullptr)
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
		animOverrideData.condition = std::move(jsonEntry->condition);
		animOverrideData.pollCondition = jsonEntry->pollCondition;
		animOverrideData.matchBaseGroupId = jsonEntry->matchBaseGroupId;
	}
	for (const auto& iter : fs::recursive_directory_iterator(dirPath))
	{
		if (_stricmp(iter.path().extension().string().c_str(), ".kf") != 0)
			continue;
		const auto& relPath = GetRelativePath(iter.path(), "AnimGroupOverride");
		const std::string_view path = AddStringToPool(ToLower(relPath.string()));
		animOverrideData.path = path;
		try
		{
			OverrideFormAnimation(animOverrideData, firstPerson);
		}
		catch (std::exception& e)
		{
			ERROR_LOG(FormatString("AnimGroupOverride Error: %s", e.what()));
		}
	}
}

void LoadPathsForPOV(const fs::path& path, UInt32 identifier, JSONEntry* jsonEntry = nullptr)
{
	for (const auto& iter : fs::directory_iterator(path))
	{
		if (!iter.is_directory()) continue;
		const auto& str = iter.path().filename().string();
		if (_stricmp(str.c_str(), "_male") == 0)
			LoadPathsForType(iter.path(), identifier, false, jsonEntry);
		else if (_stricmp(str.c_str(), "_1stperson") == 0)
			LoadPathsForType(iter.path(), identifier, true, jsonEntry);
	}
}

void LoadPathsForList(const fs::path& path, const BGSListForm* listForm, JSONEntry* jsonEntry = nullptr);

bool LoadForForm(const fs::path& iterPath, const TESForm* form, JSONEntry* jsonEntry = nullptr)
{
	if (const auto* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP))
		LoadPathsForPOV(iterPath, weapon->refID, jsonEntry);
	else if (const auto* actor = DYNAMIC_CAST(form, TESForm, Actor))
		LoadPathsForPOV(iterPath, actor->refID, jsonEntry);
	else if (const auto* list = DYNAMIC_CAST(form, TESForm, BGSListForm))
		LoadPathsForList(iterPath, list, jsonEntry);
	else if (const auto* race = DYNAMIC_CAST(form, TESForm, TESRace))
		LoadPathsForPOV(iterPath, race->refID, jsonEntry);
	else
		LoadPathsForPOV(iterPath, form->refID, jsonEntry);
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
	LoadPathsForPOV(path, mod->modIndex);
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
							//ERROR_LOG(FormatString("Form %X was not found", formId));
							continue;
						}
						LOG(FormatString("Registered form %X for folder %s", formId, folder.c_str()));
						jsonEntries.emplace_back(folder, form, condition, pollCondition, priority, matchBaseGroupId);
					}
				}
				else
				{
					jsonEntries.emplace_back(folder, nullptr, condition, pollCondition, priority, matchBaseGroupId);
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

bool LoadJSONInBSAPaths(const std::vector<std::string_view>& bsaAnimPaths, JSONEntry& entry)
{
	const auto jsonFolderPath = "meshes\\animgroupoverride\\" + ToLower(entry.folderName) + "\\";
	auto thisModsPaths = bsaAnimPaths | ra::views::filter([&](const std::string_view& bsaPath)
	{
		return bsaPath.starts_with(jsonFolderPath);
	});
	if (ra::empty(thisModsPaths))
		return false;
	
	AnimOverrideData animOverrideData = {
		.identifier = entry.form->refID,
		.enable = true,
		.condition = entry.condition,
		.pollCondition = entry.pollCondition,
		.matchBaseGroupId = entry.matchBaseGroupId,
	};
	for (auto path : thisModsPaths)
	{
		const auto firstPerson = path.contains("_1stperson");
		const auto thirdPerson = path.contains("_male");
		if (!thirdPerson && !firstPerson)
			continue;
		animOverrideData.path = path;
		OverrideFormAnimation(animOverrideData, firstPerson);
	}
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
			if (!LoadJSONInBSAPaths(bsaAnimPaths, entry))
				LOG(FormatString("Path %s does not exist yet it is present in JSON", path.c_str()));
			continue;
		}
		if (!entry.form) // global
			LoadPathsForPOV(path, 0, &entry);
		else if (LoadForForm(path, entry.form, &entry))
			LOG(FormatString("Loaded from JSON folder %s to form %X", path.c_str(), entry.form->refID));
	}
}

void LoadAnimPathsFromBSA(const fs::path& path, std::vector<std::string_view>& animPaths)
{
	ArchivePtr archive = ArchiveManager::OpenArchive(path.string().c_str(), ARCHIVE_TYPE_MESHES, false);
	
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
		for (const auto& iter: fs::directory_iterator(dir))
		{
			const auto& path = iter.path();
			if (iter.is_directory())
			{
				const auto& fileName = path.filename();
				const auto& extension = fileName.extension().string();
				const auto isMod = _stricmp(extension.c_str(), ".esp") == 0 || _stricmp(extension.c_str(), ".esm") == 0;
				const ModInfo* mod;
				if (isMod)
				{
					if ((mod = DataHandler::Get()->LookupModByName(fileName.string().c_str())))
						LoadModAnimPaths(path, mod);
					else
						ERROR_LOG(FormatString("Mod with name %s is not loaded!", fileName.string().c_str()));
				}
				
			}
			else if (_stricmp(path.extension().string().c_str(), ".json") == 0)
				HandleJson(iter.path(), jsonEntries);
			else if (path.extension() == ".bsa")
			{
				LoadAnimPathsFromBSA(iter.path(), bsaAnimPaths);
			}
		}
	}
	else
	{
		LOG(dir.string() + " does not exist.");
	}
	LoadJsonEntries(jsonEntries, bsaAnimPaths);
}


