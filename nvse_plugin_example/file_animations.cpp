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



template <typename T>
void LoadPathsForType(const std::filesystem::path& dirPath, const T identifier, bool firstPerson)
{
	std::unordered_set<UInt16> variantIds;
	for (std::filesystem::recursive_directory_iterator iter(dirPath), end; iter != end; ++iter)
	{
		if (_stricmp(iter->path().extension().string().c_str(), ".kf") != 0)
			continue;
		const auto& path = iter->path().string();
		const auto& relPath = std::filesystem::path(path.substr(path.find("AnimGroupOverride\\")));
		Log("Loading animation path " + dirPath.string() + "...");
		try
		{
			if constexpr (std::is_same<T, nullptr_t>::value) {
				Log("First case ");
				OverrideFormAnimation(nullptr, relPath, firstPerson, true, variantIds);
			}
			else if constexpr (std::is_same_v<T, const TESObjectWEAP*> || std::is_same_v<T, const Actor*> || std::is_same_v<T, const TESRace*> || std::is_same_v<T, const TESForm*>)
			{
				Log("Second case ");
				OverrideFormAnimation(identifier, relPath, firstPerson, true, variantIds);
			}
			else if constexpr (std::is_same_v<T, UInt8>) {
				Log("Third case " + relPath.string());
				OverrideModIndexAnimation(identifier, relPath, firstPerson, true, variantIds, nullptr, false);
			}
			else
				static_assert(false);
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
	for (const auto& pair : {std::make_pair("\\_male", false), std::make_pair("\\_1stperson", true)})
	{
		auto iterPath = path.string() + std::string(pair.first);
		if (std::filesystem::exists(iterPath))
			LoadPathsForType(iterPath, identifier, pair.second);
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
	auto* ctx = ConsoleManager::GetSingleton()->scriptContext;
	const auto patchEndOfLineCheck = [&](bool bDisableCheck)
	{
		// remove this when xNVSE 6.1.10/6.2 releases
		if (bDisableCheck)
			SafeWrite8(0x5B3A8D, 0xEB);
		else
			SafeWrite8(0x5B3A8D, 0x73);
	};
	patchEndOfLineCheck(true);
	auto result = ThisStdCall<bool>(0x5AEB90, ctx, condition.get(), &buffer);
	patchEndOfLineCheck(false);
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
		DebugPrint("The JSON is incorrectly formatted! It will not be applied.");
		DebugPrint(FormatString("JSON error: %s\n", e.what()));
	}

}

void HandleJson2(const std::filesystem::path& path, std::map<std::string, std::vector<int>>& typesMap)
{
	Log("\nReading types from JSON file " + path.string());

	const auto processTypesMod = [](int modIDX, Script* condition, bool pollCondition, int& priority, std::vector<std::string>& weapType, std::map<std::string, std::vector<int>>& typesMap)
	{

		const auto GetHasType2 = [](std::vector<int>& weapParams, std::vector<int>& typesData) // -1 is "any" for better flexibility
		{
			if (
				(weapParams[0] == typesData[0] || typesData[0] == -1) &&
				(weapParams[1] == typesData[1] || typesData[1] == -1) &&
				(weapParams[2] == typesData[2] || typesData[2] == -1) &&
				(weapParams[3] == typesData[3] || typesData[3] == -1)
				) {

				return true;
			}
			return false;
		};

		NiTPointerMap<TESForm>* formsMap = *(NiTPointerMap<TESForm>**)0x11C54C0;

		for (auto mIter = formsMap->Begin(); mIter; ++mIter) { // credits to Yvileapsis for the iteration example

				TESForm* form = mIter.Get();
				if (form -> GetModIndex() == modIDX || modIDX == -1) {

					if (form->IsWeapon()) {
						auto weap = static_cast<TESObjectWEAP*>(form);
						std::string test = weap->GetName();

						Log("Type to " + test + " : mod ID " + std::to_string(form->GetModIndex()));

						std::vector<int> weapParams;
						weapParams.push_back(weap->eWeaponType); //Animation type
						weapParams.push_back(weap->handGrip); //Grip type
						weapParams.push_back(weap->reloadAnim); //Reload animation (non-modded)
						weapParams.push_back(weap->attackAnim); //Attack animation
						Log("Animation type: " + std::to_string(weapParams[0]));
						Log("Grip type: " + std::to_string(weapParams[1]));
						Log("Reload animation: " + std::to_string(weapParams[2]));
						Log("Attack animation: " + std::to_string(weapParams[3]));
						// the log messages above are actually usefull for working with types
						// there's also a "reference" in tesEdit code

						for (int i = 0; i < weapType.size(); i++) {

							try {
								if (GetHasType2(weapParams, typesMap.at(weapType[i]))) {
									Log("Replacing animations with " + weapType[i] + " for " + test);
									g_jsonEntries.emplace_back(weapType[i], form, condition, pollCondition, priority);
								}
							}
							catch (const std::out_of_range& oor) {
								DebugPrint("This weapontype wasn't defined: " + weapType[i]);
							}
						}
					}
					else {
						//Log("Type: wrong item " + std::to_string(traverse->key) + "; Current rem: " + std::to_string(traverse->key % formsMap->m_numBuckets));
					}
				}

		}

		return true;
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
				auto typeData = elem.contains("typedata") ? &elem["typedata"] : nullptr;
				//auto typeName = elem["name"].get<std::string>();
				const auto* mod = !modName.empty() ? DataHandler::Get()->LookupModByName(modName.c_str()) : nullptr;

				const auto& types = elem.contains("weapontypes") ? &elem["weapontypes"] : nullptr;

				const auto& folderType = elem.contains("folderType") ? elem["folderType"].get<std::string>() : "";

				std::vector<std::string> weapType;


				if (typeData && folderType != "") { // defining types
					if (!typeData->is_array())
					{
						continue;
					}
					else {
						std::ranges::transform(*typeData, std::back_inserter(typesMap[folderType]), [&](auto& i) {return i; });

					}

				}

				if (types) { // reading applied types

					if (!types->is_array())
					{
						continue;
					}
					else {
						std::ranges::transform(*types, std::back_inserter(weapType), [&](auto& i) {return i.template get<std::string>(); });
					}
				}

				if (!mod && !modName.empty()) // stop, since types don't have mods, and mods don't have type definitions
				{
					DebugPrint("Mod name " + modName + " was not found");
					continue;
				}

				const auto& folder = elem.contains("folder") ? elem["folder"].get<std::string>() : "";
				auto* formElem = elem.contains("form") ? &elem["form"] : nullptr;
				Script* condition = nullptr;
				auto pollCondition = false;
				if (!formElem) { // no forms should be here
					if (mod)
					{
						if (folder != "") { // same as folder named after a plugin, but in json, saves space a little

							const auto dirX = GetCurPath() + R"(\Data\Meshes\AnimGroupOverride\)" + folder;
							Log(FormatString("Registered all forms for folder %s", dirX));
							std::filesystem::path folderX(dirX);
							LoadModAnimPaths(folderX, mod);
						}
						else if (types) { // the main thing

							Log("Found type in " + modName + " with " + weapType[1] + " replaced by " + folder.c_str() + "; mod IDX = " + std::to_string(mod->modIndex));

							processTypesMod(mod->modIndex, condition, pollCondition, priority, weapType, typesMap);
						}

					}
					else if (types) // global replacement
					{

						const UInt8 lastModIndex = DataHandler::Get()->GetActiveModCount();

						Log("Found type without a mod, counting: " + std::to_string(lastModIndex));

						processTypesMod(-1, condition, pollCondition, priority, weapType, typesMap);
					}
				}
			}
		}
		else
			DebugPrint(path.string() + " does not start as a JSON array");
	}
	catch (nlohmann::json::exception& e)
	{
		DebugPrint("The JSON is incorrectly formatted! It will not be applied.");
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
		if (!entry.form) // global
			LoadPathsForPOV(path, nullptr);
		else if (!LoadForForm(path, entry.form))
			Log(FormatString("Loaded from JSON folder %s to form %X", path.c_str(), entry.form->refID));
		g_jsonContext.Reset();
	}
	g_jsonEntries = std::vector<JSONEntry>();
}

