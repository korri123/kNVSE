#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "anim_overrides.h"

// A page of kNVSE's menu (knvse_menu.h): two lists side by side, the right one showing what is selected on the left,
// a description of the row selected or under the cursor, and up to four buttons. Pages build their rows from the
// state they show every time something changes, and act on rows by index.
namespace KnvseMenu
{
	enum class Side : unsigned char
	{
		Left,
		Right,
	};

	struct Row
	{
		enum class Check : signed char
		{
			None = -1,
			Off,
			On,
			Partly,
		};
		std::string text;
		// right aligned
		std::string value;
		Check check = Check::None;
		// greyed: off, not in effect, a heading
		bool dim = false;
		// Adjust changes it (arrows next to the value)
		bool adjustable = false;
		// steps the text is moved to the right
		int indent = 0;
	};

	struct Context
	{
		// the row selected in the left list
		int leftRow = 0;
		// what the user typed: rows that do not have it may be left out
		std::string_view filter;
	};

	class Page
	{
	public:
		virtual ~Page() = default;

		virtual const char* GetTitle() const = 0;
		// a list's heading, empty without the list
		virtual std::string GetHeading(Side side) const = 0;
		virtual void Build(const Context& context, std::vector<Row>& left, std::vector<Row>& right) = 0;
		// what is shown below the lists for a row of the last Build
		virtual std::string Describe(Side side, int row) const = 0;
		// what the keys do, below the description
		virtual std::string GetHelp() const = 0;
		// the label of one of the four buttons, empty without it
		virtual std::string GetButton(int button) const { return {}; }

		// Each returns whether anything changed (the rows are built again): Enter or a click on the check box,
		virtual bool Activate(Side side, int row) { return false; }
		// the arrows, or left / right on an adjustable row (direction -1 / 1),
		virtual bool Adjust(Side side, int row, int direction) { return false; }
		// a button. selectedRow: where the selection goes (a row moved up or down)
		virtual bool Press(int button, Side side, int row, int& selectedRow) { return false; }
		// Delete
		virtual bool Reset(Side side, int row) { return false; }

		// The menu closes: keep what was changed
		virtual void Close() {}
	};

	std::unique_ptr<Page> MakeSettingsPage();
	std::unique_ptr<Page> MakeAnimationsPage();
	std::unique_ptr<Page> MakePlayingPage();
	std::unique_ptr<Page> MakeProblemsPage();

	// For the text of rows: "weapon Service Rifle 0001EC17", "everyone", "<mod> (all actors)"; brief: the name only
	std::string DescribeIdentifier(UInt32 identifier, bool isModIndex, bool brief = false);
	// "AnimGroupOverride\... (x.json, priority 0)", "Script of <mod>"
	std::string DescribeOrigin(const OverrideOrigin& origin);
	// "2haaim (1st, Service Rifle)"
	std::string DescribeSet(const Overrides::SetView& view);
}
