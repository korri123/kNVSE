#include <filesystem>
#include "GameData.h"
#include "GameAPI.h"
#include "GameRTTI.h"
#include "utility.h"
#include "commands_animation.h"
#include "file_animations.h"
#include "lib/json/json.h"
#include <fstream>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_map>
#include <utility>

#include "anim_overrides.h"
#include "override_prefs.h"
#include "string_view_util.h"
#include "bethesda/bethesda_types.h"

namespace fs = std::filesystem;

std::string_view AddStringToPool(const std::string_view str)
{
	auto* handle = NiGlobalStringTable::AddString(str.data());
	return { handle, str.size() };
}

namespace
{
	constexpr std::string_view kMeshes = R"(Data\Meshes\)";

	struct JSONEntry
	{
		std::string folderName;
		const TESForm* form;
		std::string_view condition;
		int loadPriority;
		bool pollCondition;
		bool matchBaseGroupId;
		// The JSON file's name.
		std::string jsonFile;
	};

	// One KF of an AnimGroupOverride folder.
	struct FolderFile
	{
		// Relative to Data\Meshes.
		std::string path;
		bool firstPerson;
	};

	// Caches what has been read of the folders. A JSON file can name the same folder for several forms (one of the NVAO
	// folders for 33), and the cache avoids reading it from disk for each.
	struct FolderCache
	{
		// Keyed by path as given.
		std::unordered_map<std::string, std::vector<FolderFile>> looseFiles;
		// Keyed by lower case folder name: the KFs that archives have of the folder.
		std::unordered_map<std::string, std::vector<FolderFile>> archiveFiles;
	};

	// The game opens files by their ANSI path, so a name the code page cannot represent would not load. path::string()
	// throws for such a name. This version substitutes a default character, so the load fails and is logged like any
	// missing file.
	std::string ToAnsi(const fs::path& path)
	{
		const auto& wide = path.native();
		if (wide.empty())
			return {};
		const auto size = WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
		std::string result(size, '\0');
		WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
		return result;
	}

	// Lists a directory's entries in the order NTFS lists them (by upper case name), whatever the file system (a virtual
	// one need not sort). The registration order decides which of two sets ends up on top of a stack.
	std::vector<fs::directory_entry> GetSortedEntries(const fs::path& directory)
	{
		std::vector<fs::directory_entry> entries;
		std::error_code error;
		for (fs::directory_iterator iter(directory, error), end; !error && iter != end; iter.increment(error))
			entries.push_back(*iter);
		if (error)
			ERROR_LOG(FormatString("Failed to read folder %s: %s", ToAnsi(directory).c_str(), error.message().c_str()));
		const auto key = [](const fs::directory_entry& entry)
		{
			auto name = entry.path().filename().native();
			CharUpperBuffW(name.data(), static_cast<DWORD>(name.size()));
			return name;
		};
		std::ranges::stable_sort(entries, {}, key);
		return entries;
	}

	bool IsDirectory(const fs::directory_entry& entry)
	{
		std::error_code error;
		return entry.is_directory(error);
	}

	bool HasExtension(const fs::path& path, std::wstring_view extension)
	{
		const auto pathExtension = path.extension().native();
		return CompareStringOrdinal(pathExtension.data(), static_cast<int>(pathExtension.size()), extension.data(),
			static_cast<int>(extension.size()), TRUE) == CSTR_EQUAL;
	}

	void CollectKFs(const fs::path& directory, std::vector<fs::path>& kfPaths)
	{
		for (const auto& entry : GetSortedEntries(directory))
		{
			if (IsDirectory(entry))
				CollectKFs(entry.path(), kfPaths);
			else if (HasExtension(entry.path(), L".kf"))
				kfPaths.push_back(entry.path());
		}
	}

	// The KFs of the folder's _male and _1stperson subfolders. folder starts with Data\Meshes.
	const std::vector<FolderFile>& GetLooseFiles(const fs::path& folder, FolderCache& cache)
	{
		const auto [iter, isNew] = cache.looseFiles.try_emplace(ToAnsi(folder));
		auto& files = iter->second;
		if (!isNew)
			return files;
		for (const auto& entry : GetSortedEntries(folder))
		{
			if (!IsDirectory(entry))
				continue;
			const auto name = entry.path().filename().native();
			const bool firstPerson = _wcsicmp(name.c_str(), L"_1stperson") == 0;
			if (!firstPerson && _wcsicmp(name.c_str(), L"_male") != 0)
				continue;
			std::vector<fs::path> kfPaths;
			CollectKFs(entry.path(), kfPaths);
			// Files directly in the folder come first. A set registered later goes above earlier ones on its group's
			// stack, so the conditional subfolder variants (mod1, hurt, male, ...) are checked before the base set. In
			// directory order a subfolder whose name sorts before the base file (_male\mod1\ before
			// _male\sneak2hrreloadp.kf) would come first and the base file would always win.
			std::ranges::stable_partition(kfPaths, [&](const fs::path& kfPath) { return kfPath.parent_path() == entry.path(); });
			for (const auto& kfPath : kfPaths)
			{
				auto path = ToAnsi(kfPath);
				if (path.size() <= kMeshes.size())
					continue;
				files.push_back({ path.substr(kMeshes.size()), firstPerson });
			}
		}
		return files;
	}

