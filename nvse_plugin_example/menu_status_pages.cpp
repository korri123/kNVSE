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
			case Overrides::Verdict::NoTransition: return "start/stop only";
			case Overrides::Verdict::OtherIdle: return "other idle";
			case Overrides::Verdict::LoaderThread: return "loader thread";
			case Overrides::Verdict::ConditionFailed: return "bad condition";
			case Overrides::Verdict::ConditionFalse: return "condition false";
			case Overrides::Verdict::NoFiles: return "additive only";
			}
			return "";
		}

		// Only where the short verdict needs it
		const char* DescribeVerdictLong(Overrides::Verdict verdict)
		{
			switch (verdict)
			{
			case Overrides::Verdict::FolderCondition: return "Its subfolder (male, female, mod1-3, hurt, human) doesn't match.";
			case Overrides::Verdict::OtherIdle: return "No file for the playing idle.";
			case Overrides::Verdict::LoaderThread: return "Script conditions aren't checked while the actor loads.";
			case Overrides::Verdict::ConditionFailed: return "Condition doesn't compile.";
			default: return "";
			}
		}

		// What the player plays now in each section of both skeletons, and for the one selected every set that could
		// play instead, in the order they are tried, with why each does or does not
		class PlayingPage final : public Page
		{
		public:
			const char* GetTitle() const override { return "Playing now"; }

			std::string GetHeading(Side side) const override
			{
				return side == Side::Left ? "Player" : "Overrides, by priority";
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
					// group and person are the selected animation's: only who it is for tells sets apart
					auto text = std::string(set->origin ? set->origin->name : "?");
					if (view != views.end() && !(view->second.isModIndex && view->second.identifier == Overrides::kEveryone))
						text += std::format(" ({})", DescribeIdentifier(view->second.identifier, view->second.isModIndex, true));
					right.push_back({ .text = std::move(text),
						.value = plays ? "plays" : verdict == Overrides::Verdict::Plays ? "overridden" : DescribeVerdict(verdict),
						.dim = !plays });
				}
			}

			std::string Describe(Side side, int row) const override
			{
				if (side == Side::Left)
				{
					if (row >= static_cast<int>(sections.size()))
						return {};
					const auto& [animData, sequence] = sections[row];
					const auto* set = FindPlayingSet(animData, sequence);
					return set && set->origin ? std::format("{}\n{}", sequence->m_kName.CStr(), DescribeOrigin(*set->origin)) : sequence->m_kName.CStr();
				}
				const auto& [set, verdict] = candidates[row];
				auto text = set->origin ? DescribeOrigin(*set->origin) : std::string();
				if (!set->conditionScriptText.empty())
					text += std::format("\nCondition: {}", set->conditionScriptText);
				if (const char* reason = DescribeVerdictLong(verdict); *reason)
					text += std::format("\n{}", reason);
				return text;
			}

			std::string GetHelp() const override
			{
				return {};
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
				return side == Side::Left ? "Problems" : "Details";
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
					case Files: return "JSON files that can't be read, missing folders and forms.";
					case Condition: return "Conditions that don't compile. Their sets never play. The console has the errors.";
					case Load: return "KFs that failed to load. kNVSE.log has why.";
					}
					return {};
				}
				return shown[row]->text;
			}

			std::string GetHelp() const override
			{
				return {};
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
				case Overrides::Problem::Kind::Files: return "Files";
				case Overrides::Problem::Kind::Condition: return "Conditions";
				case Overrides::Problem::Kind::Load: return "KF loading";
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
