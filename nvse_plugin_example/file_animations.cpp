#include <filesystem>
#include "GameData.h"
#include "GameAPI.h"
#include "GameRTTI.h"
#include "utility.h"
#include "commands_animation.h"
#include "file_animations.h"
#include "json.h"
#include <fstream>
#include <utility>
#include <type_traits>

template <typename T>
requires std::is_same_v<T, nullptr_t> || std::is_same_v<T, UInt8> || std::is_same_v<T, const TESObjectWEAP*> || std::is_same_v<T, const Actor*>
|| std::is_same_v<T, const TESRace*> || std::is_same_v<T, const TESForm*>
void LoadPathsForType(const std::filesystem::path& dirPath, const T identifier, bool firstPerson)
{
	std::unordered_set<UInt16> variantIds;

	for (const auto& iter : std::filesystem::recursive_directory_iterator(dirPath))
	{
		if (_stricmp(iter.path().extension().string().c_str(), ".kf") != 0)
			continue;
		const auto& path = iter.path().string();
		const auto& relPath = std::filesystem::path(path.substr(path.find("AnimGroupOverride\\")));
		Log("Loading animation path " + dirPath.string() + "...");
		try
		{
			if constexpr (std::is_same_v<T, nullptr_t>)
				OverrideFormAnimation(nullptr, relPath, firstPerson, true, variantIds);
			else if constexpr (std::is_same_v<T, const TESObjectWEAP*> || std::is_same_v<T, const Actor*> || std::is_same_v<T, const TESRace*> || std::is_same_v<T, const TESForm*>)
				OverrideFormAnimation(identifier, relPath, firstPerson, true, variantIds);
			else if constexpr (std::is_same_v<T, UInt8>)
				OverrideModIndexAnimation(identifier, relPath, firstPerson, true, variantIds, nullptr, false);
		}
		catch (std::exception& e)
		{
			DebugPrint(FormatString("AnimGroupOverride Error: %s", e.what()));
		}
	}
}

void LogForm(const TESForm* form)
{
	Log(FormatString("Detected in-game form %X %s %s", form->refID, form->GetName(), form->GetFullName() ? form->GetFullName()->name.CStr() : "<no name>"));
}


template <typename T>
void LoadPathsForPOV(const std::filesystem::path& path, const T identifier)
{

	for (const auto& iter : std::filesystem::directory_iterator(path))
	{
		if (!iter.is_directory()) continue;

		const auto& str = iter.path().filename().string();
		if (_stricmp(str.c_str(), "_male") == 0)
			LoadPathsForType(iter.path(), identifier, false);
		else if (_stricmp(str.c_str(), "_1stperson") == 0)
			LoadPathsForType(iter.path(), identifier, true);
	}
}

void LoadPathsForList(const std::filesystem::path& path, const BGSListForm* listForm);

bool LoadForForm(const std::filesystem::path& iterPath, const TESForm* form)
{
	LogForm(form);
	if (const auto* weapon = DYNAMIC_CAST(form, TESForm, TESObjectWEAP))
		LoadPathsForPOV(iterPath, weapon);
	else if (const auto* actor = DYNAMIC_CAST(form, TESForm, Actor))
		LoadPathsForPOV(iterPath, actor);
	else if (const auto* list = DYNAMIC_CAST(form, TESForm, BGSListForm))
		LoadPathsForList(iterPath, list);
	else if (const auto* race = DYNAMIC_CAST(form, TESForm, TESRace))
		LoadPathsForPOV(iterPath, race);
	else
		LoadPathsForPOV(iterPath, form);
	return true;
}

void LoadPathsForList(const std::filesystem::path& path, const BGSListForm* listForm)
{
	for (auto iter = listForm->list.Begin(); !iter.End(); ++iter)
	{
		LogForm(*iter);
		LoadForForm(path, *iter);
	}
}

