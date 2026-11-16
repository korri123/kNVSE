#include "knvse_menu.h"

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <vector>

#include "GameAPI.h"
#include "menu_engine.h"
#include "menu_page.h"
#include "Utilities.h"
#include "utility.h"

namespace KnvseMenu
{
	namespace
	{
		// unused by the game (its default case of MenuManager::CreateMenuByClass), and by Stewie's Tweaks (1069)
		constexpr UInt32 kMenuId = 1072;
		constexpr auto kXmlPath = R"(Data\Menus\kNVSE\knvse_menu.xml)";
		constexpr int kNumPages = 5;
		constexpr int kNumButtons = 4;

		// <id> of the tiles that are clicked
		enum TileId : int
		{
			kId_Close = 7,
			kId_LeftPane = 4,
			kId_RightPane = 5,
			kId_Tab = 10, // 10-14
			kId_Button = 20, // 20-23
			kId_LeftRow = 100,
			kId_RightRow = 101,
			kId_Less = 102,
			kId_More = 103,
			kId_Check = 104,
		};

		struct Traits
		{
			UInt32 selected;
			UInt32 dim;
			UInt32 adjustable;
			UInt32 checkable;
			UInt32 indent;
			UInt32 active;
		};

		// One of the two lists: a fixed number of row tiles that show the rows from the scroll offset on
		struct ListView
		{
			Tile* pane = nullptr;
			Tile* heading = nullptr;
			Tile* rowParent = nullptr;
			Tile* scroll = nullptr;
			std::vector<Tile*> tiles;
			std::vector<Row> rows;
			int selected = 0;
			int offset = 0;
			float rowHeight = 1.0f;
			// the way the selection last moved: a heading is passed over that way
			int direction = 1;

			int Count() const { return static_cast<int>(rows.size()); }

			bool IsSelectable(int row) const
			{
				return row >= 0 && row < Count() && !rows[row].heading;
			}

			// the row itself, or off a heading: the next row the given way, else back
			int Resolve(int row, int step) const
			{
				if (!Count() || IsSelectable(row))
					return row;
				for (const int way : { step, -step })
				{
					int other = row;
					while (other >= 0 && other < Count() && !IsSelectable(other))
						other += way;
					if (IsSelectable(other))
						return other;
				}
				return row;
			}

			void Clamp()
			{
				selected = Resolve(std::clamp(selected, 0, (std::max)(Count() - 1, 0)), direction);
				const int visible = static_cast<int>(tiles.size());
				// the heading right above the selection stays in view
				const int top = selected > 0 && rows.size() > static_cast<size_t>(selected) && rows[selected - 1].heading ? selected - 1 : selected;
				if (top < offset)
					offset = top;
				else if (selected >= offset + visible)
					offset = selected - visible + 1;
				offset = std::clamp(offset, 0, (std::max)(Count() - visible, 0));
			}

			// the row a tile of this list shows, or -1
			int RowOf(const Tile* tile) const
			{
				const auto iter = std::ranges::find(tiles, tile);
				if (iter == tiles.end())
					return -1;
				const int row = offset + static_cast<int>(iter - tiles.begin());
				return row < Count() ? row : -1;
			}
		};

		class Menu final : public MenuEngine::Menu
		{
		public:
			static Menu* s_open;

			static MenuEngine::Menu* Create()
			{
				return new (GameHeapAlloc(sizeof(Menu))) Menu();
			}

			Menu()
			{
				id = kMenuId;
				pages[0] = MakeSettingsPage();
				pages[1] = MakeAnimationsPage();
				pages[2] = MakeSourcesPage();
				pages[3] = MakePlayingPage();
				pages[4] = MakeProblemsPage();
			}

			// The game destroys the menu once it has faded out, however it was closed: the pages keep their changes
			void Destructor(bool free) override
			{
				if (s_open == this)
					s_open = nullptr;
				for (auto& page : pages)
					page->Close();
				std::destroy_at(this);
				DestroyBase(this);
				if (free)
					GameHeapFree(this);
			}

			UInt32 GetID() override
			{
				return kMenuId;
			}

