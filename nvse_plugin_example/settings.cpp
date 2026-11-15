#include "settings.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

#include "SimpleINILibrary.h"
#include "string_view_util.h"
#include "utility.h"

PluginINISettings g_pluginSettings;

namespace Settings
{
	namespace
	{
		constexpr auto kIniPath = R"(Data\NVSE\Plugins\kNVSE.ini)";

		constexpr auto kLogging = "Logging";
		constexpr auto kBlending = "Blending";
		constexpr auto kTransitions = "Start/stop animations";
		constexpr auto kAnimFixes = "Animation file fixes";
		constexpr auto kGameFixes = "Game fixes";

		auto& s = g_pluginSettings;

		// In the order the keys are added to a new INI
		const Setting s_settings[] =
		{
			{ .section = "General", .key = "iConsoleLogLevel", .value = &g_logLevel, .defaultValue = 1, .min = 0, .max = 2,
				.label = "Log level", .category = kLogging, .apply = Apply::Live,
				.summary = "0: off. 1: kNVSE.log. 2: also the console.",
				.description = "0 = no log, 1 = kNVSE.log, 2 = console log" },
			{ .section = "General", .key = "iErrorLogLevel", .value = &g_errorLogLevel, .defaultValue = 1, .min = 0, .max = 2,
				.label = "Error log level", .category = kLogging, .apply = Apply::Live,
				.summary = "0: off. 1: kNVSE.log. 2: also the console.",
				.description = "0 = no log, 1 = kNVSE.log, 2 = error console log" },
			{ .section = "Blend Fixes", .key = "bBlendSmoothing", .value = &s.blendSmoothing, .defaultValue = 1,
				.label = "Blend smoothing", .category = kBlending, .apply = Apply::RestartToEnable,
				.summary = "Fades out snaps when animations switch without a blend.",
				.description = "smooths out the snaps in animation blending: where the blend weights jump (animations switched without a blend, more than two animation priorities on a bone) the previous pose fades out instead. Blends that are smooth already keep their timing. for some animations this might not be wanted, add noBlendSmoothing as text key to disable." },
			{ .section = "Blend Fixes", .key = "fBlendSmoothingRate", .value = &s.blendSmoothingRate, .defaultValue = 0.075, .min = 0.005, .max = 1, .step = 0.025,
				.label = "Blend smoothing time", .category = kBlending, .apply = Apply::Live, .requirements = { "bBlendSmoothing" },
				.summary = "Seconds a snap takes to fade out. Higher is smoother but slower.",
				.description = "how long (in seconds) a snap takes to fade out with bBlendSmoothing: about a third of it is left after this time. higher values mean smoother but slower." },
			{ .section = "Blend Fixes", .key = "fBlendEaseCurve", .value = &s.blendEaseCurve, .defaultValue = 0.7, .min = 0, .max = 1, .step = 0.05,
				.label = "Blend ease curve", .category = kBlending, .apply = Apply::Live,
				.summary = "0: blends run at constant speed, as in the game. 1: they ease in and out.",
				.description = "how gradually blends between animations start and stop, from 0 (constant speed, as the game does it) to 1 (they speed up at their start and slow down towards their end). Blends take as long as before and are half way at the same moment. Animations can pick another curve with the text keys \"blendCurve: <curve>\" (blends into and out of them), \"blendInCurve: <curve>\" and \"blendOutCurve: <curve>\", each from its time on: default, linear, smooth, inSine, outSine, inOutSine (also Quad, Cubic, Quart, Expo, Circ) or \"bezier x1 y1 x2 y2\" with each in 0..1. A blend takes the blendInCurve of the animation blending in, else the blendOutCurve of the one blending out; with bBlendSmoothing the curve is exactly how far the bone has blended." },
			{ .section = "Blend Fixes", .key = "bFixSpineBlendBug", .value = &s.fixSpineBlendBug, .defaultValue = 1,
				.label = "Fix spine blend when aiming", .category = kBlending, .apply = Apply::RestartToEnable, .requirements = { "bBlendSmoothing" },
				.summary = "Fixes the spine when leaving 3rd person aim while looking up or down.",
				.description = "fix spine blend bug when aiming down sights in 3rd person and cancelling the aim while looking up or down (requires bBlendSmoothing)." },
			{ .section = "Blend Fixes", .key = "bFixWeaponHandBlend", .value = &s.fixWeaponHandBlend, .defaultValue = 1,
				.label = "Keep weapon in its hand", .category = kBlending, .apply = Apply::Live,
				.summary = "Keeps the weapon on the hand holding it during blends.",
				.description = "keep the weapon on the hand holding it when animations move it to the left hand (counter-animated against the right hand): during blends, and between keys of animations baked at low frame rates. Detected automatically; animators can set it with the text keys \"weaponSpace: left\", \"weaponSpace: right\", \"weaponSpace: body\" (follows neither hand, for hand-offs) and \"weaponSpace: auto\", each holding until the next, or opt an animation out with \"weaponSpace: off\" (its share of a blend is blended as without this fix)." },
			{ .section = "Blend Fixes", .key = "bKeepLeftHandOnWeapon", .value = &s.keepLeftHandOnWeapon, .defaultValue = 1,
				.label = "Keep left hand on weapon", .category = kBlending, .apply = Apply::Live, .requirements = { "bFixWeaponHandBlend" },
				.summary = "Keeps the left hand on two-handed weapons during blends.",
				.description = "keep the left hand on two-handed weapons during blends (hip to sights and back, ...): the arms are blended joint by joint, so the hand slid off the weapon halfway through a blend between two animations that both hold it. The left arm is turned to reach for where those animations hold the weapon. Requires bFixWeaponHandBlend." },
#if _DEBUG
			{ .section = "Blend Fixes", .key = "bWeaponHandBlendDebugLog", .value = &s.weaponHandBlendDebugLog, .defaultValue = 0,
				.label = "Log weapon hands", .category = kLogging, .apply = Apply::Live, .requirements = { "bFixWeaponHandBlend" },
				.summary = "Logs which hand holds the weapon in each animation.",
				.description = "log to kNVSE.log how long each animation holds the weapon in each hand, as detected by bFixWeaponHandBlend." },
#endif
			{ .section = "Blend Fixes", .key = "bFixBlendFromPose", .value = &s.fixBlendFromPose, .defaultValue = 1,
				.label = "Ease in instead of a frozen pose", .category = kBlending, .apply = Apply::RestartToEnable, .requirements = { "bBlendSmoothing" },
				.summary = "Draws, hit reactions and idles ease in instead of starting from a frozen pose.",
				.description = "start animations that the game blends in from a frozen snapshot of the pose (drawing weapons, hit reactions, special idles, the Pip-Boy) with a normal ease-in instead: lower priority animations keep playing under them during the blend and they play from their start instead of holding their first frame, and the animations the game cut off for them fade out over the same time. Knocked down, ragdolled and dead actors keep the snapshot. Requires bBlendSmoothing." },
			{ .section = "Blend Fixes", .key = "bFixWeaponParentSwitch", .value = &s.fixWeaponParentSwitch, .defaultValue = 1,
				.label = "Fix weapon jump at draw/holster", .category = kBlending, .apply = Apply::Live, .requirements = { "bBlendSmoothing" },
				.summary = "Stops the weapon jumping as it moves between hand and holster.",
				.description = "fix the weapon flying off the hand or holster for a moment while equip and unequip animations move it between them (e.g. the Service Rifle's 2haunequip.kf, most visible with a low sgtm): the weapon's animation is held in the space of the node it is attached to until the game moves it. Requires bBlendSmoothing." },
			{ .section = "Blend Fixes", .key = "bFixGaitSwitch", .value = &s.fixGaitSwitch, .defaultValue = 1,
				.label = "Keep walks in step", .category = kBlending, .apply = Apply::Live, .requirements = { "bBlendSmoothing" },
				.summary = "Keeps the feet in step when the game switches to another walk or run.",
				.description = "keep walking and running in step when the game switches to another version of the same movement animation (to the drawn weapon's at the point of drawing or holstering where the weapon reaches the hand or holster, sneaking, power armor): the new one starts at the step the legs are at instead of its first frame, so the feet don't slide or scissor through the blend. Requires bBlendSmoothing." },
			{ .section = "Blend Fixes", .key = "bFixMovementMorphKeys", .value = &s.fixMovementMorphKeys, .defaultValue = 1,
				.label = "Sync walks by m: keys", .category = kBlending, .apply = Apply::Live, .requirements = { "bBlendSmoothing" },
				.summary = "Keeps walks in step by their m: text keys.",
				.description = "make m: text keys (m:L, m:R, ...) keep movement animations in step, as they do in the game without bBlendSmoothing: switching between two movement animations with the same set of m: keys starts the new one at the point between its keys that matches where the old one is, and the old one follows the new one's cycle while it fades out. Unlike the game, the two need not animate the same number of bones or start with the same m: key. Used instead of bFixGaitSwitch where both animations have them. Requires bBlendSmoothing." },
			{ .section = "Blend Fixes", .key = "bFixOverlappingBlends", .value = &s.fixOverlappingBlends, .defaultValue = 1,
				.label = "Blend overlapping switches", .category = kBlending, .apply = Apply::Live, .requirements = { "bBlendSmoothing" },
				.summary = "Blends switches that overlap, like holstering while running.",
				.description = "blend animation switches that overlap on the same bones (holstering while the run switches to the unarmed one, aiming while the strafe changes, ...) as a mix of what each switch goes from and to: the game only blends the two highest priorities on a bone, so the hand lingered at the holster for a moment when holstering a weapon while running in third person. Bones with one switch at a time blend as before. Requires bBlendSmoothing." },
			{ .section = "Blend Fixes", .key = "bMovementTransitions", .value = &s.movementTransitions, .defaultValue = 1,
				.label = "Walk start/stop animations", .category = kTransitions, .apply = Apply::Live, .requirements = { "bBlendSmoothing" },
				.summary = "Plays _start and _stop animations of walks and runs.",
				.description = "play start and stop animations of walks and runs: files in an AnimGroupOverride folder named like the walk or run they belong to with _start or _stop in the name (mtforward_start.kf, 2hafastforward_stop_a.kf; the KF's cycle type may be Clamp). A start animation plays when an actor starts walking or running from standing and goes on to the walk at its End text key, a stop animation (of several, the one whose first frame has the feet closest to where they are: stops on the left and right foot) plays when the player stops and blends to the idle at its End key. NPCs walking to the end of a path start theirs before they get there, where its root motion ends at the point they stop, so they stop where they would have; NPCs stopped by anything else (dialogue, waiting for another actor) stop as before. Both move the actor by their root motion. In first person the player plays those of _1stperson folders; a third person one plays there only with an allowFirstPerson text key (else the camera would move by a start or stop the arms do not show). Requires bBlendSmoothing." },
			{ .section = "Blend Fixes", .key = "bAimTransitions", .value = &s.aimTransitions, .defaultValue = 1,
				.label = "Aim start/stop animations", .category = kTransitions, .apply = Apply::Live, .requirements = { "bBlendSmoothing", "bFixSpineBlendBug" },
				.summary = "Plays _start and _stop animations when raising and lowering the sights.",
				.description = "play animations of raising the weapon to the sights and lowering it, for the player: files in an AnimGroupOverride folder named like the aiming down the sights animation they belong to with _start (hip to sights) or _stop (sights to hip) in the name (2haaimis_start.kf, 1hpaimis_stop_a.kf; _1stperson folders for the first person arms). One plays in place of the blend between the aim and aimIS animations and goes on to the animation it leads to at its End text key, with its own blend out. Aim released or pressed again partway plays the animation of the other way from where the weapon is, or the same one backwards when there is none (its sound and script text keys are skipped while it plays backwards). The weapon fires once the animation has ended. In third person they follow the pitch like an animation with an aimPitch text key. Requires bBlendSmoothing and bFixSpineBlendBug." },
			{ .section = "General", .key = "bFixAttackISTransition", .value = &s.fixAttackISTransition, .defaultValue = 1,
				.label = "Blend sights attacks to hip", .category = kBlending, .apply = Apply::Live,
				.summary = "Blends to the hip attack when aim is released mid-attack.",
				.description = "fix iron sight attack anims being glued to player's face even after player has released aim button. kNVSE will trigger a blend to the hipfire attack animation when the attack key is released." },
			{ .section = "Blend Fixes", .key = "bFixBlendSamePriority", .value = &s.fixBlendSamePriority, .defaultValue = 1,
				.label = "Fix same priority flicker", .category = kBlending, .apply = Apply::Live,
				.summary = "Fixes flicker between weapon animations of the same priority.",
				.description = "try to fix blending weapon animations with same bone priorities causing flickering as game tries to blend them with bones with the next high priority anim (usually mtidle.kf)" },
			{ .section = "Anim Fixes", .key = "bFixEndKeyTimeShorterThanStopTime", .value = &s.fixEndKeyTimeShorterThanStopTime, .defaultValue = 1,
				.label = "Fix stop time past end key", .category = kAnimFixes, .apply = Apply::NewAnimations,
				.summary = "Fixes animations whose stop time is past their end key.",
				.description = "try to fix animations with broken export stop time where it's greater than the end key time and time of last transform data" },
			{ .section = "Anim Fixes", .key = "bFixWrongAKeyInRespectEndKeyAnim", .value = &s.fixWrongAKeyInRespectEndKeyAnim, .defaultValue = 1,
				.label = "Fix wrong a: keys", .category = kAnimFixes, .apply = Apply::NewAnimations,
				.summary = "Fixes bad a: text keys in 1st person animations.",
				.description = "try to fix animations where animator messed up the a: text key value in the first person animation. Previous versions of kNVSE ignored this key in first person animations." },
			{ .section = "Anim Fixes", .key = "bFixWrongPrnKey", .value = &s.fixWrongPrnKey, .defaultValue = 1,
				.label = "Fix wrong prn keys", .category = kAnimFixes, .apply = Apply::NewAnimations,
				.summary = "Fixes bad prn text keys in 1st person animations.",
				.description = "try to fix animations where animator messed up the prn text key value in the first person animation. Previous versions of kNVSE ignored this key in first person animations." },
			{ .section = "Anim Fixes", .key = "bFixWrongAnimName", .value = &s.fixWrongAnimName, .defaultValue = 1,
				.label = "Fix wrong animation names", .category = kAnimFixes, .apply = Apply::NewAnimations,
				.summary = "Fixes animations whose name doesn't match their anim group.",
				.description = "try to fix animations where the name of the animation file does not match anim group name." },
			{ .section = "Anim Fixes", .key = "bFixMissingPrnKey", .value = &s.fixMissingPrnKey, .defaultValue = 1,
				.label = "Fix missing prn keys", .category = kAnimFixes, .apply = Apply::NewAnimations,
				.summary = "Adds missing prn keys to 1st person animations.",
				.description = "try to fix animations where the prn key is missing in the first person animation." },
			{ .section = "Anim Fixes", .key = "bFixReloadStartAllowReloadTweak", .value = &s.fixReloadStartAllowReloadTweak, .defaultValue = 1,
				.label = "Fix Allow Reload In Attack", .category = kGameFixes, .apply = Apply::Live,
				.summary = "Fixes looping reloads with Stewie's Allow Reload In Attack.",
				.description = "fix looping reloads in Stewie Tweak \"Allow Reload In Attack\" when attacking when attack is done when Aim is EaseIn and ReloadXStart becomes TransDest." },
			{ .section = "Anim Fixes", .key = "bFixReloadStance", .value = &s.fixReloadStance, .defaultValue = 1,
				.label = "Switch reload stance at once", .category = kGameFixes, .apply = Apply::Live,
				.summary = "Sneaking or standing up mid-reload switches the reload's stance at once.",
				.description = "fix the body staying in the old stance when sneaking or standing up during a reload in third person: the game only switches the reload to the new stance's animation when it starts the next reload (or round of a looping reload, e.g. the Cowboy Repeater, whose legs sink into the ground or float above it until then), while the camera moves at once. kNVSE blends to it at once." },
			{ .section = "Anim Fixes", .key = "bFixShortGaitBlend", .value = &s.fixShortGaitBlend, .defaultValue = 1,
				.label = "Fix too short walk blends", .category = kAnimFixes, .apply = Apply::NewAnimations,
				.summary = "Raises walk and run blends shorter than 0.2 s to 0.2 s.",
				.description = "fix walk and run animations that blend in or out much faster than the others, e.g. the vanilla third person 1hpfastforward.kf and 2hlfastforward.kf (Blend: 3, a tenth of a second, where the other walks and runs take 6 or 9 frames): Blend, BlendIn and BlendOut text keys of a walk or run below 6 frames (0.2 seconds, the game's default blend) are changed to 6 when the animation loads. Start and stop animations and KFs with a noFix text key keep theirs." },
			{ .section = "Anim Fixes", .key = "sLegacyAnimTimePaths", .value = &s.legacyAnimTimePathsText, .defaultText = "B42Inject,B42Interact,B42Loot",
				.label = "Legacy anim time paths", .category = kAnimFixes, .apply = Apply::Live,
				.summary = "Mods that rely on the anim time of old kNVSE versions.",
				.description = "use legacy anim time algorithm for these paths (these mods rely on bugged behavior from previous versions of kNVSE)." },
			{ .section = "Engine Fixes", .key = "bFixLoopingReloads", .value = &s.fixLoopingReloads, .defaultValue = 1,
				.label = "Fix looping reloads", .category = kGameFixes, .apply = Apply::Live,
				.summary = "Fixes bugs of looping reloads.",
				.description = "see https://www.youtube.com/watch?v=Vnh2PG-D15A" },
			{ .section = "Engine Fixes", .key = "bFixModReloadAnims", .value = &s.fixModReloadAnims, .defaultValue = 1,
				.label = "Fix extended magazine reloads", .category = kGameFixes, .apply = Apply::Restart,
				.summary = "Fixes reloads of weapons whose magazine mod changes the reload type.",
				.description = "fix weapons whose Increase Clip Capacity mod switches the reload animation between a looping (ReloadW-Z) and a non-looping one: the reload did not play at all, or its loop played only once" },
			{ .section = "Engine Fixes", .key = "bFixNPCLoopingReloads", .value = &s.fixNPCLoopingReloads, .defaultValue = 1,
				.label = "NPCs load one round per loop", .category = kGameFixes, .apply = Apply::Restart,
				.summary = "NPCs load looping reloads one round at a time, like the player.",
				.description = "fix NPCs loading the whole clip of a weapon with a looping reload (Cowboy Repeater, lever-action shotgun, ...) in one round: they load one round per loop, like the player. Needs xNVSE 6.4.9 or newer." },
			{ .section = "Engine Fixes", .key = "bFixAccumRootBlend", .value = &s.fixAccumRootBlend, .defaultValue = 1,
				.label = "Fix root motion at blends", .category = kGameFixes, .apply = Apply::Live,
				.summary = "Fixes actors stalling for a frame when root motion blends start or end.",
				.description = "fix actors stalling for a frame (or slowing down for one) at the start and end of blends between animations that move them by their root motion: the game's blend of the root's movement lost a frame of it whenever an animation joined or left the blend. Vanilla walks and runs hide it (the game moves the actor at the walk's average speed while a walk blends in); start and stop animations (bMovementTransitions) do not." },
			{ .section = "Engine Fixes", .key = "bFixAimUpDownVariants", .value = &s.fixAimUpDownVariants, .defaultValue = 1,
				.label = "Fix aim pitch of some weapons", .category = kGameFixes, .apply = Apply::Restart,
				.summary = "Fixes 3rd person aim not following the pitch for some weapon types.",
				.description = "fix third person aim and attack animations not following the pitch (looking up or down) for weapon types whose up/down KFs are in more than one archive: the game failed to find the up and down animations that go with the playing one when a KF is listed twice (2haaimup in Update.bsa: the Service Rifle, Assault Carbine, ...; 2hh via Old World Blues, h2h via Dead Money, 1hpattack8 via Update.bsa) or a group has several KFs." },
			{ .section = "Engine Fixes", .key = "bFixIdleStrafing", .value = &s.fixIdleStrafing, .defaultValue = 1,
				.label = "Strafe during idles", .category = kGameFixes, .apply = Apply::Restart,
				.summary = "Lets the player strafe and turn during idle animations.",
				.description = "allow player to strafe/turn sideways mid idle animation" },
			{ .section = "Blend Fixes", .key = "bFixBlendAnimMultipliers", .value = &s.fixBlendAnimMultipliers, .defaultValue = 1,
				.label = "Scale blends with anim speed", .category = kBlending, .apply = Apply::Live,
				.summary = "Blend times follow animation speed multipliers.",
				.description = "fix blend times not being affected by animation multipliers (fixes animations playing twice in 1st person when an anim multiplier is big)" },
		};