void LoadModAnimPaths(const std::filesystem::path& path, const ModInfo* mod)
{
	LoadPathsForPOV<const UInt8>(path, mod->modIndex);
	for (std::filesystem::directory_iterator iter(path), end; iter != end; ++iter)
	{
		const auto& iterPath = iter->path();
		Log("Loading form ID " + iterPath.string());
		if (iter->is_directory())
		{
			try
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
					{
						LoadForForm(iterPath, form);
					}
					else
					{
						DebugPrint(FormatString("Form %X not found!", formId));
					}
				}
				else
				{
					DebugPrint("Failed to convert " + folderName + " to a form ID");
				}
			}
			catch (std::exception&) {}
		}
		else
		{
			DebugPrint("Skipping as path is not a directory...");
		}
	}
}

struct JSONEntry
{
	std::string folderName;
	const TESForm* form;
	Script* conditionScript;
	bool pollCondition;
	int loadPriority;

	JSONEntry(std::string folderName, const TESForm* form, Script* script, bool pollCondition, int priority)
		: folderName(std::move(folderName)), form(form), conditionScript(script), pollCondition(pollCondition), loadPriority(priority)
	{
	}
};

std::vector<JSONEntry> g_jsonEntries;
// std::unordered_map<std::string, std::filesystem::path> g_jsonFolders;

Script* CompileConditionScript(const std::string& condString, const std::string& folderName)
{
	ScriptBuffer buffer;
	DataHandler::Get()->DisableAssignFormIDs(true);
	auto condition = MakeUnique<Script, 0x5AA0F0, 0x5AA1A0>();
	DataHandler::Get()->DisableAssignFormIDs(false);
	std::string scriptSource;
	if (!FindStringCI(condString, "SetFunctionValue"))
		scriptSource = FormatString("begin function{}\nSetFunctionValue (%s)\nend\n", condString.c_str());
	else
	{
		auto condStr = ReplaceAll(condString, "%r", "\r\n");
		condStr = ReplaceAll(condStr, "%R", "\r\n");
		scriptSource = FormatString("begin function{}\n%s\nend\n", condStr.c_str());
	}
	buffer.scriptName.Set(("kNVSEConditionScript_" + folderName).c_str());
	buffer.scriptText = scriptSource.data();
	buffer.partialScript = true;
	*buffer.scriptData = 0x1D;
	buffer.dataOffset = 4;
	buffer.currentScript = condition.get();
	const auto* ctx = ConsoleManager::GetSingleton()->scriptContext;
	const auto result = ThisStdCall<bool>(0x5AEB90, ctx, condition.get(), &buffer);
	buffer.scriptText = nullptr;
	condition->text = nullptr;
	if (!result)
	{
		DebugPrint("Failed to compile condition script " + condString);
		return nullptr;
	}
	return condition.release();
}