	// The KFs that the loaded archives have of the folder's _male and _1stperson subfolders and the condition folders
	// in them. Paths are as the archives list them.
	const std::vector<FolderFile>& GetArchiveFiles(const std::string& folderName, FolderCache& cache)
	{
		const auto [iter, isNew] = cache.archiveFiles.try_emplace(ToLower(folderName));
		auto& files = iter->second;
		if (!isNew)
			return files;
		const auto basePath = sv::stack_string<0x400>(R"(meshes\animgroupoverride\%s\)", iter->first.c_str());
		const auto addList = [&](const char* subfolder, const char* childFolder, bool firstPerson)
		{
			auto path = sv::stack_string<0x400>("%s%s%s", basePath.c_str(), subfolder, childFolder);
			const auto list = ArchiveManager::GetDirectoryAnimPaths(path.str());
			for (const char* archivePath : list)
			{
				if (sv::starts_with_ci(archivePath, path.str()))
					files.push_back({ archivePath, firstPerson });
			}
		};
		for (const auto& [subfolder, firstPerson] : { std::pair{ "_1stperson\\", true }, std::pair{ "_male\\", false } })
		{
			addList(subfolder, "", firstPerson);
			for (const auto* childFolder : { "mod1\\", "mod2\\", "mod3\\", "hurt\\", "human\\", "male\\", "female\\" })
				addList(subfolder, childFolder, firstPerson);
		}
		return files;
	}

	size_t Register(const Overrides::Source& source, const std::vector<FolderFile>& folderFiles)
	{
		if (folderFiles.empty())
			return 0;
		std::vector<Overrides::File> files;
		files.reserve(folderFiles.size());
		for (const auto& file : folderFiles)
			files.push_back({ file.path, file.firstPerson });
		return Overrides::Register(source, files);
	}

	Overrides::Source GetSource(UInt32 identifier, bool isModIndex, const JSONEntry* jsonEntry, const OverrideOrigin* origin)
	{
		Overrides::Source source{ .identifier = identifier, .isModIndex = isModIndex, .keepCondition = true, .origin = origin };
		if (jsonEntry)
		{
			source.conditionText = jsonEntry->condition;
			source.pollCondition = jsonEntry->pollCondition;
			source.matchBaseGroupId = jsonEntry->matchBaseGroupId;
		}
		return source;
	}

	// Registers what register_ finds for the form, or for each form of a form list.
	template <typename F>
	size_t ForEachForm(const TESForm* form, F&& register_)
	{
		const auto* list = DYNAMIC_CAST(form, TESForm, BGSListForm);
		if (!list)
			return register_(form->refID);
		size_t numFiles = 0;
		for (auto iter = list->list.Begin(); !iter.End(); ++iter)
		{
			if (*iter)
				numFiles += ForEachForm(*iter, register_);
		}
		return numFiles;
	}

