#include "main.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "commands_animation.h"
#include "hooks.h"
#include "utility.h"
#include <filesystem>
#include <fstream>
#include <numeric>
#include <span>

#include "file_animations.h"
#include "GameData.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
#include "game_types.h"
#include <set>
#include <thread>

#include "knvse_version.h"
#include "LambdaVariableContext.h"
#include "nihooks.h"
#include "NiObjects.h"
#include "SimpleINILibrary.h"
#include "aim_transitions.h"
#include "anim_switch.h"
#include "blend_fixes.h"
#include "movement_transitions.h"
#if _DEBUG
#include "commands_misc.h"
#endif
#include "knvse_events.h"
#include "knvse_menu.h"
#include "respect_end_key.h"
#include "spawn_keys.h"
#include "debug_tools.h"
#include "text_keys.h"
#include "gamebryo/NiStream.h"
#include "burst_fire.h"
#include "weapon_fire.h"

#define REG_CMD(name) 	nvse->RegisterCommand(&kCommandInfo_ ##name)
#define REG_CMD_TYPED(name, type) 	nvse->RegisterTypedCommand(&kCommandInfo_ ##name, type)

IDebugLog		gLog("kNVSE.log");

PluginHandle	g_pluginHandle = kPluginHandle_Invalid;

NVSEMessagingInterface* g_messagingInterface;
NVSEEventManagerInterface* g_eventManagerInterface;
NVSEInterface* g_nvseInterface;
NVSECommandTableInterface* g_cmdTable;
const CommandInfo* g_TFC;
PlayerCharacter* g_player;
ICriticalSection g_executionQueueCS;
std::deque<std::function<void()>> g_synchronizedExecutionQueue;
ExpressionEvaluatorUtils s_expEvalUtils;

std::unordered_map<std::string, std::vector<CustomAnimGroupScript>> g_customAnimGroups;

#if RUNTIME
NVSEScriptInterface* g_script;
NVSEArrayVarInterface* g_arrayVarInterface = nullptr;
NVSEStringVarInterface* g_stringVarInterface = nullptr;
NVSEConsoleInterface* g_consoleInterface = nullptr;
NVSEDataInterface* g_nvseDataInterface = nullptr;
#endif

_CaptureLambdaVars CaptureLambdaVars;
_UncaptureLambdaVars UncaptureLambdaVars;
std::vector<std::string> g_eachFrameScriptLines;
MapHitCounters g_mapHitCounters;
AverageTimers g_averageTimers;

bool isEditor = false;

float GetAnimTime(const NiControllerSequence* anim)
{
	if (anim->m_eState == NiControllerSequence::TRANSDEST)
	{
		if (anim->m_fDestFrame != -NI_INFINITY)
			return anim->m_fDestFrame - 0.001f;
		return 0.0f;
	}
	if (anim->m_fLastTime != -NI_INFINITY)
		return anim->m_fLastScaledTime;
	return 0.0f;
}

float GetAnimTimeLegacy(const AnimData* animData, const NiControllerSequence* anim)
{
	auto time = anim->m_fOffset + animData->timePassed;
	if (anim->m_eState == NiControllerSequence::TRANSDEST)
	{
		time = anim->m_fEndTime - animData->timePassed;
	}
	return time;
}

namespace
{
	struct ScriptResult
	{
		TESObjectREFR* callingObj;
		Script* script;
		NVSEArrayVarInterface::Element value;
	};
	// Results are searched in order and the vector stays allocated across frames (a handful per frame). Entries are keyed
	// by the script itself rather than its compiled code: compiled code refers to forms by index into the script's own
	// list, so conditions naming different forms compile to identical code. CompileConditionScript already gives every
	// condition text its own script.
	thread_local std::vector<ScriptResult> t_scriptResults;
}

