#pragma once

#include <atomic>
#include <string_view>

// Registers the animations of Data\Meshes\AnimGroupOverride (folders named after mods, JSON files, BSAs).
void LoadFileAnimPaths();
// Returns the string in the game's global string table (NiGlobalStringTable). It is never released.
std::string_view AddStringToPool(std::string_view str);

// The files are registered by a dedicated thread while the game continues loading to the main menu. Anything that
// needs the override sets before that finishes waits for it.
namespace FileAnimations
{
	extern std::atomic<bool> g_loaded;

	inline bool IsLoaded()
	{
		return g_loaded.load(std::memory_order_acquire);
	}

	// Main thread, once the forms are loaded.
	void StartLoading();
	// Any thread.
	void WaitUntilLoaded();
	// True if the files are registered. Does not wait. Main thread.
	bool CheckLoaded();
}