			// After the XML was read: the tiles it has, the rows of the lists
			bool Init(Tile* tile)
			{
				traits = {
					MenuEngine::TraitId("_selected"), MenuEngine::TraitId("_dim"), MenuEngine::TraitId("_adjustable"),
					MenuEngine::TraitId("_checkable"), MenuEngine::TraitId("_indent"), MenuEngine::TraitId("_active")
				};
				title = MenuEngine::FindTile(tile, R"(Frame\Title)");
				filterText = MenuEngine::FindTile(tile, R"(Frame\Filter)");
				description = MenuEngine::FindTile(tile, R"(Frame\Description)");
				help = MenuEngine::FindTile(tile, R"(Frame\Help)");
				for (int i = 0; i < kNumPages; ++i)
					tabs[i] = MenuEngine::FindTile(tile, FormatString(R"(Frame\Tab%d)", i).c_str());
				for (int i = 0; i < kNumButtons; ++i)
					buttons[i] = MenuEngine::FindTile(tile, FormatString(R"(Frame\Button%d)", i).c_str());
				const char* paneNames[] = { R"(Frame\LeftPane)", R"(Frame\RightPane)" };
				for (int side = 0; side < 2; ++side)
				{
					auto& list = lists[side];
					list.pane = MenuEngine::FindTile(tile, paneNames[side]);
					list.heading = MenuEngine::FindTile(list.pane, "Heading");
					list.rowParent = MenuEngine::FindTile(list.pane, "Rows");
					list.scroll = MenuEngine::FindTile(list.rowParent, "Scroll");
					if (!list.heading || !list.rowParent || !list.scroll)
						return false;
					const float height = MenuEngine::GetFloat(list.rowParent, Tile::kTileValue_height);
					for (int i = 0; ; ++i)
					{
						auto* row = AddTileFromTemplate(list.rowParent, "Row");
						if (!row)
							return false;
						list.rowHeight = (std::max)(MenuEngine::GetFloat(row, Tile::kTileValue_height), 1.0f);
						MenuEngine::SetFloat(row, Tile::kTileValue_id, side == 0 ? kId_LeftRow : kId_RightRow);
						MenuEngine::SetFloat(row, Tile::kTileValue_y, static_cast<float>(i) * list.rowHeight);
						// "<" and ">" do not fit in the XML
						MenuEngine::SetString(MenuEngine::FindTile(row, R"(Less\Label)"), Tile::kTileValue_string, "<");
						MenuEngine::SetString(MenuEngine::FindTile(row, R"(More\Label)"), Tile::kTileValue_string, ">");
						list.tiles.push_back(row);
						if (static_cast<float>(i + 2) * list.rowHeight > height)
							break;
					}
				}
				return title && filterText && description && help
					&& std::ranges::all_of(tabs, [](Tile* tab) { return tab != nullptr; })
					&& std::ranges::all_of(buttons, [](Tile* button) { return button != nullptr; });
			}

			void ShowPage(int page)
			{
				pageIndex = page;
				focus = Side::Left;
				filter.clear();
				for (auto& list : lists)
				{
					list.selected = 0;
					list.offset = 0;
					list.direction = 1;
				}
				Refresh();
			}

