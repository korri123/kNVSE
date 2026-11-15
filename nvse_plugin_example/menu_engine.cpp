#include "menu_engine.h"

#include <cstring>

#include "SafeWrite.h"
#include "Utilities.h"

namespace MenuEngine
{
	namespace
	{
		// InterfaceManager
		void* GetInterface()
		{
			return *reinterpret_cast<void**>(0x11D8A80);
		}

		// The start menu's option (StartMenuOption, 0x10 bytes): a line of its lists
		struct StartMenuOption
		{
			void* vtable;
			const char* text;
			void (*callback)();
			UInt32 flags;
		};
		constexpr UInt32 kVtbl_StartMenuOption = 0x1076EE0;
		// shown in the Settings list
		constexpr UInt32 kOption_Settings = 0x4;

		// MenuManager::CreateMenuByClass (0x71E420) picks a menu's class by a switch over its ID: case 37 is the
		// default ("MENUS: Unknown menu"), and the code at 0x71F142 returns the menu in eax
		constexpr UInt32 kCreateCaseTable = 0x71F154;
		constexpr UInt32 kCreateCaseIndices = 0x71F1EC;
		constexpr UInt8 kDefaultCase = 37;

		UInt32 s_menuId = 0;
		Menu* (*s_create)() = nullptr;
		UInt32 s_defaultCase = 0;

		const char* s_optionName = nullptr;
		void (*s_onOption)() = nullptr;
		bool (*s_isOpen)() = nullptr;
		UInt32 s_addSettingsOption = 0;
		UInt32 s_mainMenuIsActive = 0;

		__declspec(naked) void CreateMenuDefaultCase()
		{
			__asm
			{
				mov eax, [ebp + 8]
				cmp eax, s_menuId
				jne other
				call s_create
				push 0x71F142 // returns the menu in eax
				ret
			other:
				jmp s_defaultCase
			}
		}

		// StartMenu::SetupButtons adds the Settings option to its list (BSSimpleArray::Add 0x7CB2E0): kNVSE's goes
		// after it and whatever else was added there (Stewie's Tweaks)
		void __fastcall AddSettingsOptions(void* options, void*, StartMenuOption** settings)
		{
			ThisStdCall(s_addSettingsOption, options, settings);
			auto* option = static_cast<StartMenuOption*>(GameHeapAlloc(sizeof(StartMenuOption)));
			*option = { reinterpret_cast<void*>(kVtbl_StartMenuOption), s_optionName, s_onOption, kOption_Settings };
			ThisStdCall(0x7CB2E0, options, &option);
		}

		// InterfaceManager::Idle asks whether Escape is for the start menu's main page (StartMenu::MainMenuIsActive):
		// with kNVSE's menu open it closes that menu only, not the start menu as well
		bool MainMenuIsActiveHook()
		{
			return CdeclCall<bool>(s_mainMenuIsActive) || s_isOpen();
		}
	}

	void SetFloat(Tile* tile, UInt32 trait, float value)
	{
		if (tile)
			ThisStdCall(0xA012D0, tile, trait, value, true);
	}

	void SetString(Tile* tile, UInt32 trait, const char* text)
	{
		if (tile)
			ThisStdCall(0xA01350, tile, trait, text, true);
	}

	float GetFloat(Tile* tile, UInt32 trait)
	{
		const auto* value = tile ? tile->GetValue(trait) : nullptr;
		return value ? value->num : 0.0f;
	}

	UInt32 TraitId(const char* name)
	{
		return Tile::TraitNameToID(name);
	}

	Tile* FindTile(Tile* parent, const char* path)
	{
		if (!parent)
			return nullptr;
		char buffer[0x100];
		strcpy_s(buffer, path);
		return parent->GetComponentTile(buffer);
	}

	Tile* ReadMenuXML(const char* path)
	{
		auto* menuRoot = *reinterpret_cast<Tile**>(static_cast<char*>(GetInterface()) + 0x9C);
		return ThisStdCall<Tile*>(0xA01B00, menuRoot, path);
	}

	Tile* GetMenuTile(UInt32 menuId)
	{
		// NiTArray<TileMenu*>: the menus' tiles by ID
		auto** tiles = *reinterpret_cast<Tile***>(0x11F3508 + 4);
		return tiles[menuId - kMinMenuId];
	}

	int GetMouseWheel()
	{
		return ThisStdCall<int>(0x70EC70, GetInterface());
	}

	bool IsEscapePressed()
	{
		constexpr UInt32 kControl_Escape = 0x1C;
		constexpr UInt32 kIsPressed = 1;
		return ThisStdCall<bool>(0xA24660, *reinterpret_cast<void**>(0x11F35CC), kControl_Escape, kIsPressed);
	}

	void RegisterEntity(const char* name, UInt32 value)
	{
		CdeclCall(0x9FF8A0, name, value);
	}

	float GetMaxDepth()
	{
		return StdCall<float>(0xA1DFB0);
	}

	void ShowSettingsList()
	{
		CdeclCall(0x7D0700); // MenuButton:Settings
	}

	Menu::Menu()
	{
		ThisStdCall(0xA1C4A0, this);
	}

	void Menu::DestroyBase(Menu* menu)
	{
		ThisStdCall(0xA1C520, menu);
	}

	void Menu::SetMenuTile(Tile* tile)
	{
		ThisStdCall(0xA1DC70, this, tile, false);
	}

	void Menu::PrepForVisibility(bool stayHidden)
	{
		ThisStdCall(0xA1DC20, this, stayHidden);
	}

	Tile* Menu::AddTileFromTemplate(Tile* parent, const char* templateName)
	{
		return ThisStdCall<Tile*>(0xA1DDB0, this, parent, templateName, 0);
	}

	void Menu::Close()
	{
		// the trait Menu::StartFadeOut (0xA1D910) checks to take the menu off the stack of menus that have the focus
		SetFloat(rootTile, 6002, 1.0f);
		ThisStdCall(0xA1D910, this);
	}

	void WriteHooks(UInt32 menuId, Menu* (*create)(), const char* optionName, void (*onOption)(), bool (*isOpen)())
	{
		s_menuId = menuId;
		s_create = create;
		s_optionName = optionName;
		s_onOption = onOption;
		s_isOpen = isOpen;

		if (*reinterpret_cast<UInt8*>(kCreateCaseIndices + menuId - kMinMenuId) != kDefaultCase)
		{
			_MESSAGE("kNVSE menu: menu ID %u is taken by another plugin, the menu is not available", menuId);
			return;
		}
		const auto caseEntry = kCreateCaseTable + 4 * kDefaultCase;
		s_defaultCase = *reinterpret_cast<UInt32*>(caseEntry);
		SafeWrite32(caseEntry, reinterpret_cast<UInt32>(CreateMenuDefaultCase));

		WriteRelCall(0x7CC96A, AddSettingsOptions, &s_addSettingsOption);
		WriteRelCall(0x70E686, MainMenuIsActiveHook, &s_mainMenuIsActive);
	}
}
