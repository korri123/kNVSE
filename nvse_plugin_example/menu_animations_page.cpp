#include <algorithm>
#include <format>
#include <map>
#include <tuple>

#include "anim_overrides.h"
#include "commands_animation.h"
#include "GameData.h"
#include "GameObjects.h"
#include "menu_page.h"
#include "override_prefs.h"
#include "string_view_util.h"
#include "utility.h"

namespace KnvseMenu
{
	std::string DescribeIdentifier(UInt32 identifier, bool isModIndex, bool brief)
	{
		if (isModIndex)
		{
			if (identifier == Overrides::kEveryone)
				return "everyone";
			const char* modName = DataHandler::Get()->GetNthModName(identifier);
			return std::format("{}{}", modName ? modName : "?", brief ? "" : " (all actors)");
		}
		auto* form = LookupFormByID(identifier);
		if (!form)
			return std::format("{:08X} (not loaded)", identifier);
		const char* name = form->GetTheName();
		if (!name || !*name)
			name = form->GetName();
		if (brief)
			return name && *name ? name : std::format("{:08X}", identifier);
		const char* kind;
		switch (form->typeID)
		{
		case kFormType_TESObjectWEAP: kind = "weapon"; break;
		case kFormType_TESRace: kind = "race"; break;
		case kFormType_TESNPC: kind = "NPC"; break;
		case kFormType_TESCreature: kind = "creature"; break;
		case kFormType_Character: case kFormType_Creature: kind = "actor"; break;
		default: kind = "form"; break;
		}
		return std::format("{} {} {:08X}", kind, name && *name ? name : "", identifier);
	}

	std::string DescribeOrigin(const OverrideOrigin& origin)
	{
		switch (origin.kind)
		{
		case OverrideOrigin::Kind::ModFolder:
			return std::format(R"(AnimGroupOverride\{})", origin.name);
		case OverrideOrigin::Kind::Json:
			return std::format(R"(AnimGroupOverride\{} ({}, priority {}))", origin.name, origin.jsonFile, origin.jsonPriority);
		case OverrideOrigin::Kind::Script:
			return std::format("Script of {}", std::string_view(origin.name).substr(std::string_view("script: ").size()));
		case OverrideOrigin::Kind::Plugin:
			return "Plugin";
		}
		return {};
	}

	std::string DescribeSet(const Overrides::SetView& view)
	{
		const auto& set = *view.set;
		const auto path = !set.anims.empty() ? set.anims[0].path : set.additiveAnimPath;
		return std::format("{} ({}, {})", sv::get_file_stem(path), view.firstPerson ? "1st" : "3rd",
			DescribeIdentifier(view.identifier, view.isModIndex, true));
	}

	namespace
	{
		// The AnimGroupOverride sets by origin, the one tried first first, and in each its sets and their files. The user
		// disables origins, sets and files and orders the origins (OverridePrefs), which the registry applies at once.
		class AnimationsPage final : public Page
		{
		public:
			const char* GetTitle() const override { return "Animations"; }

			std::string GetHeading(Side side) const override
			{
				return side == Side::Left ? "Sources" : "Animations";
			}

