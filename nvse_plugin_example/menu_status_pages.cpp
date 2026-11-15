#include <format>
#include <unordered_map>

#include "actor_overrides.h"
#include "commands_animation.h"
#include "GameObjects.h"
#include "GameProcess.h"
#include "menu_page.h"
#include "string_view_util.h"

namespace KnvseMenu
{
	namespace
	{
		const char* DescribeVerdict(Overrides::Verdict verdict)
		{
			switch (verdict)
			{
			case Overrides::Verdict::Plays: return "plays";
			case Overrides::Verdict::Disabled: return "disabled";
			case Overrides::Verdict::FolderCondition: return "not for this actor";
			case Overrides::Verdict::NoTransition: return "no such start / stop";
			case Overrides::Verdict::OtherIdle: return "other idle";
			case Overrides::Verdict::LoaderThread: return "loader thread";
			case Overrides::Verdict::ConditionFailed: return "condition broken";
			case Overrides::Verdict::ConditionFalse: return "condition false";
			case Overrides::Verdict::NoFiles: return "additive only";
			}
			return "";
		}

		const char* DescribeVerdictLong(Overrides::Verdict verdict)
		{
			switch (verdict)
			{
			case Overrides::Verdict::Plays: return "Its conditions hold: the first such set is the one that plays.";
			case Overrides::Verdict::Disabled: return "Disabled (in this menu, or by a script).";
			case Overrides::Verdict::FolderCondition: return "Its subfolder (male, female, mod1-3, hurt, human) does not match the actor.";
			case Overrides::Verdict::NoTransition: return "It has only start / stop animations, or not the kind asked for.";
			case Overrides::Verdict::OtherIdle: return "It has no file of the idle that plays.";
			case Overrides::Verdict::LoaderThread: return "Its script condition is not run on the thread that loads the actor.";
			case Overrides::Verdict::ConditionFailed: return "Its condition does not compile (see Problems).";
			case Overrides::Verdict::ConditionFalse: return "Its condition does not hold now.";
			case Overrides::Verdict::NoFiles: return "It has only an additive animation.";
			}
			return "";
		}

		// What the player plays now in each section of both skeletons, and for the one selected every set that could
		// play instead, in the order they are tried, with why each does or does not
		class PlayingPage final : public Page
		{
		public:
			const char* GetTitle() const override { return "Playing now"; }

			std::string GetHeading(Side side) const override
			{
				return side == Side::Left ? "The player's animations" : "Sets for it, in the order they are tried";
			}

			void Build(const Context& context, std::vector<Row>& left, std::vector<Row>& right) override
			{
				sections.clear();
				candidates.clear();
				views.clear();
				if (!g_thePlayer || !g_thePlayer->parentCell || !g_thePlayer->baseProcess)
				{
					left.push_back({ .text = "Load a game first", .dim = true });
					return;
				}
				Overrides::ForEachSet([&](const Overrides::SetView& view) { views.emplace(view.set, view); });
				const std::pair<AnimData*, const char*> skeletons[] = {
					{ g_thePlayer->baseProcess->GetAnimData(), "3rd" }, { g_thePlayer->firstPersonAnimData, "1st" }
				};
				static constexpr const char* kSections[] = { "idle", "movement", "left arm", "left hand", "weapon", "weapon up", "weapon down", "special idle" };
				for (const auto& [animData, pov] : skeletons)
				{
					if (!animData)
						continue;
					for (int section = 0; section < 8; ++section)
					{
						auto* sequence = animData->animSequence[section];
						if (!sequence || !sequence->animGroup || !sequence->m_kName.CStr())
							continue;
						const auto path = std::string_view(sequence->m_kName.CStr());
						if (!context.filter.empty() && !sv::contains_ci(path, context.filter))
							continue;
						sections.push_back({ animData, sequence });
						const auto* set = FindPlayingSet(animData, sequence);
						left.push_back({ .text = std::format("{} {}: {}", pov, kSections[section], sv::get_file_stem(path)),
							.value = set ? "override" : "game" });
					}
				}
				if (context.leftRow >= static_cast<int>(sections.size()))
					return;
				const auto& [animData, sequence] = sections[context.leftRow];
				candidates = Overrides::Explain(sequence->animGroup->groupID, animData);
				bool played = false;
				for (const auto& [set, verdict] : candidates)
				{
					const bool plays = verdict == Overrides::Verdict::Plays && !played;
					played |= plays;
					const auto view = views.find(set);
					right.push_back({ .text = std::format("{}: {}", set->origin ? set->origin->name : "?",
						view != views.end() ? DescribeSet(view->second) : std::string()),
						.value = plays ? "plays" : verdict == Overrides::Verdict::Plays ? "not reached" : DescribeVerdict(verdict),
						.dim = !plays });
				}
			}

