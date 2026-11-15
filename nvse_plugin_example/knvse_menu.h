#pragma once

// kNVSE's menu: Settings > Animation in the start and pause menu. Pages: the settings of kNVSE.ini, the
// AnimGroupOverride animations (what is installed, from where, which play; the user disables and orders them:
// OverridePrefs), what plays for the player and why, and what went wrong loading them. Data\Menus\kNVSE\knvse_menu.xml
namespace KnvseMenu
{
	// At kMessage_PostPostLoad: after other plugins patched the start menu, before it builds its lists
	void WriteHooks();
	bool IsOpen();
}