			void Build(const Context& context, std::vector<Row>& left, std::vector<Row>& right) override
			{
				Snapshot();
				shownOrigins.clear();
				for (size_t i = 0; i < origins.size(); ++i)
				{
					if (Matches(origins[i], context.filter))
						shownOrigins.push_back(i);
				}
				for (const auto index : shownOrigins)
				{
					const auto& entry = origins[index];
					const bool disabled = entry.origin->userDisabled.load(std::memory_order_relaxed);
					left.push_back({ .text = entry.origin->name, .value = std::format("{}", entry.sets.size()),
						.check = disabled ? Row::Check::Off : Row::Check::On, .dim = disabled });
				}
				if (shownOrigins.empty())
					left.push_back({ .text = context.filter.empty() ? "None installed" : "No matches", .dim = true });

				rightRows.clear();
				selectedOrigin = context.leftRow < static_cast<int>(shownOrigins.size()) ? &origins[shownOrigins[context.leftRow]] : nullptr;
				if (!selectedOrigin)
					return;
				const bool originDisabled = selectedOrigin->origin->userDisabled.load(std::memory_order_relaxed);
				const bool filterSets = !context.filter.empty() && !sv::contains_ci(selectedOrigin->origin->name, context.filter);
				for (size_t setIndex = 0; setIndex < selectedOrigin->sets.size(); ++setIndex)
				{
					const auto& view = selectedOrigin->sets[setIndex];
					const auto& set = *view.set;
					if (filterSets && !SetMatches(view, context.filter))
						continue;
					const auto enabledFiles = std::ranges::count_if(set.anims, [&](const AnimPath& anim) { return set.IsFileEnabled(anim); });
					const auto check = enabledFiles == static_cast<long>(set.anims.size()) ? Row::Check::On : enabledFiles ? Row::Check::Partly : Row::Check::Off;
					const bool playable = !originDisabled && set.IsEnabled();
					right.push_back({ .text = DescribeSet(view), .value = view.stackSize > 1 ? std::format("{} of {}", view.stackPosition + 1, view.stackSize) : "",
						.check = set.anims.empty() ? Row::Check::None : check, .dim = !playable });
					rightRows.push_back({ setIndex, -1 });
					for (size_t fileIndex = 0; fileIndex < set.anims.size(); ++fileIndex)
					{
						const auto& anim = set.anims[fileIndex];
						const bool enabled = set.IsFileEnabled(anim);
						right.push_back({ .text = std::string(RelativePath(anim.path)), .value = FileTags(anim),
							.check = enabled ? Row::Check::On : Row::Check::Off, .dim = !playable || !enabled, .indent = 1 });
						rightRows.push_back({ setIndex, static_cast<int>(fileIndex) });
					}
					if (!set.additiveAnimPath.empty())
					{
						right.push_back({ .text = std::string(RelativePath(set.additiveAnimPath)), .value = "additive", .dim = !playable, .indent = 1 });
						rightRows.push_back({ setIndex, -2 });
					}
				}
			}

			std::string Describe(Side side, int row) const override
			{
				if (side == Side::Left)
				{
					if (row >= static_cast<int>(shownOrigins.size()))
						return {};
					const auto& entry = origins[shownOrigins[row]];
					size_t numFiles = 0;
					for (const auto& view : entry.sets)
						numFiles += view.set->anims.size();
					return std::format("{}\n{} sets, {} files", DescribeOrigin(*entry.origin), entry.sets.size(), numFiles);
				}
				const auto [setIndex, fileIndex] = rightRows[row];
				const auto& view = selectedOrigin->sets[setIndex];
				const auto& set = *view.set;
				if (fileIndex >= 0)
					return std::format("{}\n{}", set.anims[fileIndex].path, DescribeFileUse(set));
				if (fileIndex == -2)
					return std::format("{}\nAdditive", set.additiveAnimPath);
				return DescribeSetDetails(view);
			}

			std::string GetHelp() const override
			{
				return "Enter: toggle   Home/End: move";
			}

			std::string GetButton(int button) const override
			{
				switch (button)
				{
				case 0: return "Move up";
				case 1: return "Move down";
				case 2: return "Reset order";
				case 3: return "Reload files";
				default: return {};
				}
			}

			bool Activate(Side side, int row) override
			{
				if (side == Side::Left)
				{
					if (row >= static_cast<int>(shownOrigins.size()))
						return false;
					const auto& origin = *origins[shownOrigins[row]].origin;
					OverridePrefs::SetOriginDisabled(origin.name, !origin.userDisabled.load(std::memory_order_relaxed));
					return Apply();
				}
				const auto [setIndex, fileIndex] = rightRows[row];
				const auto& set = *selectedOrigin->sets[setIndex].set;
				if (fileIndex == -1)
				{
					// all on, else all off
					const bool allEnabled = std::ranges::all_of(set.anims, [&](const AnimPath& anim) { return set.IsFileEnabled(anim); });
					for (const auto& anim : set.anims)
						SetFileEnabled(anim.path, !allEnabled);
				}
				else if (fileIndex >= 0)
				{
					const auto& anim = set.anims[fileIndex];
					SetFileEnabled(anim.path, !set.IsFileEnabled(anim));
				}
				else
					return false;
				return Apply();
			}