void LoadFileAnimPaths2()
{
	Log("Loading file anims 2");
	const auto dir = GetCurPath() + R"(\Data\Meshes\AnimGroupOverride)";
	const auto then = std::chrono::system_clock::now();
	std::map<std::string, std::vector<int>> typesMap;
	if (std::filesystem::exists(dir))
	{
		for (std::filesystem::directory_iterator iter(dir.c_str()), end; iter != end; ++iter)
		{
			const auto& path = iter->path();
			const auto& fileName = path.filename().string();
			if (_stricmp(path.extension().string().c_str(), ".json") == 0 && fileName.find("_types_") == 0)
			{
				HandleJson2(iter->path(), typesMap);
			}
		}
	}
	else
	{
		Log(dir + " does not exist.");
	}
	//LoadJsonEntries2();
	const auto now = std::chrono::system_clock::now();
	const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - then);
	DebugPrint(FormatString("Loaded AnimGroupOverride2 in %d ms", diff.count()));
}

void LoadFileAnimPaths()
{
	LoadFileAnimPaths2(); // making sure that _types_ are loaded first

	Log("Loading file anims");
	const auto dir = GetCurPath() + R"(\Data\Meshes\AnimGroupOverride)";
	const auto then = std::chrono::system_clock::now();
	if (std::filesystem::exists(dir))
	{
		for (std::filesystem::directory_iterator iter(dir.c_str()), end; iter != end; ++iter)
		{
			const auto& path = iter->path();
			const auto& fileName = path.filename();
			if (iter->is_directory())
			{
				Log(iter->path().string() + " found");

				const auto* mod = DataHandler::Get()->LookupModByName(fileName.string().c_str());

				if (mod)
					LoadModAnimPaths(path, mod);
				else if (_stricmp(fileName.extension().string().c_str(), ".esp") == 0 || _stricmp(fileName.extension().string().c_str(), ".esm") == 0)
					DebugPrint(FormatString("Mod with name %s is not loaded!", fileName.string().c_str()));
				else if (_stricmp(fileName.string().c_str(), "_male") != 0 && _stricmp(fileName.string().c_str(), "_1stperson") != 0)
				{
					Log("Found anim folder " + fileName.string() + " which can be used in JSON");
				}
			}
			else if (_stricmp(path.extension().string().c_str(), ".json") == 0)
			{
				HandleJson(iter->path());
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