			// Builds the page's rows again and shows them
			void Refresh()
			{
				auto& page = *pages[pageIndex];
				for (auto& list : lists)
					list.rows.clear();
				page.Build({ lists[0].selected, filter }, lists[0].rows, lists[1].rows);
				// the left selection was on a heading: the right list shows what it moved to
				const int built = lists[0].selected;
				lists[0].Clamp();
				if (lists[0].selected != built)
				{
					for (auto& list : lists)
						list.rows.clear();
					page.Build({ lists[0].selected, filter }, lists[0].rows, lists[1].rows);
				}
				MenuEngine::SetString(title, Tile::kTileValue_string, FormatString("kNVSE: %s", page.GetTitle()).c_str());
				MenuEngine::SetString(filterText, Tile::kTileValue_string, filter.empty() ? "Type to filter" : FormatString("Filter: %s", filter.c_str()).c_str());
				MenuEngine::SetFloat(filterText, traits.dim, filter.empty());
				for (int i = 0; i < kNumPages; ++i)
				{
					MenuEngine::SetString(MenuEngine::FindTile(tabs[i], "Label"), Tile::kTileValue_string, pages[i]->GetTitle());
					MenuEngine::SetFloat(tabs[i], traits.active, i == pageIndex);
				}
				for (int i = 0; i < kNumButtons; ++i)
				{
					const auto label = page.GetButton(i);
					MenuEngine::SetString(MenuEngine::FindTile(buttons[i], "Label"), Tile::kTileValue_string, label.c_str());
					MenuEngine::SetFloat(buttons[i], Tile::kTileValue_visible, !label.empty());
				}
				for (int side = 0; side < 2; ++side)
				{
					auto& list = lists[side];
					list.Clamp();
					const auto heading = page.GetHeading(static_cast<Side>(side));
					MenuEngine::SetString(list.heading, Tile::kTileValue_string, heading.c_str());
					MenuEngine::SetFloat(list.pane, Tile::kTileValue_visible, !heading.empty());
					ShowRows(list, focus == static_cast<Side>(side));
				}
				MenuEngine::SetString(help, Tile::kTileValue_string, page.GetHelp().c_str());
				ShowDescription(focus, Selected().selected);
			}

			void ShowRows(ListView& list, bool focused)
			{
				for (size_t i = 0; i < list.tiles.size(); ++i)
				{
					auto* tile = list.tiles[i];
					const int index = list.offset + static_cast<int>(i);
					const bool shown = index < list.Count();
					MenuEngine::SetFloat(tile, Tile::kTileValue_visible, shown);
					if (!shown)
						continue;
					const auto& row = list.rows[index];
					const char* check = row.check == Row::Check::On ? "[x]" : row.check == Row::Check::Partly ? "[-]" : "[ ]";
					MenuEngine::SetString(MenuEngine::FindTile(tile, R"(Check\Label)"), Tile::kTileValue_string, check);
					MenuEngine::SetString(MenuEngine::FindTile(tile, "Text"), Tile::kTileValue_string, row.text.c_str());
					MenuEngine::SetString(MenuEngine::FindTile(tile, "Value"), Tile::kTileValue_string, row.value.c_str());
					MenuEngine::SetFloat(tile, traits.checkable, row.check != Row::Check::None);
					MenuEngine::SetFloat(tile, traits.adjustable, row.adjustable);
					MenuEngine::SetFloat(tile, traits.dim, row.dim);
					MenuEngine::SetFloat(tile, traits.indent, static_cast<float>(row.indent));
					MenuEngine::SetFloat(tile, traits.selected, index == list.selected ? (focused ? 2.0f : 1.0f) : 0.0f);
				}
				// the scroll bar: where the rows shown are among all
				const int visible = static_cast<int>(list.tiles.size());
				const float height = MenuEngine::GetFloat(list.rowParent, Tile::kTileValue_height);
				MenuEngine::SetFloat(list.scroll, Tile::kTileValue_visible, list.Count() > visible);
				if (list.Count() > visible)
				{
					MenuEngine::SetFloat(list.scroll, Tile::kTileValue_y, height * static_cast<float>(list.offset) / static_cast<float>(list.Count()));
					MenuEngine::SetFloat(list.scroll, Tile::kTileValue_height, (std::max)(height * static_cast<float>(visible) / static_cast<float>(list.Count()), 8.0f));
				}
			}

			void ShowDescription(Side side, int row)
			{
				const auto& list = lists[static_cast<int>(side)];
				const auto text = row >= 0 && row < list.Count() ? pages[pageIndex]->Describe(side, row) : std::string();
				MenuEngine::SetString(description, Tile::kTileValue_string, text.c_str());
			}

			ListView& Selected()
			{
				return lists[static_cast<int>(focus)];
			}

			void Select(Side side, int row, int direction = 1)
			{
				auto& list = lists[static_cast<int>(side)];
				row = list.Resolve(row, direction);
				const bool leftChanged = side == Side::Left && row != lists[0].selected;
				focus = side;
				list.selected = row;
				list.direction = direction;
				if (leftChanged)
				{
					lists[1].selected = 0;
					lists[1].offset = 0;
					lists[1].direction = 1;
				}
				Refresh();
			}