			bool Press(int button, Side side, int row, int& selectedRow) override
			{
				switch (button)
				{
				case 0:
				case 1:
					if (side != Side::Left || row >= static_cast<int>(shownOrigins.size()))
						return false;
					return Move(row, button == 0 ? -1 : 1, selectedRow);
				case 2:
					OverridePrefs::SetOrder({});
					return Apply();
				case 3:
					if (!g_thePlayer || !g_thePlayer->baseProcess)
						return false;
					ReloadOverrideFiles();
					generation = 0;
					return true;
				default:
					return false;
				}
			}

			void Close() override
			{
				if (!OverridePrefs::HasChanges())
					return;
				OverridePrefs::Save();
				// the animations playing now come from the sets as they were
				if (g_thePlayer && g_thePlayer->parentCell && g_thePlayer->baseProcess)
					g_thePlayer->RestartAnims();
			}

		private:
			struct OriginEntry
			{
				const OverrideOrigin* origin;
				// the latest registration among its sets: the order the origins of one rank are tried in
				UInt32 latest = 0;
				std::vector<Overrides::SetView> sets;
			};

			// a row of the right list: a set, one of its files (index into anims), or its additive file (-2)
			struct RightRow
			{
				size_t set;
				int file;
			};

			// what the registry has, again after it changed
			void Snapshot()
			{
				if (generation == Overrides::Generation())
					return;
				generation = Overrides::Generation();
				origins.clear();
				stacks.clear();
				std::map<const OverrideOrigin*, size_t> indices;
				Overrides::ForEachSet([&](const Overrides::SetView& view)
				{
					if (!view.set->origin)
						return;
					auto [iter, inserted] = indices.try_emplace(view.set->origin, origins.size());
					if (inserted)
						origins.push_back({ view.set->origin });
					auto& entry = origins[iter->second];
					entry.latest = (std::max)(entry.latest, view.set->sequence);
					entry.sets.push_back(view);
					stacks[{ view.identifier, view.isModIndex, view.firstPerson, view.groupId }].push_back(view);
				});
				std::ranges::sort(origins, [](const OriginEntry& a, const OriginEntry& b)
				{
					return std::tuple(a.origin->rank, a.latest) > std::tuple(b.origin->rank, b.latest);
				});
				for (auto& entry : origins)
				{
					std::ranges::sort(entry.sets, [](const Overrides::SetView& a, const Overrides::SetView& b)
					{
						const auto pathOf = [](const Overrides::SetView& view)
						{
							return !view.set->anims.empty() ? view.set->anims[0].path : view.set->additiveAnimPath;
						};
						return std::tuple(sv::get_file_stem(pathOf(a)), a.firstPerson, a.identifier) < std::tuple(sv::get_file_stem(pathOf(b)), b.firstPerson, b.identifier);
					});
				}
				for (auto& stack : stacks | std::views::values)
					std::ranges::sort(stack, {}, &Overrides::SetView::stackPosition);
			}

			bool Apply()
			{
				Overrides::ApplyPrefs();
				return true;
			}

			bool Move(int row, int direction, int& selectedRow)
			{
				std::vector<std::string> order;
				for (const auto& entry : origins)
					order.push_back(entry.origin->name);
				const auto from = static_cast<int>(shownOrigins[row]);
				// past the origins the filter hides
				const int toRow = row + direction;
				if (toRow < 0 || toRow >= static_cast<int>(shownOrigins.size()))
					return false;
				const auto to = static_cast<int>(shownOrigins[toRow]);
				const auto name = order[from];
				order.erase(order.begin() + from);
				order.insert(order.begin() + to, name);
				OverridePrefs::SetOrder(std::move(order));
				selectedRow = toRow;
				return Apply();
			}

