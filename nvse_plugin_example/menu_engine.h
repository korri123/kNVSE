#pragma once

#include "GameTiles.h"

// The game's XML menu system, as far as kNVSE's menu (knvse_menu.h) needs it: tiles, a Menu to derive from, the hooks
// that let the game create a menu of a new ID and add an option to the Settings list of the start / pause menu.
// Main thread only, like the game's interface.
namespace MenuEngine
{
	// The lowest menu ID (MessageMenu); IDs up to 1084 index the game's menu arrays
	constexpr UInt32 kMinMenuId = 1001;

	// on a null tile (a tile the XML lacks) they do nothing
	void SetFloat(Tile* tile, UInt32 trait, float value);
	void SetString(Tile* tile, UInt32 trait, const char* text);
	float GetFloat(Tile* tile, UInt32 trait);
	// The ID of a trait: a user trait (a name starting with '_') exists once XML that uses it was read
	UInt32 TraitId(const char* name);
	// A child tile by name, any depth: "Pane\Rows"
	Tile* FindTile(Tile* parent, const char* path);

	// Reads a menu's XML into the interface. The tile of the menu it describes, or null
	Tile* ReadMenuXML(const char* path);
	// The tile of the open menu of that ID, or null
	Tile* GetMenuTile(UInt32 menuId);
	// The mouse wheel's movement this frame: below 0 towards the user
	int GetMouseWheel();
	bool IsEscapePressed();
	// Makes "&name;" in menu XML stand for the value (a menu's class)
	void RegisterEntity(const char* name, UInt32 value);
	// The depth a menu needs to be drawn above every open one
	float GetMaxDepth();
	// Shows the Settings list of the start / pause menu
	void ShowSettingsList();

	// The game's Menu (0x28 bytes): a class deriving from it is one the game can create, update and destroy. Its
	// virtual functions are the game's vtable, in order; Destructor is the game's (called with true when the menu
	// closes) and must destroy the derived object
	class Menu
	{
	public:
		Menu();

		virtual void Destructor(bool free) = 0;
		virtual void SetTile(UInt32 tileId, Tile* tile) {}
		virtual void HandleLeftClickPress(UInt32 tileId, Tile* tile) {}
		virtual void HandleClick(SInt32 tileId, Tile* tile) {}
		virtual void HandleMouseover(UInt32 tileId, Tile* tile) {}
		virtual void HandleUnmouseover(UInt32 tileId, Tile* tile) {}
		virtual void PostDragTileChange(UInt32 tileId, Tile* newTile, Tile* tile) {}
		virtual void PreDragTileChange(UInt32 tileId, Tile* oldTile, Tile* tile) {}
		virtual void HandleActiveMenuClickHeld(UInt32 tileId, Tile* tile) {}
		virtual void OnClickHeld(UInt32 tileId, Tile* tile) {}
		virtual void HandleMousewheel(UInt32 tileId, Tile* tile) {}
		virtual void Update() {}
		// typed characters (and backspace, arrows, ... as 0x8000000X). Whether it was used
		virtual bool HandleKeyboardInput(UInt32 inputChar) { return false; }
		virtual UInt32 GetID() = 0;
		// the menu keys: arrows, Enter, Tab, page up / down (SpecialKey). Whether it was used
		virtual bool HandleSpecialKeyInput(int key, float keyState) { return false; }
		virtual bool HandleControllerInput(int input, Tile* tile) { return false; }
		virtual void OnUpdateUserTrait(int trait) {}
		virtual void HandleControllerConnectOrDisconnect(bool connected) {}

		// The menu's tile from ReadMenuXML: shows it
		void SetMenuTile(Tile* tile);
		void PrepForVisibility(bool stayHidden);
		// A new tile under parent, from one of the menu's XML templates
		Tile* AddTileFromTemplate(Tile* parent, const char* templateName);
		// Fades the menu out; the game destroys it afterwards (Destructor)
		void Close();

	protected:
		// The game's destructor of the base: from Destructor, after the derived object was destroyed
		static void DestroyBase(Menu* menu);

	public:
		TileMenu* rootTile; // 04
		UInt32 templates[2]; // 08 tList of the XML templates
		UInt32 unk10;
		UInt32 unk14;
		UInt32 unk18;
		UInt8 byte1C;
		UInt8 byte1D;
		UInt8 byte1E;
		UInt8 byte1F;
		UInt32 id; // 20
		UInt32 fadeState; // 24
	};
	static_assert(sizeof(Menu) == 0x28);

	// HandleSpecialKeyInput
	enum SpecialKey : int
	{
		kKey_Enter = -2,
		kKey_Up = 1,
		kKey_Down = 2,
		kKey_Right = 3,
		kKey_Left = 4,
		kKey_Space = 9,
		kKey_Tab = 10,
		kKey_ShiftEnter = 11,
		kKey_AltEnter = 12,
		kKey_ShiftLeft = 13,
		kKey_ShiftRight = 14,
		kKey_PageUp = 15,
		kKey_PageDown = 16,
	};

	// HandleKeyboardInput
	constexpr UInt32 kInput_Backspace = 0x80000000;
	constexpr UInt32 kInput_Delete = 0x80000007;

	// Writes the hooks: menuId is created by create(), and an option named optionName in the Settings list calls
	// onOption. After every plugin's load (Stewie's Tweaks patches the same calls in its: these call what they find),
	// before the start menu builds its lists (it does before kMessage_DeferredInit)
	void WriteHooks(UInt32 menuId, Menu* (*create)(), const char* optionName, void (*onOption)(), bool (*isOpen)());
}