			void Act(bool changed)
			{
				if (changed)
					Refresh();
			}

			// the list and row of a row tile or of one of its children
			std::pair<Side, int> Locate(Tile* tile) const
			{
				for (; tile; tile = tile->parent)
				{
					for (int side = 0; side < 2; ++side)
					{
						if (const int row = lists[side].RowOf(tile); row >= 0)
							return { static_cast<Side>(side), row };
					}
				}
				return { Side::Left, -1 };
			}

			void HandleClick(SInt32 tileId, Tile* tile) override
			{
				auto& page = *pages[pageIndex];
				switch (tileId)
				{
				case kId_Close:
					Close();
					return;
				case kId_LeftRow:
				case kId_RightRow:
				case kId_Check:
				case kId_Less:
				case kId_More:
				{
					const auto [side, row] = Locate(tile);
					if (row < 0)
						return;
					Select(side, row);
					if (tileId == kId_Check)
						Act(page.Activate(side, row));
					else if (tileId == kId_Less || tileId == kId_More)
						Act(page.Adjust(side, row, tileId == kId_More ? 1 : -1));
					return;
				}
				default:
					break;
				}
				if (tileId >= kId_Tab && tileId < kId_Tab + kNumPages)
					ShowPage(tileId - kId_Tab);
				else if (tileId >= kId_Button && tileId < kId_Button + kNumButtons)
					PressButton(tileId - kId_Button);
			}

			void PressButton(int button)
			{
				auto& list = Selected();
				int selected = list.selected;
				if (pages[pageIndex]->Press(button, focus, list.selected, selected))
				{
					list.selected = selected;
					Refresh();
				}
			}

			void HandleMouseover(UInt32 tileId, Tile* tile) override
			{
				if (tileId == kId_LeftRow || tileId == kId_RightRow || tileId == kId_Check || tileId == kId_Less || tileId == kId_More)
				{
					const auto [side, row] = Locate(tile);
					if (row >= 0)
						ShowDescription(side, row);
				}
			}

			void HandleUnmouseover(UInt32 tileId, Tile* tile) override
			{
				ShowDescription(focus, Selected().selected);
			}

			void HandleMousewheel(UInt32 tileId, Tile* tile) override
			{
				auto [side, row] = Locate(tile);
				if (row < 0)
				{
					if (tileId == kId_LeftPane)
						side = Side::Left;
					else if (tileId == kId_RightPane)
						side = Side::Right;
					else
						return;
				}
				auto& list = lists[static_cast<int>(side)];
				const int step = MenuEngine::GetMouseWheel() > 0 ? -3 : 3;
				list.offset = std::clamp(list.offset + step, 0, (std::max)(list.Count() - static_cast<int>(list.tiles.size()), 0));
				// the selection stays where it is: Clamp would scroll back to it
				ShowRows(list, focus == side);
			}

			bool HandleSpecialKeyInput(int key, float keyState) override
			{
				auto& page = *pages[pageIndex];
				auto& list = Selected();
				const int pageStep = (std::max)(static_cast<int>(list.tiles.size()) - 1, 1);
				switch (key)
				{
				case MenuEngine::kKey_Up:
				case MenuEngine::kKey_Down:
				case MenuEngine::kKey_PageUp:
				case MenuEngine::kKey_PageDown:
				{
					const int step = key == MenuEngine::kKey_Up ? -1 : key == MenuEngine::kKey_Down ? 1 : key == MenuEngine::kKey_PageUp ? -pageStep : pageStep;
					if (list.Count())
						Select(focus, std::clamp(list.selected + step, 0, list.Count() - 1), step < 0 ? -1 : 1);
					return true;
				}
				case MenuEngine::kKey_Left:
				case MenuEngine::kKey_Right:
				{
					const int direction = key == MenuEngine::kKey_Right ? 1 : -1;
					if (list.selected < list.Count() && list.rows[list.selected].adjustable)
						Act(page.Adjust(focus, list.selected, direction));
					else if (lists[1].Count() || direction < 0)
					{
						focus = direction > 0 ? Side::Right : Side::Left;
						Refresh();
					}
					return true;
				}
				case MenuEngine::kKey_Enter:
				case MenuEngine::kKey_Space:
					if (list.selected < list.Count())
						Act(page.Activate(focus, list.selected));
					return true;
				case MenuEngine::kKey_Tab:
				case MenuEngine::kKey_ShiftRight:
					ShowPage((pageIndex + 1) % kNumPages);
					return true;
				case MenuEngine::kKey_ShiftLeft:
					ShowPage((pageIndex + kNumPages - 1) % kNumPages);
					return true;
				default:
					return false;
				}
			}