			// A base aim or attack file goes with its up and down files (their variant is picked by the base's)
			void SetFileEnabled(std::string_view path, bool enabled)
			{
				OverridePrefs::SetFileDisabled(path, !enabled);
				const auto extension = path.rfind('.');
				if (extension == std::string_view::npos)
					return;
				for (const auto* suffix : { "up", "down" })
				{
					const auto sibling = std::format("{}{}{}", path.substr(0, extension), suffix, path.substr(extension));
					if (IsRegistered(sibling))
						OverridePrefs::SetFileDisabled(sibling, !enabled);
				}
			}

			bool IsRegistered(std::string_view path) const
			{
				for (const auto& entry : origins)
				{
					for (const auto& view : entry.sets)
					{
						if (std::ranges::any_of(view.set->anims, [&](const AnimPath& anim) { return anim.path == path; }))
							return true;
					}
				}
				return false;
			}

			// a file's path from its origin's folder on
			std::string_view RelativePath(std::string_view path) const
			{
				const auto folder = std::format(R"(animgroupoverride\{}\)", selectedOrigin->origin->name);
				if (sv::starts_with_ci(path, folder))
					return path.substr(folder.size());
				return path;
			}

			static std::string FileTags(const AnimPath& anim)
			{
				std::string tags;
				const auto add = [&](const char* tag) { tags += tags.empty() ? tag : std::format(", {}", tag); };
				if (anim.isStartAnim)
					add("start");
				if (anim.isStopAnim)
					add("stop");
				if (anim.partialReload)
					add("partial");
				if (anim.isAmmoSwap)
					add("ammo swap");
				return tags;
			}

			static std::string DescribeFileUse(const SavedAnims& set)
			{
				if (set.anims.size() == 1)
					return {};
				return std::format("{} variants, {}", set.anims.size(), set.hasOrder ? "in order" : "random");
			}

			std::string DescribeSetDetails(const Overrides::SetView& view) const
			{
				const auto& set = *view.set;
				auto text = std::format("{}, {} person", DescribeIdentifier(view.identifier, view.isModIndex), view.firstPerson ? "1st" : "3rd");
				if (!set.conditionScriptText.empty())
					text += std::format("\nCondition: {}", set.conditionScriptText);
				else if (set.conditionScript.load(std::memory_order_relaxed))
					text += "\nScript condition";
				if (set.conditionFailed.load(std::memory_order_relaxed))
					text += "\nCondition doesn't compile";
				if (set.disabled.load(std::memory_order_relaxed))
					text += "\nDisabled by a script";
				const auto iter = stacks.find({ view.identifier, view.isModIndex, view.firstPerson, view.groupId });
				if (iter != stacks.end() && iter->second.size() > 1)
				{
					text += "\nPriority:";
					for (const auto& other : iter->second)
						text += std::format("\n{}{}", other.set == view.set ? "> " : "  ", other.set->origin ? other.set->origin->name : "?");
				}
				return text;
			}

			bool Matches(const OriginEntry& entry, std::string_view filter) const
			{
				return filter.empty() || sv::contains_ci(entry.origin->name, filter)
					|| std::ranges::any_of(entry.sets, [&](const Overrides::SetView& view) { return SetMatches(view, filter); });
			}

			static bool SetMatches(const Overrides::SetView& view, std::string_view filter)
			{
				return std::ranges::any_of(view.set->anims, [&](const AnimPath& anim) { return sv::contains_ci(anim.path, filter); })
					|| sv::contains_ci(DescribeIdentifier(view.identifier, view.isModIndex), filter);
			}

			using StackKey = std::tuple<UInt32, bool, bool, UInt16>;

			UInt32 generation = 0;
			std::vector<OriginEntry> origins;
			std::map<StackKey, std::vector<Overrides::SetView>> stacks;
			std::vector<size_t> shownOrigins;
			const OriginEntry* selectedOrigin = nullptr;
			std::vector<RightRow> rightRows;
		};
	}

	std::unique_ptr<Page> MakeAnimationsPage()
	{
		return std::make_unique<AnimationsPage>();
	}
}
