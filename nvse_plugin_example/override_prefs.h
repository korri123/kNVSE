#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The user's choices about AnimGroupOverride animations, made in the kNVSE menu and kept in
// Data\NVSE\Plugins\kNVSE_overrides.json: origins (OverrideOrigin: a folder, a script's mod) and files that are
// disabled, the default order of the origins, and per form (what a set is registered for: a weapon, an actor, a race,
// a mod's actors, everyone; by Overrides::FormKey) the order of the origins that animate it and the ones disabled for
// it. Kept apart from what scripts enable and disable (SavedAnims::disabled), which a mod's script may do every frame.
// The registry applies them (Overrides::ApplyPrefs, and to every set it makes).
// Any thread; one lock of its own, never held while it calls out
namespace OverridePrefs
{
	// The choices for one form
	struct FormPrefs
	{
		// origin names, lower case, the one tried first first. Empty while the user has not ordered them for the form:
		// the default order (Prefs::order) decides
		std::vector<std::string> order;
		// origin names, lower case
		std::unordered_set<std::string> disabledOrigins;

		bool IsEmpty() const { return order.empty() && disabledOrigins.empty(); }
	};

	struct Prefs
	{
		// lower case
		std::unordered_set<std::string> disabledOrigins;
		// lower case, relative to Data\Meshes
		std::unordered_set<std::string> disabledFiles;
		// origin names, lower case, the one tried first first. Empty while the user has not ordered them
		std::vector<std::string> order;
		// by Overrides::FormKey; no empty entries
		std::unordered_map<std::string, FormPrefs> forms;
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
	// Whether any form has choices of its own: until then nothing needs a form's key
	bool HasFormPrefs();
	// The choices for the form (Overrides::FormKey); empty if it has none
	FormPrefs GetForm(std::string_view formKey);
	// The rank of an origin in an order (higher: tried first): above every origin in it when it is not, and equal for
	// all while the order is empty
	int GetRank(const std::vector<std::string>& order, std::string_view name);

	void SetOriginDisabled(std::string_view name, bool disabled);
	void SetFileDisabled(std::string_view path, bool disabled);
	// The origins' names, the one tried first first. Empty: the order they were registered in
	void SetOrder(std::vector<std::string> order);
	// The same for one form. Empty: the default order
	void SetFormOrder(std::string_view formKey, std::vector<std::string> order);
	void SetFormOriginDisabled(std::string_view formKey, std::string_view name, bool disabled);
	// Drops the form's choices: the default order, every origin enabled for it
	void ResetForm(std::string_view formKey);
}