		constexpr size_t kNumSettings = std::size(s_settings);
		// Each setting's value as text: when the game started (what its hooks were written for) and as last read from
		// or written to the INI
		std::string s_loadedText[kNumSettings];
		std::string s_savedText[kNumSettings];

		size_t IndexOf(const Setting& setting)
		{
			return &setting - s_settings;
		}

		std::string_view ShortName(std::string_view key)
		{
			key.remove_prefix(1);
			if (key.size() > 3 && sv::starts_with_ci(key, "fix"))
				key.remove_prefix(3);
			return key;
		}

		double Clamp(const Setting& setting, double number)
		{
			if (setting.IsBool())
				return number != 0.0 ? 1.0 : 0.0;
			if (setting.min < setting.max)
				return std::clamp(number, setting.min, setting.max);
			return number;
		}

		void OnChanged(const Setting& setting)
		{
			if (setting.value == decltype(setting.value)(&g_pluginSettings.legacyAnimTimePathsText))
			{
				const auto& text = g_pluginSettings.legacyAnimTimePathsText;
				g_pluginSettings.legacyAnimTimePaths = text.empty() ? std::vector<std::string>() : SplitString(text);
			}
		}

		void SetDefault(const Setting& setting)
		{
			if (setting.IsString())
				*std::get<std::string*>(setting.value) = setting.defaultText;
			else
				SetNumber(setting, setting.defaultValue);
		}