bool CallFunction(Script* funcScript, TESObjectREFR* callingObj, TESObjectREFR* container,
	NVSEArrayVarInterface::Element* result)
{
	const auto useCache = g_isThreadCacheEnabled;
	size_t cacheIndex = 0;
	if (useCache)
	{
		for (const auto& cached : t_scriptResults)
		{
			if (cached.callingObj == callingObj && cached.script == funcScript)
			{
				*result = cached.value;
				return true;
			}
		}
		// Set before the call. A condition that asks about the actor's animations can re-enter this function; the
		// re-entrant call answers false instead of recursing forever.
		cacheIndex = t_scriptResults.size();
		t_scriptResults.push_back({ callingObj, funcScript, NVSEArrayVarInterface::Element{} });
	}
	const auto wasInConditionFunction = g_globals.isInConditionFunction;
	g_globals.isInConditionFunction = true;
	const auto success = g_script->CallFunction(
		funcScript,
		callingObj,
		container,
		result,
		0
	);
	g_globals.isInConditionFunction = wasInConditionFunction;
	// Index into the vector, because calls made by the script may have added results.
	if (useCache && cacheIndex < t_scriptResults.size())
		t_scriptResults[cacheIndex].value = *result;
	return success;
}

bool IsActorInvalid(Actor* actor)
{
	return !actor || actor->IsDead(true) || actor->IsDeleted() || !actor->baseProcess || !actor->Get3D();
}

struct SetThisAnimScriptPath
{
	SetThisAnimScriptPath(BSAnimGroupSequence* anim)
	{
		g_globals.thisAnimScriptPath = anim->m_kName.Str();
	}

	~SetThisAnimScriptPath()
	{
		g_globals.thisAnimScriptPath = {};
	}
};

namespace
{
	// Whether whoever started the sequence still plays it
	bool IsPlaying(const AnimTime& animTime, const AnimGroupInfo* groupInfo)
	{
		const auto* anim = animTime.anim.data;
		if (anim->m_eState == kAnimState_Inactive)
			return false;
		// A group's sequence plays until another takes its section
		return animTime.player != AnimTime::Player::Group || !groupInfo || animTime.animData->animSequence[groupInfo->sequenceType] == anim;
	}

	// respectEndKey: the player's 1st person sequence gives its times to the 3rd person sequence of the play. False once
	// it has ended and the 3rd person sequence changed, or has no 3rd person sequence.
	bool UpdateRespectEndKey(AnimTime& animTime, const AnimGroupInfo* groupInfo)
	{
		auto* anim = animTime.anim.data;
		auto* player = PlayerCharacter::GetSingleton();
		const auto* current3rdPersonAnim = player->Get3rdPersonAnimData()->animSequence[groupInfo->sequenceType];
		auto& respectEndKeyData = animTime.respectEndKeyData;
		if (current3rdPersonAnim != respectEndKeyData.anim3rdCounterpart && anim->m_eState == kAnimState_Inactive)
			return false;
		if (!respectEndKeyData.anim3rdCounterpart)
		{
			const auto sequenceId = anim->animGroup->GetGroupInfo()->sequenceType;
			auto* anim3rd = player->baseProcess->GetAnimData()->animSequence[sequenceId];
			if (!anim3rd || !anim3rd->animGroup || (anim3rd->animGroup->groupID & 0xFF) != (anim->animGroup->groupID & 0xFF))
				anim3rd = anim->Get3rdPersonCounterpart();
			if (!anim3rd || !anim3rd->animGroup)
				return false;
			respectEndKeyData.anim3rdCounterpart = anim3rd;
		}
		// In 3rd person nothing applies the times; RespectEndKey::EndFrame gives the sequence its own times back.
		if (!player->IsThirdPerson())
		{
			// A sequence that has ended keeps its times on the 3rd person sequence until that changes, but does not
			// take them back from another 1st person animation of the group that started since.
			if (anim->m_eState != kAnimState_Inactive)
				RespectEndKey::Apply(respectEndKeyData.anim3rdCounterpart, anim);
			else
				RespectEndKey::Keep(respectEndKeyData.anim3rdCounterpart);
		}
		return true;
	}

