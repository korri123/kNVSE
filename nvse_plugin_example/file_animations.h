#pragma once

#include <atomic>
#include <string_view>

// Registers the animations of Data\Meshes\AnimGroupOverride (folders named after mods, JSON files, BSAs)
void LoadFileAnimPaths();
// The string in the game's global string table (NiGlobalStringTable), never released
std::string_view AddStringToPool(std::string_view str);

// The files are registered by a thread of their own while the game goes on loading to the main menu. Whoever needs
// the override sets before it is done waits for it
namespace FileAnimations
{
	extern std::atomic<bool> g_loaded;

	inline bool IsLoaded()
	{
		return g_loaded.load(std::memory_order_acquire);
	}

	// Main thread, once the forms are loaded
	void StartLoading();
	// Any thread
	void WaitUntilLoaded();
	// Whether the files are registered, without waiting. Main thread
	bool CheckLoaded();
}
