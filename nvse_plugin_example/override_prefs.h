#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The user's choices about AnimGroupOverride animations, made in the kNVSE menu and stored in
// Data\NVSE\Plugins\kNVSE_overrides.json:
//  - origins (OverrideOrigin: a folder, a script's mod) and files that are disabled, and the default order of origins
//  - per form (what a set is registered for: a weapon, an actor, a race, a mod's actors or everyone, identified by
//    Overrides::FormKey): the order of the origins that animate it and the origins disabled for it
// Kept separate from what scripts enable and disable (SavedAnims::disabled), which a mod's script may change every
// frame. The registry applies these choices (Overrides::ApplyPrefs, and to every set it creates).
// Callable from any thread. Uses its own lock, which is never held while calling out.
namespace OverridePrefs
{
	// The choices for one form.
	struct FormPrefs
	{
		// Origin names, lower case, the one tried first listed first. Empty if the user has not ordered the origins for
		// this form, in which case the default order (Prefs::order) applies.
		std::vector<std::string> order;
		// Origin names, lower case.
		std::unordered_set<std::string> disabledOrigins;

		bool IsEmpty() const { return order.empty() && disabledOrigins.empty(); }
	};

	struct Prefs
	{
		// Origin names, lower case.
		std::unordered_set<std::string> disabledOrigins;
		// Lower case, relative to Data\Meshes.
		std::unordered_set<std::string> disabledFiles;
		// Origin names, lower case, the one tried first listed first. Empty if the user has not ordered them.
		std::vector<std::string> order;
		// Keyed by Overrides::FormKey. No empty entries.
		std::unordered_map<std::string, FormPrefs> forms;
	};

	// Reads the file, unless choices were made since the last Save (they are newer). Call before the files are registered.
	void Load();
	// Writes the file. Returns true on success.
	bool Save();
	// True if anything changed since the last Load or Save.
	bool HasChanges();

	Prefs Get();
	bool IsOriginDisabled(std::string_view name);
	bool IsFileDisabled(std::string_view path);
	// True if any form has choices of its own. Until then nothing needs a form's key.
	bool HasFormPrefs();
	// The choices for the form (Overrides::FormKey), or an empty FormPrefs if it has none.
	FormPrefs GetForm(std::string_view formKey);
	// The rank of an origin in an order (higher is tried first). An origin not in the order ranks above every origin in
	// it. All origins rank equal while the order is empty.
	int GetRank(const std::vector<std::string>& order, std::string_view name);

	void SetOriginDisabled(std::string_view name, bool disabled);
	void SetFileDisabled(std::string_view path, bool disabled);
	// Sets the origins' order, the one tried first listed first. Empty means the order they were registered in.
	void SetOrder(std::vector<std::string> order);
	// Same for one form. Empty means the default order.
	void SetFormOrder(std::string_view formKey, std::vector<std::string> order);
	void SetFormOriginDisabled(std::string_view formKey, std::string_view name, bool disabled);
	// Drops the form's choices: default order, every origin enabled for it.
	void ResetForm(std::string_view formKey);
}