			std::string Describe(Side side, int row) const override
			{
				if (side == Side::Left)
				{
					if (row >= static_cast<int>(sections.size()))
						return "The player's animations are listed once a game is loaded.";
					const auto& [animData, sequence] = sections[row];
					const auto* set = FindPlayingSet(animData, sequence);
					return std::format("{}\n{}", sequence->m_kName.CStr(),
						set && set->origin ? std::format("From {}.", DescribeOrigin(*set->origin)) : std::string("The game's own animation: no set of an origin plays."));
				}
				const auto& [set, verdict] = candidates[row];
				const auto view = views.find(set);
				auto text = std::format("{}\n{}", view != views.end() ? DescribeSet(view->second) : std::string(), DescribeVerdictLong(verdict));
				if (set->origin)
					text += std::format("\nFrom {}.", DescribeOrigin(*set->origin));
				if (!set->conditionScriptText.empty())
					text += std::format("\nCondition: {}", set->conditionScriptText);
				return text;
			}

			std::string GetHelp() const override
			{
				return "Up, Down: pick an animation   Right: the sets for it   Tab: next page   type: filter   Esc: close";
			}

		private:
			// The set the sequence was played from: one of those tried for its group that has its file
			const SavedAnims* FindPlayingSet(AnimData* animData, BSAnimGroupSequence* sequence) const
			{
				for (const auto& [set, verdict] : Overrides::Explain(sequence->animGroup->groupID, animData))
				{
					if (set->ContainsAnim(sequence))
						return set;
				}
				return nullptr;
			}

			struct Section
			{
				AnimData* animData;
				BSAnimGroupSequence* sequence;
			};
			std::vector<Section> sections;
			std::vector<Overrides::Candidate> candidates;
			std::unordered_map<const SavedAnims*, Overrides::SetView> views;
		};

		// What went wrong loading the override files, by kind: shown until they are loaded again
		class ProblemsPage final : public Page
		{
		public:
			const char* GetTitle() const override { return "Problems"; }

			std::string GetHeading(Side side) const override
			{
				return side == Side::Left ? "Problems with the override files" : "Problems";
			}

			void Build(const Context& context, std::vector<Row>& left, std::vector<Row>& right) override
			{
				using enum Overrides::Problem::Kind;
				problems.clear();
				for (auto& problem : Overrides::GetProblems())
				{
					if (context.filter.empty() || sv::contains_ci(problem.text, context.filter))
						problems.push_back(std::move(problem));
				}
				for (const auto kind : { Files, Condition, Load })
				{
					const auto count = std::ranges::count(problems, kind, &Overrides::Problem::kind);
					left.push_back({ .text = KindName(kind), .value = std::format("{}", count), .dim = count == 0 });
				}
				const auto kind = static_cast<Overrides::Problem::Kind>(context.leftRow);
				shown.clear();
				for (const auto& problem : problems)
				{
					if (problem.kind != kind)
						continue;
					shown.push_back(&problem);
					right.push_back({ .text = problem.text });
				}
			}

			std::string Describe(Side side, int row) const override
			{
				using enum Overrides::Problem::Kind;
				if (side == Side::Left)
				{
					switch (static_cast<Overrides::Problem::Kind>(row))
					{
					case Files: return "JSON files that could not be read, and folders or forms they name that are not there.";
					case Condition: return "Conditions of JSON entries or scripts that do not compile (the console has the error): the sets that have one never play.";
					case Load: return "Override KFs that failed to load (kNVSE.log has why): they are passed over when a variant is picked.";
					}
					return {};
				}
				return std::format("{}\n{}", shown[row]->text, Describe(Side::Left, static_cast<int>(shown[row]->kind)));
			}

			std::string GetHelp() const override
			{
				return "Up, Down: pick   Right: the problems of a kind   Tab: next page   type: filter   Esc: close";
			}

			std::string GetButton(int button) const override
			{
				return button == 3 ? "Reload files" : "";
			}

			bool Press(int button, Side side, int row, int& selectedRow) override
			{
				if (button != 3 || !g_thePlayer || !g_thePlayer->baseProcess)
					return false;
				ReloadOverrideFiles();
				return true;
			}

		private:
			static const char* KindName(Overrides::Problem::Kind kind)
			{
				switch (kind)
				{
				case Overrides::Problem::Kind::Files: return "JSON files and folders";
				case Overrides::Problem::Kind::Condition: return "Conditions";
				case Overrides::Problem::Kind::Load: return "KFs that do not load";
				}
				return "";
			}

			std::vector<Overrides::Problem> problems;
			std::vector<const Overrides::Problem*> shown;
		};
	}

	std::unique_ptr<Page> MakePlayingPage()
	{
		return std::make_unique<PlayingPage>();
	}

	std::unique_ptr<Page> MakeProblemsPage()
	{
		return std::make_unique<ProblemsPage>();
	}
}