	void LoadModAnimPaths(const fs::path& path, const ModInfo* mod, FolderCache& cache)
	{
		const auto modFolder = ToAnsi(path.filename());
		const auto* modOrigin = Overrides::GetOrigin(OverrideOrigin::Kind::ModFolder, modFolder);
		Register(GetSource(mod->modIndex, true, nullptr, modOrigin), GetLooseFiles(path, cache));
		for (const auto& entry : GetSortedEntries(path))
		{
			if (!IsDirectory(entry))
				continue;
			const auto folderName = ToAnsi(entry.path().filename());
			if (folderName.empty() || folderName[0] == '_') // _1stperson, _male
				continue;
			const auto id = HexStringToInt(folderName);
			if (id == -1)
			{
				const auto problem = "Failed to convert " + folderName + " to a form ID";
				ERROR_LOG(problem);
				Overrides::ReportProblem(Overrides::Problem::Kind::Files, problem);
				continue;
			}
			const auto formId = (id & 0x00FFFFFF) + (mod->modIndex << 24);
			const auto* form = LookupFormByID(formId);
			if (!form)
			{
				const auto problem = FormatString("%s\\%s: form %X not found", modFolder.c_str(), folderName.c_str(), formId);
				ERROR_LOG(problem);
				Overrides::ReportProblem(Overrides::Problem::Kind::Files, problem);
				continue;
			}
			const auto* formOrigin = Overrides::GetOrigin(OverrideOrigin::Kind::ModFolder, modFolder + '\\' + folderName);
			ForEachForm(form, [&](UInt32 identifier)
			{
				return Register(GetSource(identifier, false, nullptr, formOrigin), GetLooseFiles(entry.path(), cache));
			});
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
		const auto pathString = ToAnsi(path);
		LOG("\nReading from JSON file " + pathString);
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
						LOG(FormatString("JSON %s: mod %s is not loaded, skipping folder %s", pathString.c_str(), modName.c_str(),
							elem.value("folder", std::string()).c_str()));
						continue;
					}
					auto folder = elem["folder"].get<std::string>();
					ra::replace(folder, '/', '\\');
					// Relative to AnimGroupOverride. A path joined with one that starts at the root would resolve to the root.
					folder.erase(0, folder.find_first_not_of('\\'));
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
							const auto localFormId = formId & 0x00FFFFFF;
							formId = (mod->modIndex << 24) + localFormId;
							auto* form = LookupFormByID(formId);
							if (!form)
							{
								LOG(FormatString("JSON %s: form %06X of %s (%08X in the load order) was not found, skipping folder %s",
									pathString.c_str(), localFormId, modName.c_str(), formId, folder.c_str()));
								continue;
							}
							jsonEntries.push_back({ folder, form, condition, priority, pollCondition, matchBaseGroupId, ToAnsi(path.filename()) });
						}
					}
					else
					{
						jsonEntries.push_back({ folder, nullptr, condition, priority, pollCondition, matchBaseGroupId, ToAnsi(path.filename()) });
					}
				}
			}
			else
			{
				ERROR_LOG(pathString + " does not start as a JSON array");
				Overrides::ReportProblem(Overrides::Problem::Kind::Files, pathString + ": not a JSON array");
			}
		}
		catch (const nlohmann::json::exception& e)
		{
			ERROR_LOG("The JSON is incorrectly formatted! It will not be applied. Path: " + pathString);
			ERROR_LOG(FormatString("JSON error: %s\n", e.what()));
			Overrides::ReportProblem(Overrides::Problem::Kind::Files, FormatString("%s: %s", pathString.c_str(), e.what()));
		}
	}

	// The KFs under meshes\animgroupoverride of a BSA that lies in the AnimGroupOverride folder.
	void LoadAnimPathsFromBSA(const fs::path& path, std::vector<std::string>& animPaths)
	{
		const auto pathString = ToAnsi(path);
		const auto* archive = ArchiveManager::OpenArchive(pathString.c_str(), ARCHIVE_TYPE_MESHES, false);
		if (!archive)
		{
			ERROR_LOG("Failed to open archive " + pathString);
			return;
		}
		const char* dirStrings = archive->pDirectoryStringArray;
		const char* fileStrings = archive->pFileNameStringArray;
		if (!dirStrings || !fileStrings)
			return;

		for (UInt32 i = 0; i < archive->kArchive.uiDirectories; ++i)
		{
			const size_t dirOffset = archive->pDirectoryStringOffsets[i];
			const std::string_view directory(dirStrings + dirOffset);
			if (!directory.starts_with("meshes\\animgroupoverride\\"))
				continue;
			for (UInt32 j = 0; j < archive->kArchive.pDirectories[i].uiFiles; ++j)
			{
				const size_t fileOffset = archive->pFileNameStringOffsets[i][j];
				const std::string_view fileName(fileStrings + fileOffset);
				if (sv::get_file_extension(fileName) != ".kf")
					continue;
				animPaths.push_back(std::string(directory) + '\\' + std::string(fileName));
			}
		}
	}

	// A JSON entry's animations: those of the BSAs in the AnimGroupOverride folder, of the loaded archives and the
	// loose files. Each is registered as its own batch in that order, so loose files end up on top.
	void LoadJsonEntry(const JSONEntry& entry, const std::vector<std::string>& bsaAnimPaths, FolderCache& cache)
	{
		if (entry.form)
			LOG(FormatString("JSON: Loading animations for form %X in path %s", entry.form->refID, entry.folderName.c_str()));
		else
			LOG("JSON: Loading animations for global override in path " + entry.folderName);

		sv::stack_string<0x400> archiveFolder(R"(meshes\animgroupoverride\%s\)", entry.folderName.c_str());
		std::vector<FolderFile> bsaFiles;
		for (const auto& bsaPath : bsaAnimPaths)
		{
			if (!sv::starts_with_ci(bsaPath, archiveFolder.str()))
				continue;
			const auto firstPerson = sv::contains_ci(bsaPath, "_1stperson");
			if (firstPerson || sv::contains_ci(bsaPath, "_male"))
				bsaFiles.push_back({ bsaPath, firstPerson });
		}
		const auto folderPath = fs::path(R"(Data\Meshes\AnimGroupOverride)") / entry.folderName;
		std::error_code error;
		const bool hasLooseFiles = fs::exists(folderPath, error);
		const auto* origin = Overrides::GetOrigin(OverrideOrigin::Kind::Json, entry.folderName, entry.jsonFile, entry.loadPriority);
		const auto registerFor = [&](UInt32 identifier, bool isModIndex)
		{
			const auto source = GetSource(identifier, isModIndex, &entry, origin);
			auto numFiles = Register(source, bsaFiles);
			numFiles += Register(source, GetArchiveFiles(entry.folderName, cache));
			if (hasLooseFiles)
				numFiles += Register(source, GetLooseFiles(folderPath, cache));
			return numFiles;
		};
		if (!entry.form)
			registerFor(Overrides::kEveryone, true);
		else
			ForEachForm(entry.form, [&](UInt32 identifier) { return registerFor(identifier, false); });
	}

	std::mutex s_loadMutex;
	std::thread s_loadThread;
	std::atomic<bool> s_loadFinished = false;

	// A thread that is still joinable when the DLL's statics are destroyed (the game closed before anything looked up an
	// animation) would end the process through std::terminate.
	struct LoadThreadDetacher
	{
		~LoadThreadDetacher()
		{
			if (s_loadThread.joinable())
				s_loadThread.detach();
		}
	} s_loadThreadDetacher;
}