	// The player's skeletons play a group together, each from its own KF, and the keys of both used to act: a 3rd
	// person reload that loads rounds at its own key times (Dri's Chinese Pistol, MrShersh's LoadRounds.gek) loaded them
	// in 1st person a second before the 1st person KF's key. Script keys of the skeleton not in view are left out where
	// the view's sequence of the group has keys of the same kind, so KFs made for one view keep working in the other.
	bool HasViewsOwnKeys(const AnimTime& animTime, TextKeys::Table::Flag kind)
	{
		const auto* anim = animTime.anim.data;
		if (!IsPlayersOtherAnimData(animTime.animData) || !anim->animGroup)
			return false;
		const auto* groupInfo = GetGroupInfo(static_cast<AnimGroupID>(anim->animGroup->groupID));
		if (!groupInfo)
			return false;
		auto* player = PlayerCharacter::GetSingleton();
		const auto* viewAnim = player->GetAnimData(!player->IsThirdPerson())->animSequence[groupInfo->sequenceType];
		if (!viewAnim || !viewAnim->animGroup || (viewAnim->animGroup->groupID & 0xFF) != (anim->animGroup->groupID & 0xFF))
			return false;
		const auto* keys = TextKeys::Find(*viewAnim);
		return keys && keys->Has(kind);
	}

	// Acts on the keys of the play that the time has passed
	void FireTimedKeys(AnimTime& animTime, Actor* actor, float time)
	{
		using enum TextKeys::Table::Flag;
		auto* animData = animTime.animData;
		auto* anim = animTime.anim.data;
		animTime.scriptCalls.Update(time, [&](Script* script)
		{
			if (!HasViewsOwnKeys(animTime, kScriptCall))
				g_script->CallFunction(script, actor, nullptr, nullptr, 0);
		});
		animTime.soundKeys.Update(time, [&](UInt32 index)
		{
			if (index < animTime.sounds.size() && !animTime.sounds[index].failed && !IsPlayersOtherAnimData(animData))
				animTime.sounds[index].Play(actor, animData != PlayerCharacter::GetSingleton()->firstPersonAnimData);
		});
		const auto runScriptLine = [&](Script* script)
		{
			if (!HasViewsOwnKeys(animTime, kScriptLine))
				ThisStdCall<bool>(0x5AC1E0, script, actor, actor->GetEventList(), nullptr, true);
		};
		if (animTime.useLegacyTime) // prevent getting stuck in b42 interact
			animTime.scriptLines.UpdateLegacy(time, runScriptLine);
		else
			animTime.scriptLines.Update(time, runScriptLine);
		if (animTime.ammoKeysAct)
			animTime.ammoKeys.Update(time, [&](const AmmoKeys::Key& key) { AmmoKeys::Fire(actor, key, anim); });
		if (animTime.burstKeys.Exists())
		{
			// Started over: the clip is the one it has now
			if (time < animTime.burstKeys.lastTime - 0.01f)
				BurstFire::Start(actor, animTime.burst);
			animTime.burstKeys.Update(time, [&](const BurstFire::Key& key) { BurstFire::Fire(actor, animData, anim, key, animTime.burst); });
		}
	}

	// The scripts of the custom anim groups (SetCustomAnimGroupScript) the sequence's KF is in, every frame
	void RunCustomAnimGroupScripts(AnimTime& animTime, Actor* actor)
	{
		const auto basePath = GetAnimBasePath(animTime.anim->m_kName.Str());
		// pooled paths
		for (const std::string_view animPath : Overrides::GetCustomGroupPaths(basePath))
		{
			const auto name = ExtractCustomAnimGroupName(animPath);
			if (name.empty())
				continue;
			const auto scriptsIter = g_customAnimGroups.find(ToLower(name));
			if (scriptsIter == g_customAnimGroups.end())
				continue;
			for (auto& script : scriptsIter->second)
			{
				g_script->CallFunctionAlt(*script.script, actor, 2, animPath.data(), animTime.firstPerson);
				if (*script.cleanUpScript)
					animTime.cleanUpScripts.emplace(std::make_pair(*script.cleanUpScript, std::string(animPath)));
			}
		}
	}