			bool HandleKeyboardInput(UInt32 input) override
			{
				switch (input)
				{
				case MenuEngine::kInput_Backspace:
					if (!filter.empty())
					{
						filter.pop_back();
						RefreshFiltered();
					}
					return true;
				case MenuEngine::kInput_Delete:
				{
					auto& list = Selected();
					if (list.selected < list.Count())
						Act(pages[pageIndex]->Reset(focus, list.selected));
					return true;
				}
				case 0x80000005: // Home
				case 0x80000006: // End
					PressButton(input == 0x80000005 ? 0 : 1);
					return true;
				default:
					break;
				}
				if (input >= 0x20 && input < 0x7F && filter.size() < 40)
				{
					filter.push_back(static_cast<char>(input));
					RefreshFiltered();
					return true;
				}
				return false;
			}

			void Update() override
			{
				if (MenuEngine::IsEscapePressed())
					Close();
			}

		private:
			void RefreshFiltered()
			{
				for (auto& list : lists)
				{
					list.selected = 0;
					list.offset = 0;
				}
				Refresh();
			}

			std::array<std::unique_ptr<Page>, kNumPages> pages;
			std::array<ListView, 2> lists;
			int pageIndex = 0;
			Side focus = Side::Left;
			std::string filter;
			Traits traits{};
			Tile* title = nullptr;
			Tile* filterText = nullptr;
			Tile* description = nullptr;
			Tile* help = nullptr;
			std::array<Tile*, kNumPages> tabs{};
			std::array<Tile*, kNumButtons> buttons{};
		};

		Menu* Menu::s_open = nullptr;

		void Open()
		{
			static bool s_registered = false;
			if (!s_registered)
			{
				s_registered = true;
				MenuEngine::RegisterEntity("&kNVSEMenu;", kMenuId);
			}
			if (auto* existing = MenuEngine::GetMenuTile(kMenuId))
				existing->Destroy(true);
			const float depth = MenuEngine::GetMaxDepth();
			auto* tile = MenuEngine::ReadMenuXML(kXmlPath);
			auto* menu = tile ? reinterpret_cast<MenuEngine::Menu*>(static_cast<TileMenu*>(tile)->menu) : nullptr;
			if (!menu || menu->GetID() != kMenuId)
			{
				Console_Print("kNVSE: the menu could not be loaded from %s", kXmlPath);
				return;
			}
			auto* knvseMenu = static_cast<Menu*>(menu);
			knvseMenu->SetMenuTile(tile);
			if (!knvseMenu->Init(tile))
			{
				Console_Print("kNVSE: %s lacks tiles the menu needs", kXmlPath);
				knvseMenu->Close();
				return;
			}
			MenuEngine::SetFloat(tile, Tile::kTileValue_depth, depth);
			Menu::s_open = knvseMenu;
			knvseMenu->ShowPage(0);
			knvseMenu->PrepForVisibility(false);
		}

		void OnSettingsOption()
		{
			Open();
			// the start menu stays on its Settings list behind it
			MenuEngine::ShowSettingsList();
		}

		bool IsOpenHook()
		{
			return Menu::s_open != nullptr;
		}
	}

	void WriteHooks()
	{
		MenuEngine::WriteHooks(kMenuId, Menu::Create, "Animation", OnSettingsOption, IsOpenHook);
	}

	bool IsOpen()
	{
		return Menu::s_open != nullptr;
	}
}