void LoadFileAnimPaths()
{
	ScopedTimer timer("Loaded AnimGroupOverride");
	LOG("Loading file anims");
	// Before the first set is made: each set takes the user's choices for its files when it is created.
	OverridePrefs::Load();

	const fs::path dir = R"(Data\Meshes\AnimGroupOverride)";
	std::error_code error;
	if (!fs::exists(dir, error))
	{
		LOG(ToAnsi(dir) + " does not exist.");
		return;
	}
	FolderCache cache;
	std::vector<std::string> bsaAnimPaths;
	std::vector<JSONEntry> jsonEntries;
	// A folder or file that fails does not stop the others.
	const auto guarded = [](const std::string& what, auto&& f)
	{
		try
		{
			f();
		}
		catch (const std::exception& e)
		{
			const auto problem = FormatString("AnimGroupOverride Error: %s: %s", what.c_str(), e.what());
			ERROR_LOG(problem);
			Overrides::ReportProblem(Overrides::Problem::Kind::Files, FormatString("%s: %s", what.c_str(), e.what()));
		}
	};
	for (const auto& entry : GetSortedEntries(dir))
	{
		const auto& path = entry.path();
		guarded(ToAnsi(path), [&]
		{
			if (IsDirectory(entry))
			{
				if (HasExtension(path, L".esp") || HasExtension(path, L".esm"))
				{
					const auto fileName = ToAnsi(path.filename());
					if (const auto* mod = DataHandler::Get()->LookupModByName(fileName.c_str()))
						LoadModAnimPaths(path, mod, cache);
					else
						ERROR_LOG(FormatString("Mod with name %s is not loaded!", fileName.c_str()));
				}
			}
			else if (HasExtension(path, L".json"))
				HandleJson(path, jsonEntries);
			else if (HasExtension(path, L".bsa"))
				LoadAnimPathsFromBSA(path, bsaAnimPaths);
		});
	}
	// Entries of the same priority keep the order they were read in. A sort that is not stable would leave the winner
	// to the implementation.
	std::ranges::stable_sort(jsonEntries, {}, &JSONEntry::loadPriority);
	for (const auto& entry : jsonEntries)
		guarded(entry.folderName, [&] { LoadJsonEntry(entry, bsaAnimPaths, cache); });
	// Apply the user's order of the origins, once they all exist.
	Overrides::ApplyPrefs();
}

std::atomic<bool> FileAnimations::g_loaded = false;

void FileAnimations::StartLoading()
{
	s_loadThread = std::thread([]
	{
		try
		{
			LoadFileAnimPaths();
		}
		catch (const std::exception& e)
		{
			ERROR_LOG(FormatString("AnimGroupOverride Error: loading stopped: %s", e.what()));
		}
		catch (...)
		{
			ERROR_LOG("AnimGroupOverride Error: loading stopped");
		}
		s_loadFinished.store(true, std::memory_order_release);
	});
}

void FileAnimations::WaitUntilLoaded()
{
	std::scoped_lock lock(s_loadMutex);
	if (s_loadThread.joinable())
		s_loadThread.join();
	g_loaded.store(true, std::memory_order_release);
}

bool FileAnimations::CheckLoaded()
{
	if (IsLoaded())
		return true;
	if (!s_loadFinished.load(std::memory_order_acquire))
		return false;
	WaitUntilLoaded();
	return true;
}