	// AdditiveOverlay: its reference group left the weapon section; over an attack loop also attack released or the
	// clip empty
	bool HasOverlayEnded(const AnimTime& animTime, Actor* actor)
	{
		const auto refGroupId = animTime.referenceAnimGroupId;
		const auto* currentAnim = animTime.animData->animSequence[kSequence_Weapon];
		if (!currentAnim || !currentAnim->animGroup || currentAnim->animGroup->groupID != refGroupId)
			return true;
		const auto baseGroupId = static_cast<AnimGroupID>(refGroupId);
		if (baseGroupId != kAnimGroup_AttackLoop && baseGroupId != kAnimGroup_AttackLoopIS)
			return false;
		if (!GameFuncs::GetControlState(ControlCode::Attack, Decoding::IsDXKeyState::IsHeld))
			return true;
		const auto* ammoInfo = actor->baseProcess->GetAmmoInfo();
		return !IsGodMode() && (!ammoInfo || ammoInfo->count == 0);
	}
}

void HandleCustomTextKeys()
{
	// No lock: this runs on MainGameLoop event which is before any threads are active
	DebugAssert(!AILinearTaskManager::ShouldQueue3DTask());
	for (auto it = g_timeTrackedAnims.begin(); it != g_timeTrackedAnims.end();)
	{
		auto& animTime = *it->second;
		BSAnimGroupSequence* anim = animTime.anim;
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(animTime.actorId), TESForm, Actor);

		// The Animation the play is on, once the actor is known to be valid
		AnimData* animDataOfEase = nullptr;
		const auto erase = [&]
		{
			for (const auto& [cleanUpScript, path] : animTime.cleanUpScripts)
				g_script->CallFunctionAlt(cleanUpScript, actor, 2, path.c_str(), animTime.firstPerson);
			const NiPointer<BSAnimGroupSequence> endedAnim = anim;
			const auto stateBefore = anim ? anim->m_eState : NiControllerSequence::INACTIVE;
			it = g_timeTrackedAnims.erase(it);
			// ~AnimTime eased it out (a play it owns to its end): part of the Animation's current switch. An ease that
			// was in progress belongs to the switch that started it, which can be an earlier one (the menus pause
			// tracking, not the Animations).
			if (animDataOfEase && endedAnim && endedAnim->m_eState != stateBefore)
				AnimSwitch::Current(animDataOfEase).Add(endedAnim);
		};

		if (!anim)
		{
			erase();
			continue;
		}
		SetThisAnimScriptPath setThisAnimScriptPath(anim);
		if (IsActorInvalid(actor))
		{
			erase();
			continue;
		}
		auto* animData = animTime.animData;
		animDataOfEase = animData;
		auto* groupInfo = anim->animGroup ? GetGroupInfo(static_cast<AnimGroupID>(anim->animGroup->groupID)) : nullptr;
		const auto time = animTime.useLegacyTime ? GetAnimTimeLegacy(animData, anim) : GetAnimTime(anim);

		if (animTime.respectEndKey && anim->animGroup && !UpdateRespectEndKey(animTime, groupInfo))
		{
			erase();
			continue;
		}
		// Tracked until the 3rd person sequence changes, without acting on keys (the pollCondition anim)
		if (animTime.respectEndKey && !IsPlaying(animTime, groupInfo))
		{
			++it;
			continue;
		}
		// An aim transition plays backwards. Its time would look like the animation restarting on every frame, so the
		// keys would fire again each frame.
		if (!AimTransitions::IsPlayedBack(anim))
			FireTimedKeys(animTime, actor, time);
		if (animTime.hasCustomAnimGroups)
			RunCustomAnimGroupScripts(animTime, actor);

		if (animTime.player == AnimTime::Player::AdditiveOverlay && HasOverlayEnded(animTime, actor))
		{
			erase();
			continue;
		}
		// After the keys, so that keys on the end frame act (Functional Backpack by mrshersh). respectEndKey plays end
		// above, so rapidly firing variants whose 1st person anim is shorter than the 3rd person one work.
		if (!animTime.respectEndKey && !IsPlaying(animTime, groupInfo))
		{
			erase();
			continue;
		}
		if (animTime.player == AnimTime::Player::ScriptToEnd && time >= anim->m_fEndKeyTime
			&& anim->m_eState != NiControllerSequence::EASEIN && anim->m_eState != NiControllerSequence::TRANSDEST)
		{
			erase();
			continue;
		}
		if (animTime.hasSpawnKeys)
			SpawnKeys::Update(animData, anim, time);
		++it;
	}
	SpawnKeys::EndFrame();
	RespectEndKey::EndFrame();
}

