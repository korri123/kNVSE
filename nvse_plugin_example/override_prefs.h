#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// The user's choices about AnimGroupOverride animations, made in the kNVSE menu and kept in
// Data\NVSE\Plugins\kNVSE_overrides.json: origins (OverrideOrigin: a folder, a script's mod) and files that are
// disabled, and the order of the origins. Kept apart from what scripts enable and disable (SavedAnims::disabled), which
// a mod's script may do every frame. The registry applies them (Overrides::ApplyPrefs, and to every set it makes).
// Any thread; one lock of its own, never held while it calls out
namespace OverridePrefs
{
	struct Prefs
	{
		// lower case
		std::unordered_set<std::string> disabledOrigins;
		// lower case, relative to Data\Meshes
		std::unordered_set<std::string> disabledFiles;
		// origin names, lower case, the one tried first first. Empty while the user has not ordered them
		std::vector<std::string> order;
	};

	// Reads the file, unless choices were made since the last Save (they are newer). Before the files are registered
	void Load();
	// Writes the file. Whether it succeeded
	bool Save();
	// Whether anything changed since the last Load or Save
	bool HasChanges();

	Prefs Get();
	bool IsOriginDisabled(std::string_view name);
	bool IsFileDisabled(std::string_view path);
	// The rank of an origin (higher: tried first): from the order, above every origin in it when it is not, and equal
	// for all while there is no order
	int GetRank(const Prefs& prefs, std::string_view name);

	void SetOriginDisabled(std::string_view name, bool disabled);
	void SetFileDisabled(std::string_view path, bool disabled);
	// The origins' names, the one tried first first. Empty: the order they were registered in
	void SetOrder(std::vector<std::string> order);
}
