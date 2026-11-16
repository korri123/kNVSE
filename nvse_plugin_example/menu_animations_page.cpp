#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <tuple>
#include <unordered_map>

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

	namespace
	{
		// What a set is registered for, as the menu groups it
		enum class Category : UInt8
		{
			Everyone,
			Weapons,
			NPCs,
			Creatures,
			Races,
			Actors,
			Mods,
			Other,
			Count,
		};

		constexpr const char* kCategoryNames[] = { "Everyone", "Weapons", "NPCs", "Creatures", "Races", "Actors", "Mods", "Other" };
		static_assert(std::size(kCategoryNames) == static_cast<size_t>(Category::Count));

		Category CategoryOf(UInt32 identifier, bool isModIndex)
		{
			if (isModIndex)
				return identifier == Overrides::kEveryone ? Category::Everyone : Category::Mods;
			const auto* form = LookupFormByID(identifier);
			if (!form)
				return Category::Other;
			switch (form->typeID)
			{
			case kFormType_TESObjectWEAP: return Category::Weapons;
			case kFormType_TESNPC: return Category::NPCs;
			case kFormType_TESCreature: return Category::Creatures;
			case kFormType_TESRace: return Category::Races;
			case kFormType_Character: case kFormType_Creature: return Category::Actors;
			default: return Category::Other;
			}
		}

		const char* DescribeFolderCondition(FolderCondition condition)
		{
			switch (condition)
			{
			case FolderCondition::Male: return "male";
			case FolderCondition::Female: return "female";
			case FolderCondition::Mod1: return "mod1";
			case FolderCondition::Mod2: return "mod2";
			case FolderCondition::Mod3: return "mod3";
			case FolderCondition::Hurt: return "hurt";
			case FolderCondition::Human: return "human";
			default: return nullptr;
			}
		}

		bool IsPlayable(const SavedAnims& set)
		{
			return set.IsEnabled() && !set.conditionFailed.load(std::memory_order_relaxed);
		}

		std::string_view PathOf(const SavedAnims& set)
		{
			return !set.anims.empty() ? set.anims[0].path : set.additiveAnimPath;
		}

		// "2haaim (1st, female)": the form is the one shown
		std::string DescribeSetBrief(const Overrides::SetView& view)
		{
			const char* folder = DescribeFolderCondition(view.set->folderCondition);
			return std::format("{} ({}{}{})", sv::get_file_stem(PathOf(*view.set)), view.firstPerson ? "1st" : "3rd", folder ? ", " : "", folder ? folder : "");
		}

		// An origin's name where little room is left: its first folder
		std::string ShortName(const OverrideOrigin* origin)
		{
			if (!origin)
				return "?";
			std::string_view name = origin->name;
			if (const auto separator = name.find('\\'); separator != std::string_view::npos && origin->kind != OverrideOrigin::Kind::Script)
				name = name.substr(0, separator);
			constexpr size_t kMaxLength = 24;
			return name.size() > kMaxLength ? std::format("{}..", name.substr(0, kMaxLength - 2)) : std::string(name);
		}

		// a file's path from its origin's folder on
		std::string_view RelativePath(const OverrideOrigin& origin, std::string_view path)
		{
			const auto folder = std::format(R"(animgroupoverride\{}\)", origin.name);
			if (sv::starts_with_ci(path, folder))
				return path.substr(folder.size());
			return path;
		}

		std::string FileTags(const AnimPath& anim)
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

		Row::Check FilesCheck(const SavedAnims& set)
		{
			if (set.anims.empty())
				return Row::Check::None;
			const auto enabledFiles = std::ranges::count_if(set.anims, [&](const AnimPath& anim) { return set.IsFileEnabled(anim); });
			return enabledFiles == static_cast<long>(set.anims.size()) ? Row::Check::On : enabledFiles ? Row::Check::Partly : Row::Check::Off;
		}

		bool SetMatches(const SavedAnims& set, std::string_view filter)
		{
			return std::ranges::any_of(set.anims, [&](const AnimPath& anim) { return sv::contains_ci(anim.path, filter); })
				|| (!set.additiveAnimPath.empty() && sv::contains_ci(set.additiveAnimPath, filter));
		}

		bool Apply()
		{
			Overrides::ApplyPrefs();
			return true;
		}

		bool ReloadFiles()
		{
			if (!g_thePlayer || !g_thePlayer->baseProcess)
				return false;
			ReloadOverrideFiles();
			return true;
		}

		void SaveChoices()
		{
			if (!OverridePrefs::HasChanges())
				return;
			OverridePrefs::Save();
			// the animations playing now come from the sets as they were
			if (g_thePlayer && g_thePlayer->parentCell && g_thePlayer->baseProcess)
				g_thePlayer->RestartAnims();
		}

		// What the registry has, by form (what sets are registered for) and by origin, made again after it changed
		class Catalog
		{
		public:
			// The sets of one origin for one form
			struct Source
			{
				const OverrideOrigin* origin = nullptr;
				// by file name, then point of view
				std::vector<Overrides::SetView> sets;
				int formRank = 0;
				int originRank = 0;
				UInt32 latest = 0;
				// disabled by the user for this form
				bool formDisabled = false;
				// of its sets: those that may play, and of these the ones no other origin's set is tried before
				// without a condition
				size_t numPlayable = 0;
				size_t numPlaying = 0;
			};

			struct Form
			{
				UInt32 identifier = 0;
				bool isModIndex = false;
				Category category = Category::Other;
				// its name; "weapon Service Rifle 0001EC17"
				std::string name;
				std::string label;
				// OverridePrefs' key
				std::string key;
				// in the order they are tried for the form
				std::vector<Source> sources;
				size_t numSets = 0;
			};

			struct Origin
			{
				const OverrideOrigin* origin = nullptr;
				int rank = 0;
				UInt32 latest = 0;
				// its forms by category and name: (index of forms, index of the form's sources)
				std::vector<std::pair<size_t, size_t>> forms;
				size_t numSets = 0;
			};

			// Where a set is in its stack
			struct Status
			{
				bool playable = false;
				// the first set of another origin tried before it that plays whenever it would, else the first that
				// plays at some of those times
				const SavedAnims* above = nullptr;
				// above plays whenever this set would: this one never does
				bool overridden = false;
			};

			// in category and name order
			std::vector<Form> forms;
			// in the default order
			std::vector<Origin> origins;

			void Invalidate() { generation = 0; }

			void Update()
			{
				if (generation == Overrides::Generation())
					return;
				generation = Overrides::Generation();
				forms.clear();
				origins.clear();
				stacks.clear();
				pathForms.clear();
				std::map<std::pair<UInt32, bool>, size_t> formIndices;
				std::unordered_map<const OverrideOrigin*, size_t> originIndices;
				Overrides::ForEachSet([&](const Overrides::SetView& view)
				{
					const auto& set = *view.set;
					if (!set.origin)
						return;
					const auto [formIter, newForm] = formIndices.try_emplace({ view.identifier, view.isModIndex }, forms.size());
					if (newForm)
						forms.push_back({ .identifier = view.identifier, .isModIndex = view.isModIndex });
					auto& form = forms[formIter->second];
					++form.numSets;
					auto source = std::ranges::find(form.sources, set.origin, &Source::origin);
					if (source == form.sources.end())
						source = form.sources.insert(form.sources.end(), Source{ .origin = set.origin });
					source->sets.push_back(view);
					// the same for every set of the origin and form
					source->formRank = set.formRank;
					source->originRank = set.origin->rank;
					source->formDisabled = set.formDisabled.load(std::memory_order_relaxed);
					source->latest = (std::max)(source->latest, set.sequence);

					const auto [originIter, newOrigin] = originIndices.try_emplace(set.origin, origins.size());
					if (newOrigin)
						origins.push_back({ .origin = set.origin, .rank = set.origin->rank });
					auto& origin = origins[originIter->second];
					origin.latest = (std::max)(origin.latest, set.sequence);
					++origin.numSets;

					stacks[{ view.identifier, view.isModIndex, view.firstPerson, view.groupId }].push_back(view);
					const std::pair formId(view.identifier, view.isModIndex);
					for (const auto& anim : set.anims)
					{
						auto& users = pathForms[anim.path];
						if (std::ranges::find(users, formId) == users.end())
							users.push_back(formId);
					}
				});
				for (auto& stack : stacks | std::views::values)
					std::ranges::sort(stack, {}, &Overrides::SetView::stackPosition);

				for (auto& form : forms)
				{
					form.category = CategoryOf(form.identifier, form.isModIndex);
					form.name = form.category == Category::Everyone ? kCategoryNames[static_cast<size_t>(Category::Everyone)]
						: DescribeIdentifier(form.identifier, form.isModIndex, true);
					form.label = DescribeIdentifier(form.identifier, form.isModIndex);
					form.key = Overrides::FormKey(form.identifier, form.isModIndex);
					std::ranges::sort(form.sources, [](const Source& a, const Source& b)
					{
						return std::tuple(a.formRank, a.originRank, a.latest) > std::tuple(b.formRank, b.originRank, b.latest);
					});
					for (auto& source : form.sources)
					{
						std::ranges::sort(source.sets, [](const Overrides::SetView& a, const Overrides::SetView& b)
						{
							return std::tuple(sv::get_file_stem(PathOf(*a.set)), a.firstPerson, a.set->folderCondition)
								< std::tuple(sv::get_file_stem(PathOf(*b.set)), b.firstPerson, b.set->folderCondition);
						});
						for (const auto& view : source.sets)
						{
							const auto status = StatusOf(view);
							source.numPlayable += status.playable;
							source.numPlaying += status.playable && !status.overridden;
						}
					}
				}
				std::ranges::sort(forms, [](const Form& a, const Form& b)
				{
					if (a.category != b.category)
						return a.category < b.category;
					const auto nameA = ToLower(a.name);
					const auto nameB = ToLower(b.name);
					return nameA != nameB ? nameA < nameB : a.identifier < b.identifier;
				});

				std::ranges::sort(origins, [](const Origin& a, const Origin& b)
				{
					return std::tuple(a.rank, a.latest) > std::tuple(b.rank, b.latest);
				});
				originIndices.clear();
				for (size_t i = 0; i < origins.size(); ++i)
					originIndices[origins[i].origin] = i;
				for (size_t formIndex = 0; formIndex < forms.size(); ++formIndex)
				{
					const auto& sources = forms[formIndex].sources;
					for (size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex)
						origins[originIndices[sources[sourceIndex].origin]].forms.emplace_back(formIndex, sourceIndex);
				}
			}

			Status StatusOf(const Overrides::SetView& view) const
			{
				Status status{ .playable = IsPlayable(*view.set) };
				if (!status.playable)
					return status;
				for (const auto& other : Stack(view))
				{
					if (other.set == view.set)
						break;
					if (other.set->origin == view.set->origin || !IsPlayable(*other.set))
						continue;
					if (Covers(*other.set, *view.set))
					{
						status.above = other.set;
						status.overridden = true;
						break;
					}
					if (!status.above)
						status.above = other.set;
				}
				return status;
			}

			// Whether the set plays whenever the other would: no script condition, and a subfolder that holds when the
			// other's does
			static bool Covers(const SavedAnims& set, const SavedAnims& other)
			{
				return set.conditionScriptText.empty() && !set.conditionScript.load(std::memory_order_relaxed)
					&& (set.folderCondition == FolderCondition::None || set.folderCondition == other.folderCondition);
			}

			// the sets of the view's stack, the one tried first first
			const std::vector<Overrides::SetView>& Stack(const Overrides::SetView& view) const
			{
				return stacks.at({ view.identifier, view.isModIndex, view.firstPerson, view.groupId });
			}

			// how many forms have sets with the file
			size_t NumFormsUsing(std::string_view path) const
			{
				const auto iter = pathForms.find(path);
				return iter != pathForms.end() ? iter->second.size() : 0;
			}

			bool IsRegistered(std::string_view path) const
			{
				return pathForms.contains(path);
			}

		private:
			using StackKey = std::tuple<UInt32, bool, bool, UInt16>;

			UInt32 generation = 0;
			std::map<StackKey, std::vector<Overrides::SetView>> stacks;
			// pooled path -> the forms (identifier, is mod index) it is registered for
			std::unordered_map<std::string_view, std::vector<std::pair<UInt32, bool>>> pathForms;
		};

		std::string DescribeSourceStatus(const Catalog::Source& source)
		{
			if (source.origin->userDisabled.load(std::memory_order_relaxed))
				return "disabled globally";
			if (source.formDisabled || !source.numPlayable)
				return "disabled";
			if (source.numPlaying == source.numPlayable)
				return "active";
			if (!source.numPlaying)
				return "overridden";
			return std::format("{} of {} active", source.numPlaying, source.numPlayable);
		}

		std::string DescribeSetStatus(const Catalog::Status& status)
		{
			if (!status.playable)
				return "disabled";
			if (!status.above)
				return "active";
			return status.overridden ? "overridden" : "partly overridden";
		}

		std::string DescribeSetDetails(const Catalog& catalog, const Overrides::SetView& view)
		{
			const auto& set = *view.set;
			auto text = set.anims.size() == 1 ? std::string(set.anims[0].path) : std::string(sv::get_file_stem(PathOf(set)));
			text += view.firstPerson ? "\n1st person" : "\n3rd person";
			if (set.anims.size() > 1)
				text += std::format(", {} variants ({})", set.anims.size(), set.hasOrder ? "in order" : "random");
			if (const char* folder = DescribeFolderCondition(set.folderCondition))
				text += std::format(", {} only", folder);
			if (!set.conditionScriptText.empty())
				text += std::format("\nCondition: {}", set.conditionScriptText);
			else if (set.conditionScript.load(std::memory_order_relaxed))
				text += "\nCondition: script";
			if (set.conditionFailed.load(std::memory_order_relaxed))
				text += "\nCondition has errors";
			if (set.disabled.load(std::memory_order_relaxed))
				text += "\nDisabled by a script";
			if (const auto status = catalog.StatusOf(view); status.above)
			{
				const auto& name = status.above->origin ? status.above->origin->name : "?";
				text += status.overridden ? std::format("\nOverridden by {}", name) : std::format("\nOverridden by {} when its condition is met", name);
			}
			return text;
		}

		std::string DescribeFile(const Catalog& catalog, std::string_view path)
		{
			auto text = std::string(path);
			if (const auto numForms = catalog.NumFormsUsing(path); numForms > 1)
				text += std::format("\nAlso used by {} other {}", numForms - 1, numForms == 2 ? "form" : "forms");
			return text;
		}

		// A base aim or attack file goes with its up and down files (their variant is picked by the base's)
		void SetFileEnabled(const Catalog& catalog, std::string_view path, bool enabled)
		{
			OverridePrefs::SetFileDisabled(path, !enabled);
			const auto extension = path.rfind('.');
			if (extension == std::string_view::npos)
				return;
			for (const auto* suffix : { "up", "down" })
			{
				const auto sibling = std::format("{}{}{}", path.substr(0, extension), suffix, path.substr(extension));
				if (catalog.IsRegistered(sibling))
					OverridePrefs::SetFileDisabled(sibling, !enabled);
			}
		}

		// all on, else all off
		void ToggleFiles(const Catalog& catalog, const SavedAnims& set)
		{
			const bool allEnabled = std::ranges::all_of(set.anims, [&](const AnimPath& anim) { return set.IsFileEnabled(anim); });
			for (const auto& anim : set.anims)
				SetFileEnabled(catalog, anim.path, !allEnabled);
		}

		std::string DescribeCategory(const Catalog& catalog, Category category)
		{
			size_t numForms = 0;
			size_t numShared = 0;
			for (const auto& form : catalog.forms)
			{
				if (form.category != category)
					continue;
				++numForms;
				numShared += form.sources.size() > 1;
			}
			return std::format("{} {}, {} with multiple sources", numForms, numForms == 1 ? "form" : "forms", numShared);
		}

		std::string Count(size_t number, const char* singular, const char* plural)
		{
			return std::format("{} {}", number, number == 1 ? singular : plural);
		}

		// The forms AnimGroupOverride animations are registered for (weapons, actors, races, a mod's actors, everyone),
		// and for the one selected the sources that animate it, in the order they are tried, with their animations and
		// files. Per form the user orders the sources and turns them off (OverridePrefs::FormPrefs): one author's
		// animations for a weapon over another's
		class AnimationsPage final : public Page
		{
		public:
			const char* GetTitle() const override { return "Animations"; }

			std::string GetHeading(Side side) const override
			{
				if (side == Side::Left)
					return "Forms";
				return selectedForm ? std::format("Sources for {}", selectedForm->name) : "Sources";
			}

			void Build(const Context& context, std::vector<Row>& left, std::vector<Row>& right) override
			{
				catalog.Update();
				leftRows.clear();
				const auto filter = context.filter;
				for (size_t category = 0; category < static_cast<size_t>(Category::Count); ++category)
				{
					const auto headingIndex = left.size();
					// everyone's sets are one form: no heading over it
					const bool headed = category != static_cast<size_t>(Category::Everyone);
					size_t numShown = 0;
					for (size_t formIndex = 0; formIndex < catalog.forms.size(); ++formIndex)
					{
						const auto& form = catalog.forms[formIndex];
						if (static_cast<size_t>(form.category) != category || !Matches(form, filter))
							continue;
						if (!numShown++ && headed)
						{
							left.push_back({ .text = kCategoryNames[category], .dim = true, .heading = true });
							leftRows.push_back({ static_cast<Category>(category), -1 });
						}
						const bool playing = std::ranges::any_of(form.sources, [](const Catalog::Source& source) { return source.numPlaying != 0; });
						left.push_back({ .text = form.name,
							.value = form.sources.size() > 1 ? std::format("{} sources", form.sources.size()) : "", .dim = !playing, .indent = headed });
						leftRows.push_back({ form.category, static_cast<int>(formIndex) });
					}
					if (numShown && headed)
						left[headingIndex].value = std::format("{}", numShown);
				}
				if (leftRows.empty())
					left.push_back({ .text = filter.empty() ? "None installed" : "No matches", .dim = true });

				rightRows.clear();
				sourceRows.clear();
				selectedForm = context.leftRow < static_cast<int>(leftRows.size()) && leftRows[context.leftRow].form >= 0
					? &catalog.forms[leftRows[context.leftRow].form] : nullptr;
				if (!selectedForm)
					return;
				const auto& form = *selectedForm;
				const bool showAll = filter.empty() || sv::contains_ci(form.label, filter);
				sourceRows.assign(form.sources.size(), -1);
				for (size_t sourceIndex = 0; sourceIndex < form.sources.size(); ++sourceIndex)
				{
					const auto& source = form.sources[sourceIndex];
					const bool showSets = showAll || sv::contains_ci(source.origin->name, filter);
					if (!showSets && std::ranges::none_of(source.sets, [&](const Overrides::SetView& view) { return SetMatches(*view.set, filter); }))
						continue;
					const bool off = source.formDisabled || source.origin->userDisabled.load(std::memory_order_relaxed);
					sourceRows[sourceIndex] = static_cast<int>(right.size());
					right.push_back({ .text = source.origin->name, .value = DescribeSourceStatus(source),
						.check = source.formDisabled ? Row::Check::Off : Row::Check::On, .dim = off });
					rightRows.push_back({ sourceIndex, -1, kSetRow });
					for (size_t setIndex = 0; setIndex < source.sets.size(); ++setIndex)
					{
						const auto& view = source.sets[setIndex];
						const auto& set = *view.set;
						if (!showSets && !SetMatches(set, filter))
							continue;
						const auto status = catalog.StatusOf(view);
						right.push_back({ .text = DescribeSetBrief(view), .value = DescribeSetStatus(status), .check = FilesCheck(set),
							.dim = !status.playable || status.overridden, .indent = 1 });
						rightRows.push_back({ sourceIndex, static_cast<int>(setIndex), kSetRow });
						if (set.anims.size() > 1)
						{
							for (size_t fileIndex = 0; fileIndex < set.anims.size(); ++fileIndex)
							{
								const auto& anim = set.anims[fileIndex];
								const bool enabled = set.IsFileEnabled(anim);
								right.push_back({ .text = std::string(RelativePath(*source.origin, anim.path)), .value = FileTags(anim),
									.check = enabled ? Row::Check::On : Row::Check::Off, .dim = !status.playable || status.overridden || !enabled, .indent = 2 });
								rightRows.push_back({ sourceIndex, static_cast<int>(setIndex), static_cast<int>(fileIndex) });
							}
						}
						if (!set.additiveAnimPath.empty())
						{
							right.push_back({ .text = std::string(RelativePath(*source.origin, set.additiveAnimPath)), .value = "additive",
								.dim = !status.playable || status.overridden, .indent = 2 });
							rightRows.push_back({ sourceIndex, static_cast<int>(setIndex), kAdditiveRow });
						}
					}
				}
			}

			std::string Describe(Side side, int row) const override
			{
				if (side == Side::Left)
				{
					if (row >= static_cast<int>(leftRows.size()))
						return {};
					const auto& leftRow = leftRows[row];
					if (leftRow.form < 0)
						return DescribeCategory(catalog, leftRow.category);
					return DescribeForm(catalog.forms[leftRow.form]);
				}
				if (!selectedForm || row >= static_cast<int>(rightRows.size()))
					return {};
				const auto& [sourceIndex, setIndex, fileIndex] = rightRows[row];
				const auto& source = selectedForm->sources[sourceIndex];
				if (setIndex < 0)
					return DescribeSource(source);
				const auto& view = source.sets[setIndex];
				if (fileIndex >= 0)
					return DescribeFile(catalog, view.set->anims[fileIndex].path);
				if (fileIndex == kAdditiveRow)
					return std::format("{}\nAdditive", view.set->additiveAnimPath);
				return DescribeSetDetails(catalog, view);
			}

			std::string GetHelp() const override
			{
				return "Enter: toggle   Home/End: move   Del: reset form";
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
				if (side == Side::Left || !selectedForm || row >= static_cast<int>(rightRows.size()))
					return false;
				const auto& [sourceIndex, setIndex, fileIndex] = rightRows[row];
				const auto& source = selectedForm->sources[sourceIndex];
				if (setIndex < 0)
				{
					OverridePrefs::SetFormOriginDisabled(selectedForm->key, source.origin->name, !source.formDisabled);
					return Apply();
				}
				const auto& set = *source.sets[setIndex].set;
				if (fileIndex == kSetRow)
					ToggleFiles(catalog, set);
				else if (fileIndex >= 0)
				{
					const auto& anim = set.anims[fileIndex];
					SetFileEnabled(catalog, anim.path, !set.IsFileEnabled(anim));
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
					if (side != Side::Right || !selectedForm || row >= static_cast<int>(rightRows.size()))
						return false;
					return Move(row, button == 0 ? -1 : 1, selectedRow);
				case 2:
					if (!selectedForm)
						return false;
					OverridePrefs::SetFormOrder(selectedForm->key, {});
					return Apply();
				case 3:
					if (!ReloadFiles())
						return false;
					catalog.Invalidate();
					return true;
				default:
					return false;
				}
			}

			bool Reset(Side side, int row) override
			{
				if (!selectedForm)
					return false;
				OverridePrefs::ResetForm(selectedForm->key);
				return Apply();
			}

			void Close() override
			{
				SaveChoices();
			}

		private:
			static constexpr int kSetRow = -1;
			static constexpr int kAdditiveRow = -2;

			// a row of the left list: a category's heading (form -1) or a form (index of the catalog's forms)
			struct LeftRow
			{
				Category category;
				int form;
			};

			// a row of the right list: a source (set -1), one of its sets (file kSetRow), one of the set's files (index
			// into anims), or its additive file (kAdditiveRow)
			struct RightRow
			{
				size_t source;
				int set;
				int file;
			};

			static bool Matches(const Catalog::Form& form, std::string_view filter)
			{
				return filter.empty() || sv::contains_ci(form.label, filter) || sv::contains_ci(kCategoryNames[static_cast<size_t>(form.category)], filter)
					|| std::ranges::any_of(form.sources, [&](const Catalog::Source& source)
					{
						return sv::contains_ci(source.origin->name, filter)
							|| std::ranges::any_of(source.sets, [&](const Overrides::SetView& view) { return SetMatches(*view.set, filter); });
					});
			}

			std::string DescribeForm(const Catalog::Form& form) const
			{
				auto text = std::format("{}\n{}, {}", form.label, Count(form.sources.size(), "source", "sources"), Count(form.numSets, "animation", "animations"));
				const auto prefs = OverridePrefs::GetForm(form.key);
				if (!prefs.order.empty())
					text += "\nCustom order";
				if (!prefs.disabledOrigins.empty())
					text += std::format("\n{} disabled", Count(prefs.disabledOrigins.size(), "source", "sources"));
				return text;
			}

			std::string DescribeSource(const Catalog::Source& source) const
			{
				auto text = DescribeOrigin(*source.origin);
				if (source.origin->userDisabled.load(std::memory_order_relaxed))
					text += "\nDisabled for all forms";
				else if (source.formDisabled)
					text += "\nDisabled for this form";
				else
					text += std::format("\n{} of {} animations active", source.numPlaying, source.sets.size());
				return text;
			}

			// Moves the source of the row past the next source shown that way, for the selected form only
			bool Move(int row, int direction, int& selectedRow)
			{
				const auto& form = *selectedForm;
				const auto from = rightRows[row].source;
				// the sources shown, in order
				std::vector<size_t> shown;
				for (size_t i = 0; i < sourceRows.size(); ++i)
				{
					if (sourceRows[i] >= 0)
						shown.push_back(i);
				}
				const auto position = static_cast<int>(std::ranges::find(shown, from) - shown.begin());
				const int toPosition = position + direction;
				if (toPosition < 0 || toPosition >= static_cast<int>(shown.size()))
					return false;
				const auto to = shown[toPosition];
				std::vector<std::string> order;
				for (const auto& source : form.sources)
					order.push_back(source.origin->name);
				auto name = order[from];
				order.erase(order.begin() + from);
				order.insert(order.begin() + to, std::move(name));

				// the selection stays on its row of the source: the rows of the two sources swap
				const auto rowsOf = [&](size_t source)
				{
					const auto next = std::ranges::find_if(shown, [&](size_t other) { return sourceRows[other] > sourceRows[source]; });
					return (next != shown.end() ? sourceRows[*next] : static_cast<int>(rightRows.size())) - sourceRows[source];
				};
				const int firstRow = (std::min)(sourceRows[from], sourceRows[to]);
				selectedRow = (direction < 0 ? firstRow : firstRow + rowsOf(to)) + (row - sourceRows[from]);

				OverridePrefs::SetFormOrder(form.key, std::move(order));
				return Apply();
			}

			Catalog catalog;
			std::vector<LeftRow> leftRows;
			std::vector<RightRow> rightRows;
			// per source of the selected form: its row, -1 when the filter hides it
			std::vector<int> sourceRows;
			const Catalog::Form* selectedForm = nullptr;
		};

		// The sources of AnimGroupOverride animations (folders, JSON entries, scripts) in the default order, and for
		// the one selected the forms it animates. The user turns sources off everywhere or for one form, and orders them
		// for the forms that have no order of their own
		class SourcesPage final : public Page
		{
		public:
			const char* GetTitle() const override { return "Sources"; }

			std::string GetHeading(Side side) const override
			{
				return side == Side::Left ? "Sources" : "Used by";
			}

			void Build(const Context& context, std::vector<Row>& left, std::vector<Row>& right) override
			{
				catalog.Update();
				const auto filter = context.filter;
				shownOrigins.clear();
				for (size_t i = 0; i < catalog.origins.size(); ++i)
				{
					if (Matches(catalog.origins[i], filter))
						shownOrigins.push_back(i);
				}
				for (const auto index : shownOrigins)
				{
					const auto& origin = catalog.origins[index];
					const bool disabled = origin.origin->userDisabled.load(std::memory_order_relaxed);
					left.push_back({ .text = origin.origin->name,
						.value = Count(origin.forms.size(), "form", "forms"),
						.check = disabled ? Row::Check::Off : Row::Check::On, .dim = disabled });
				}
				if (shownOrigins.empty())
					left.push_back({ .text = filter.empty() ? "None installed" : "No matches", .dim = true });

				rightRows.clear();
				selectedOrigin = context.leftRow < static_cast<int>(shownOrigins.size()) ? &catalog.origins[shownOrigins[context.leftRow]] : nullptr;
				if (!selectedOrigin)
					return;
				const bool showAll = filter.empty() || sv::contains_ci(selectedOrigin->origin->name, filter);
				std::optional<Category> category;
				for (const auto& [formIndex, sourceIndex] : selectedOrigin->forms)
				{
					const auto& form = catalog.forms[formIndex];
					const auto& source = form.sources[sourceIndex];
					if (!showAll && !sv::contains_ci(form.label, filter)
						&& std::ranges::none_of(source.sets, [&](const Overrides::SetView& view) { return SetMatches(*view.set, filter); }))
						continue;
					if (category != form.category)
					{
						category = form.category;
						right.push_back({ .text = kCategoryNames[static_cast<size_t>(form.category)], .dim = true, .heading = true });
						rightRows.push_back({ form.category, -1, 0 });
					}
					right.push_back({ .text = form.name, .value = DescribeSourceStatus(source),
						.check = source.formDisabled ? Row::Check::Off : Row::Check::On,
						.dim = source.formDisabled || source.origin->userDisabled.load(std::memory_order_relaxed), .indent = 1 });
					rightRows.push_back({ form.category, static_cast<int>(formIndex), sourceIndex });
				}
			}

			std::string Describe(Side side, int row) const override
			{
				if (side == Side::Left)
				{
					if (row >= static_cast<int>(shownOrigins.size()))
						return {};
					const auto& origin = catalog.origins[shownOrigins[row]];
					auto text = std::format("{}\n{}, {}", DescribeOrigin(*origin.origin), Count(origin.forms.size(), "form", "forms"),
						Count(origin.numSets, "animation", "animations"));
					if (origin.origin->userDisabled.load(std::memory_order_relaxed))
						text += "\nDisabled for all forms";
					return text;
				}
				if (!selectedOrigin || row >= static_cast<int>(rightRows.size()))
					return {};
				const auto& rightRow = rightRows[row];
				if (rightRow.form < 0)
					return DescribeCategory(catalog, rightRow.category);
				const auto& form = catalog.forms[rightRow.form];
				const auto& source = form.sources[rightRow.source];
				auto text = std::format("{}\n{} of {} animations active", form.label, source.numPlaying, source.sets.size());
				if (source.formDisabled)
					text += "\nDisabled for this form";
				return text;
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
					const auto& origin = *catalog.origins[shownOrigins[row]].origin;
					OverridePrefs::SetOriginDisabled(origin.name, !origin.userDisabled.load(std::memory_order_relaxed));
					return Apply();
				}
				if (!selectedOrigin || row >= static_cast<int>(rightRows.size()) || rightRows[row].form < 0)
					return false;
				const auto& form = catalog.forms[rightRows[row].form];
				const auto& source = form.sources[rightRows[row].source];
				OverridePrefs::SetFormOriginDisabled(form.key, source.origin->name, !source.formDisabled);
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
					if (!ReloadFiles())
						return false;
					catalog.Invalidate();
					return true;
				default:
					return false;
				}
			}

			void Close() override
			{
				SaveChoices();
			}

		private:
			// a row of the right list: a category's heading (form -1) or a form (index of the catalog's forms) with the
			// index of the selected origin among its sources
			struct RightRow
			{
				Category category;
				int form;
				size_t source;
			};

			bool Matches(const Catalog::Origin& origin, std::string_view filter) const
			{
				return filter.empty() || sv::contains_ci(origin.origin->name, filter)
					|| std::ranges::any_of(origin.forms, [&](const std::pair<size_t, size_t>& entry)
					{
						const auto& form = catalog.forms[entry.first];
						return sv::contains_ci(form.label, filter)
							|| std::ranges::any_of(form.sources[entry.second].sets, [&](const Overrides::SetView& view) { return SetMatches(*view.set, filter); });
					});
			}

			bool Move(int row, int direction, int& selectedRow)
			{
				std::vector<std::string> order;
				for (const auto& origin : catalog.origins)
					order.push_back(origin.origin->name);
				const auto from = static_cast<int>(shownOrigins[row]);
				// past the origins the filter hides
				const int toRow = row + direction;
				if (toRow < 0 || toRow >= static_cast<int>(shownOrigins.size()))
					return false;
				const auto to = static_cast<int>(shownOrigins[toRow]);
				auto name = order[from];
				order.erase(order.begin() + from);
				order.insert(order.begin() + to, std::move(name));
				OverridePrefs::SetOrder(std::move(order));
				selectedRow = toRow;
				return Apply();
			}

			Catalog catalog;
			std::vector<size_t> shownOrigins;
			const Catalog::Origin* selectedOrigin = nullptr;
			std::vector<RightRow> rightRows;
		};
	}

	std::unique_ptr<Page> MakeAnimationsPage()
	{
		return std::make_unique<AnimationsPage>();
	}

	std::unique_ptr<Page> MakeSourcesPage()
	{
		return std::make_unique<SourcesPage>();
	}
}