bool IsGodMode()
{
	const bool* godMode = reinterpret_cast<bool*>(0x11E07BA);
	return *godMode;
}

bool IsDemigodMode()
{
	// g_bIsDemigodMode or the bSetDemigodMode:GamePlay game setting
	return *reinterpret_cast<bool*>(0x11E07BB) || *reinterpret_cast<bool*>(0x11E0898);
}

void HandleSynchronizedExecutionQueue()
{
	ScopedLock lock(g_executionQueueCS);
	while (!g_synchronizedExecutionQueue.empty())
	{
		g_synchronizedExecutionQueue.front()();
		g_synchronizedExecutionQueue.pop_front();
	}
}

void ClearResultCaches()
{
	t_scriptResults.clear();
}

void SynchronizedQueue::Add(std::function<void()>&& func)
{
	std::unique_lock lock(mutex);
	queue.push_back(std::move(func));
}

void SynchronizedQueue::RunAllAndClear()
{
	// std::unique_lock lock(mutex);
	// Not needed
	DebugAssert(!AILinearTaskManager::ShouldQueue3DTask());
	while (!queue.empty())
	{
		queue.front()();
		queue.pop_front();
	}
}

static void CheckAndWarnForFixPack()
{
	constexpr auto path = R"(Data\NVSE\Plugins\scripts\ln_nvaofixpack.txt)";
	if (g_pluginSettings.fixAttackISTransition && std::filesystem::exists(path))
	{
		std::ifstream file(path);
		std::string content((std::istreambuf_iterator(file)), std::istreambuf_iterator<char>());
    
		if (content.contains("Player.PlayGroup AimIS 1"))
		{
			Script::RunScriptLine("MessageBoxEx \"kNVSE: Disable 'kNVSE Fixpack'.%R%RkNVSE has all the fixes now natively and this mod will interfere with them.\"");
		}
	}
}

void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	DebugTools::HandleMessage(msg->type);
	if (msg->type == NVSEMessagingInterface::kMessage_PostPostLoad)
	{
		// After every plugin has loaded (Stewie's Tweaks patches the start menu's calls there) and before the start menu
		// builds its lists.
		KnvseMenu::WriteHooks();
	}
	else if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		g_isThreadCacheEnabled = true;
		Console_Print("kNVSE version %d", VERSION_MAJOR);
		WriteDelayedHooks();
		FileAnimations::StartLoading();
		CheckAndWarnForFixPack();
	}
	else if (msg->type == NVSEMessagingInterface::kMessage_PreLoadGame)
	{
		AmmoKeys::Reset();
	}
	else if (msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
		// Condition results are cached for one frame, in menus too.
		ClearResultCaches();
		// Text conditions are compiled here on the main thread as soon as the files are registered.
		if (FileAnimations::CheckLoaded())
		{
			Overrides::CompileConditions();
			Overrides::UpdatePlayerPreload();
		}
		if (!Interface::IsInMenuMode())
		{
			// The first frame of play: Stewie's Tweaks runs its deferred init from the main loop once the menus exist,
			// after kNVSE's first kMessage_MainGameLoop, and the main menu is over before anything plays.
			static bool lateHooksWritten = false;
			if (!lateHooksWritten)
			{
				lateHooksWritten = true;
				AmmoKeys::WriteLateHooks();
			}
			WeaponFire::Update();
			HandleCustomTextKeys();
#if 1 // experimental fixes
			if (g_pluginSettings.fixAttackISTransition)
			{
				BlendFixes::ApplyAttackISToAttackFix();
				BlendFixes::ApplyAttackToAttackISFix();
			}
#endif
			if (g_pluginSettings.fixWeaponStance)
				BlendFixes::ApplyWeaponStanceFix();
			AimTransitions::Update();
		}
		HandleSynchronizedExecutionQueue();
		g_mainThreadQueue.RunAllAndClear(); // kMessage_MainGameLoop runs before AILinearTaskThreads start
	}
}


bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	_MESSAGE("query");

	// fill out the info structure
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "kNVSE";
	info->version = VERSION_MAJOR;
	LOG("kNVSE version " + std::to_string(VERSION_MAJOR));

	// version checks
	if (!nvse->isEditor && nvse->nvseVersion < PACKED_NVSE_VERSION)
	{
		const auto str = FormatString("kNVSE: xNVSE version too old (got %X expected at least %X). Plugin will NOT load! Install the latest version here: https://github.com/xNVSE/NVSE/releases/", nvse->nvseVersion, PACKED_NVSE_VERSION);
#if !_DEBUG
		ShowErrorMessageBox(str.c_str());
#endif
		_ERROR(str.c_str());
		return false;
	}

	if (!nvse->isEditor)
	{
		if (nvse->runtimeVersion < RUNTIME_VERSION_1_4_0_525)
		{
			_ERROR("incorrect runtime version (got %08X need at least %08X)", nvse->runtimeVersion, RUNTIME_VERSION_1_4_0_525);
			return false;
		}

		if (nvse->isNogore)
		{
			_ERROR("NoGore is not supported");
			return false;
		}
	}

	else
	{
		isEditor = true;
		if (nvse->editorVersion < CS_VERSION_1_4_0_518)
		{
			_ERROR("incorrect editor version (got %08X need at least %08X)", nvse->editorVersion, CS_VERSION_1_4_0_518);
			return false;
		}
	}
	return true;
}


int g_logLevel = 0;
int g_errorLogLevel = 0;

bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
	g_pluginHandle = nvse->GetPluginHandle();
	g_nvseInterface = (NVSEInterface*)nvse;
	g_messagingInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	g_messagingInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);
	g_script = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
	g_arrayVarInterface = (NVSEArrayVarInterface*)nvse->QueryInterface(kInterface_ArrayVar);
	g_stringVarInterface = (NVSEStringVarInterface*)nvse->QueryInterface(kInterface_StringVar);
	g_consoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
	g_cmdTable = (NVSECommandTableInterface*)nvse->QueryInterface(kInterface_CommandTable);

	nvse->InitExpressionEvaluatorUtils(&s_expEvalUtils);

	nvse->SetOpcodeBase(0x3920);
	
	REG_CMD(ForcePlayIdle);
	REG_CMD(SetWeaponAnimationPath);
	REG_CMD(SetActorAnimationPath);
	REG_CMD(PlayAnimationPath);
	REG_CMD(ForceStopIdle);
	REG_CMD(kNVSEReset);
	REG_CMD(PlayGroupAlt);
	REG_CMD(CreateIdleAnimForm);
	REG_CMD(EjectWeapon);

	NVSECommandBuilder commandBuilder(nvse);
	CreateCommands(commandBuilder);
#if _DEBUG
	Commands::BuildMiscCommands(commandBuilder);
#endif


#if _DEBUG
	REG_CMD(kNVSETest);
	if (false)
	{
		//PlayerCharacter::GetSingleton()->baseProcess->GetWeaponBone();
	}
#endif
	
	if (isEditor)
	{
		return true;
	}

	g_eventManagerInterface = (NVSEEventManagerInterface*)nvse->QueryInterface(kInterface_EventManager);

	//g_eventManagerInterface->SetNativeEventHandler("OnActorEquip", OnActorEquipEventHandler);

	g_nvseDataInterface = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data);
	CaptureLambdaVars = (_CaptureLambdaVars)g_nvseDataInterface->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList);
	UncaptureLambdaVars = (_UncaptureLambdaVars)g_nvseDataInterface->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList);

	ApplyHooks();
	Events::RegisterEvents();
	DebugTools::InstallAssertHook();
	DebugTools::StartCommandPipe();
	DebugTools::StartBackgroundMode();
	DebugTools::StartTestGame();
	return true;
}