		// The value as the code before the settings table wrote it: floats with six decimals
		void Write(CSimpleIniA& ini, const Setting& setting, bool withComment)
		{
			const auto comment = withComment ? std::format("; {}", setting.description) : std::string();
			const char* commentText = withComment ? comment.c_str() : nullptr;
			std::visit([&]<typename T>(T* value)
			{
				if constexpr (std::is_same_v<T, std::string>)
					ini.SetValue(setting.section, setting.key, value->c_str(), commentText);
				else if constexpr (std::is_same_v<T, float>)
					ini.SetDoubleValue(setting.section, setting.key, *value, commentText);
				else
					ini.SetLongValue(setting.section, setting.key, static_cast<long>(*value), commentText);
			}, setting.value);
		}
	}

	std::span<const Setting> All()
	{
		return s_settings;
	}

	const Setting* Find(std::string_view name)
	{
		for (const auto& setting : s_settings)
		{
			if (sv::equals_ci(name, setting.key) || sv::equals_ci(name, ShortName(setting.key)))
				return &setting;
		}
		return nullptr;
	}

	void Load()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(kIniPath) < 0)
			ERROR_LOG("INI file missing, generating a new one.");
		for (const auto& setting : s_settings)
		{
			const auto index = IndexOf(setting);
			const char* text = ini.GetValue(setting.section, setting.key, nullptr);
			if (!text)
			{
				SetDefault(setting);
				Write(ini, setting, true);
			}
			else if (setting.IsString())
				*std::get<std::string*>(setting.value) = text;
			else if (std::holds_alternative<float*>(setting.value))
				SetNumber(setting, ini.GetDoubleValue(setting.section, setting.key, setting.defaultValue));
			else
				SetNumber(setting, ini.GetLongValue(setting.section, setting.key, static_cast<long>(setting.defaultValue)));
			OnChanged(setting);
			s_loadedText[index] = s_savedText[index] = GetText(setting);
		}
		if (ini.SaveFile(kIniPath, false) < 0)
			ERROR_LOG("Failed to save ini file");
	}

	bool Save()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile(kIniPath);
		bool changed = false;
		for (const auto& setting : s_settings)
		{
			const auto index = IndexOf(setting);
			auto text = GetText(setting);
			if (text == s_savedText[index])
				continue;
			Write(ini, setting, !ini.GetValue(setting.section, setting.key));
			s_savedText[index] = std::move(text);
			changed = true;
		}
		if (!changed)
			return true;
		if (ini.SaveFile(kIniPath, false) < 0)
		{
			ERROR_LOG("Failed to save ini file");
			return false;
		}
		return true;
	}

	double GetNumber(const Setting& setting)
	{
		return std::visit([]<typename T>(T* value) -> double
		{
			if constexpr (std::is_same_v<T, std::string>)
				return 0.0;
			else
				return static_cast<double>(*value);
		}, setting.value);
	}

	std::string GetText(const Setting& setting)
	{
		return std::visit([]<typename T>(T* value) -> std::string
		{
			if constexpr (std::is_same_v<T, std::string>)
				return *value;
			else if constexpr (std::is_same_v<T, bool>)
				return *value ? "1" : "0";
			else
				return std::format("{}", *value);
		}, setting.value);
	}

	void SetNumber(const Setting& setting, double number)
	{
		number = Clamp(setting, number);
		std::visit([&]<typename T>(T* value)
		{
			if constexpr (std::is_same_v<T, bool>)
				*value = number != 0.0;
			else if constexpr (std::is_same_v<T, int>)
				*value = static_cast<int>(std::lround(number));
			else if constexpr (std::is_same_v<T, float>)
				*value = static_cast<float>(number);
		}, setting.value);
		OnChanged(setting);
	}

	bool SetText(const Setting& setting, std::string_view text)
	{
		if (setting.IsString())
		{
			*std::get<std::string*>(setting.value) = text;
			OnChanged(setting);
			return true;
		}
		text = sv::trim(text);
		double number = 0.0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
		if (error != std::errc() || end != text.data() + text.size())
			return false;
		SetNumber(setting, number);
		return true;
	}

	void ResetToDefault(const Setting& setting)
	{
		SetDefault(setting);
		OnChanged(setting);
	}

	bool IsDefault(const Setting& setting)
	{
		if (setting.IsString())
			return *std::get<std::string*>(setting.value) == setting.defaultText;
		return GetNumber(setting) == Clamp(setting, setting.defaultValue)
			|| std::holds_alternative<float*>(setting.value) && static_cast<float>(GetNumber(setting)) == static_cast<float>(setting.defaultValue);
	}

	bool IsRequirementMet(const Setting& setting)
	{
		return std::ranges::all_of(setting.requirements, [](const char* key)
		{
			const auto* required = key ? Find(key) : nullptr;
			return !required || GetNumber(*required) != 0.0;
		});
	}

	bool NeedsRestart(const Setting& setting)
	{
		const auto& loaded = s_loadedText[IndexOf(setting)];
		switch (setting.apply)
		{
		case Apply::Restart:
			return GetText(setting) != loaded;
		case Apply::RestartToEnable:
			return GetNumber(setting) != 0.0 && loaded == "0";
		default:
			return false;
		}
	}
}