void HandleJson(const std::filesystem::path& path)
{
	Log("\nReading from JSON file " + path.string());
	const auto strToFormID = [](const std::string& formIdStr)
	{
		const auto formId = HexStringToInt(formIdStr);
		if (formId == -1)
		{
			DebugPrint("Form field was incorrectly formatted, got " + formIdStr);
		}
		return formId;
	};
	try
	{
		std::ifstream i(path);
		nlohmann::json j;
		i >> j;
		if (j.is_array())
		{
			for (auto& elem : j)
			{
				if (!elem.is_object())
				{
					DebugPrint("JSON error: expected object with mod, form and folder fields");
					continue;
				}
				int priority = 0;
				if (elem.contains("priority"))
					priority = elem["priority"].get<int>();
				auto modName = elem.contains("mod") ? elem["mod"].get<std::string>() : "";
				const auto* mod = !modName.empty() ? DataHandler::Get()->LookupModByName(modName.c_str()) : nullptr;
				if (!mod && !modName.empty())
				{
					DebugPrint("Mod name " + modName + " was not found");
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
				Script* condition = nullptr;
				auto pollCondition = false;
				if (elem.contains("condition"))
				{
					const auto& condStr = elem["condition"].get<std::string>();
					condition = CompileConditionScript(condStr, folder);
					if (condition)
						Log("Compiled condition script " + condStr + " successfully");
					if (elem.contains("pollCondition"))
						pollCondition = elem["pollCondition"].get<bool>();
				}
				if (mod && !formIds.empty())
				{
					for (auto formId : formIds)
					{
						formId = (mod->modIndex << 24) + (formId & 0x00FFFFFF);
						auto* form = LookupFormByID(formId);
						if (!form)
						{
							DebugPrint(FormatString("Form %X was not found", formId));
							continue;
						}
						LogForm(form);
						Log(FormatString("Registered form %X for folder %s", formId, folder.c_str()));
						g_jsonEntries.emplace_back(folder, form, condition, pollCondition, priority);
					}
				}
				else
				{
					g_jsonEntries.emplace_back(folder, nullptr, condition, pollCondition, priority);
				}
			}
		}
		else
			DebugPrint(path.string() + " does not start as a JSON array");
	}
	catch (nlohmann::json::exception& e)
	{
		DebugPrint("The JSON is incorrectly formatted! It will not be applied. Path: " + path.string());
		DebugPrint(FormatString("JSON error: %s\n", e.what()));
	}
	
}

void LoadJsonEntries()
{
	ra::sort(g_jsonEntries, [&](const JSONEntry& entry1, const JSONEntry& entry2)
	{
		return entry1.loadPriority < entry2.loadPriority;
	});
	for (const auto& entry : g_jsonEntries)
	{
		g_jsonContext.script = entry.conditionScript;
		g_jsonContext.pollCondition = entry.pollCondition;
		if (entry.form)
			Log(FormatString("JSON: Loading animations for form %X in path %s", entry.form->refID, entry.folderName.c_str()));
		else
			Log("JSON: Loading animations for global override in path " + entry.folderName);
		const auto path = GetCurPath() + R"(\Data\Meshes\AnimGroupOverride\)" + entry.folderName;
		if (!std::filesystem::exists(path))
			Log(FormatString("Path %s does not exist yet it is present in JSON", path.c_str()));
		else if (!entry.form) // global
			LoadPathsForPOV(path, nullptr);
		else if (!LoadForForm(path, entry.form))
			Log(FormatString("Loaded from JSON folder %s to form %X", path.c_str(), entry.form->refID));
		g_jsonContext.Reset();
	}
	g_jsonEntries = std::vector<JSONEntry>();
}

struct PathNode
{
	std::map<std::string_view, PathNode> children;
	std::string_view path;
};

PathNode CreatePathTree(const std::vector<std::string_view>& paths)
{
	PathNode root;

	for (const auto& path : paths) {
		PathNode* current = &root;
		size_t start = 0;
		size_t end = path.find('\\');

		while (end != std::string_view::npos) {
			std::string_view component = path.substr(start, end - start);
			current = &current->children[component];
			start = end + 1;
			end = path.find('\\', start);
		}

		// Handle the last component
		std::string_view component = path.substr(start);
		current = &current->children[component];
		current->path = path;
	}

	return root;
}

void LoadFileAnimPaths()
{
	Log("Loading file anims");
	const auto dir = GetCurPath() + R"(\Data\Meshes\AnimGroupOverride)";
	const auto then = std::chrono::system_clock::now();
	if (std::filesystem::exists(dir))
	{
		for (const auto& iter: std::filesystem::directory_iterator(dir.c_str()))
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
						DebugPrint(FormatString("Mod with name %s is not loaded!", fileName.string().c_str()));
				}
				
			}
			else if (_stricmp(path.extension().string().c_str(), ".json") == 0)
			{
				HandleJson(iter.path());
			}
		}
	}
	else
	{
		Log(dir + " does not exist.");
	}
	LoadJsonEntries();
	const auto now = std::chrono::system_clock::now();
	const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - then);
	DebugPrint(FormatString("Loaded AnimGroupOverride in %d ms", diff.count()));
}
