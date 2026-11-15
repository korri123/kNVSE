#include <algorithm>
#include <format>

#include "GameAPI.h"
#include "menu_page.h"
#include "settings.h"
#include "string_view_util.h"

namespace KnvseMenu
{
	namespace
	{
		// The settings of kNVSE.ini by category: changed in memory at once (Setting::apply says when they take
		// effect), saved to the INI when the menu closes
		class SettingsPage final : public Page
		{
		public:
			const char* GetTitle() const override { return "Settings"; }

			std::string GetHeading(Side side) const override
			{
				return side == Side::Left ? "Category" : "Setting";
			}

			void Build(const Context& context, std::vector<Row>& left, std::vector<Row>& right) override
			{
				// in the order of the table, those with a setting the filter matches
				categories.clear();
				for (const auto& setting : Settings::All())
				{
					if (std::ranges::find(categories, setting.category) == categories.end())
						categories.push_back(setting.category);
				}
				std::erase_if(categories, [&](const char* category)
				{
					return std::ranges::none_of(Settings::All(), [&](const Settings::Setting& setting)
					{
						return setting.category == category && Matches(setting, context.filter);
					});
				});
				for (const auto* category : categories)
					left.push_back({ .text = category });

				shown.clear();
				const auto* category = context.leftRow < static_cast<int>(categories.size()) ? categories[context.leftRow] : nullptr;
				for (const auto& setting : Settings::All())
				{
					if (setting.category != category || !Matches(setting, context.filter))
						continue;
					shown.push_back(&setting);
					Row row{ .text = setting.label, .dim = !Settings::IsRequirementMet(setting) };
					if (setting.IsBool())
						row.check = Settings::GetNumber(setting) != 0.0 ? Row::Check::On : Row::Check::Off;
					else
					{
						row.value = Settings::GetText(setting);
						row.adjustable = !setting.IsString();
					}
					if (Settings::NeedsRestart(setting))
						row.value += row.value.empty() ? "restart" : "  restart";
					right.push_back(std::move(row));
				}
			}

			std::string Describe(Side side, int row) const override
			{
				if (side == Side::Left)
					return {};
				const auto& setting = *shown[row];
				std::string text = setting.summary;
				const auto missing = MissingRequirements(setting);
				if (!missing.empty())
					text += std::format("\nRequires {}.", missing);
				switch (setting.apply)
				{
				case Settings::Apply::Live:
					break;
				case Settings::Apply::NewAnimations:
					text += "\nApplies to animations loaded after the change.";
					break;
				case Settings::Apply::RestartToEnable:
					text += "\nTurning it on needs a restart.";
					break;
				case Settings::Apply::Restart:
					text += "\nNeeds a restart.";
					break;
				}
				if (setting.IsString())
					text += "\nEdit in kNVSE.ini.";
				return text;
			}

			std::string GetHelp() const override
			{
				return "Enter: toggle   Left/Right: change   Del: default";
			}

			std::string GetButton(int button) const override
			{
				switch (button)
				{
				case 0: return "Default";
				case 1: return "All defaults";
				default: return {};
				}
			}

			bool Activate(Side side, int row) override
			{
				if (side != Side::Right || !shown[row]->IsBool())
					return false;
				Settings::SetNumber(*shown[row], Settings::GetNumber(*shown[row]) == 0.0);
				return true;
			}

			bool Adjust(Side side, int row, int direction) override
			{
				if (side != Side::Right)
					return false;
				const auto& setting = *shown[row];
				if (setting.IsBool())
					return Activate(side, row);
				if (setting.IsString())
					return false;
				const double step = setting.step > 0.0 ? setting.step : std::holds_alternative<float*>(setting.value) ? (setting.max - setting.min) / 20.0 : 1.0;
				// on the grid of steps, so that a value typed into the INI comes back to it
				const double value = Settings::GetNumber(setting);
				const double next = std::round(value / step + direction) * step;
				Settings::SetNumber(setting, next);
				return true;
			}

			bool Reset(Side side, int row) override
			{
				if (side != Side::Right)
					return false;
				Settings::ResetToDefault(*shown[row]);
				return true;
			}

			bool Press(int button, Side side, int row, int& selectedRow) override
			{
				if (button == 0)
					return Reset(side, row);
				if (button == 1)
				{
					for (const auto& setting : Settings::All())
						Settings::ResetToDefault(setting);
					return true;
				}
				return false;
			}

			void Close() override
			{
				Settings::Save();
				std::string restart;
				for (const auto& setting : Settings::All())
				{
					if (Settings::NeedsRestart(setting))
						restart += restart.empty() ? setting.label : std::format(", {}", setting.label);
				}
				if (!restart.empty())
					QueueUIMessage(std::format("kNVSE: restart to apply {}", restart).c_str(), 0, nullptr, nullptr, 4.0f, false);
			}

		private:
			static bool Matches(const Settings::Setting& setting, std::string_view filter)
			{
				return filter.empty() || sv::contains_ci(setting.label, filter) || sv::contains_ci(setting.key, filter)
					|| sv::contains_ci(setting.summary, filter);
			}

			static std::string MissingRequirements(const Settings::Setting& setting)
			{
				std::string missing;
				for (const auto* key : setting.requirements)
				{
					const auto* required = key ? Settings::Find(key) : nullptr;
					if (required && Settings::GetNumber(*required) == 0.0)
						missing += missing.empty() ? required->label : std::format(" and {}", required->label);
				}
				return missing;
			}

			std::vector<const char*> categories;
			std::vector<const Settings::Setting*> shown;
		};
	}

	std::unique_ptr<Page> MakeSettingsPage()
	{
		return std::make_unique<SettingsPage>();
	}
}
