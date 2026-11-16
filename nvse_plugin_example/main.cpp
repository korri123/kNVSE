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
#include "gamebryo/NiStream.h"

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

void HandleCustomTextKeys()
{
	// std::unique_lock lock(g_animTimeMutex);
	// not needed since this runs on MainGameLoop event which is before any threads are active
	DebugAssert(!AILinearTaskManager::ShouldQueue3DTask());
	for (auto it = g_timeTrackedAnims.begin(); it != g_timeTrackedAnims.end();)
	{
		auto& animTime = *it->second;
		BSAnimGroupSequence* anim = animTime.anim;
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(animTime.actorId), TESForm, Actor);

		// ~AnimTime eases the animation out as part of the Animation's current switch, once the Animation is known.
		AnimData* animDataOfEase = nullptr;
		const auto erase = [&]
		{
			if (!animTime.cleanUpScripts.empty())
			{
				for (const auto& [cleanUpScript, path] : animTime.cleanUpScripts)
				{
					g_script->CallFunctionAlt(cleanUpScript, actor, 2, path.c_str(), animTime.firstPerson);
				}
			}
			const NiPointer<BSAnimGroupSequence> endedAnim = anim;
			it = g_timeTrackedAnims.erase(it);
			if (animDataOfEase && endedAnim)
				AnimSwitch::Current(animDataOfEase).Add(endedAnim);
		};

		if (!anim)
		{
			erase();
			continue;
		}
		SetThisAnimScriptPath setThisAnimScriptPath(anim);
		auto* groupInfo = anim->animGroup ? GetGroupInfo(static_cast<AnimGroupID>(anim->animGroup->groupID)) : nullptr;
#if _DEBUG
		auto animTimeDupl = TempObject(animTime); // see vals in debugger after erase
#endif
		if (!actor || !actor->baseProcess || !actor->Get3D())
		{
			erase();
			continue;
		}
		auto* animData = animTime.GetAnimData(actor);

		const auto isAnimPlaying = [&]
		{
			if (groupInfo && animTime.endIfSequenceTypeChanges)
				return anim->m_eState != kAnimState_Inactive && animData->animSequence[groupInfo->sequenceType] == anim;
			return anim->m_eState != kAnimState_Inactive;
		};
		
		if (IsActorInvalid(actor) || !animData)
		{
			erase();
			continue;
		}
		animDataOfEase = animData;

		const auto time = animTime.useLegacyTime ? GetAnimTimeLegacy(animData, anim) : GetAnimTime(anim);
	
		if (animTime.respectEndKey && anim->animGroup)
		{
			const auto* current3rdPersonAnim = g_thePlayer->Get3rdPersonAnimData()->animSequence[groupInfo->sequenceType];
			auto& respectEndKeyData = animTime.respectEndKeyData;
			if (current3rdPersonAnim != respectEndKeyData.anim3rdCounterpart && anim->m_eState == kAnimState_Inactive)
			{
				erase();
				continue;
			}
			if (!respectEndKeyData.anim3rdCounterpart)
			{
				const auto sequenceId = anim->animGroup->GetGroupInfo()->sequenceType;
				auto* anim3rd = g_thePlayer->baseProcess->GetAnimData()->animSequence[sequenceId];
				if (!anim3rd || !anim3rd->animGroup || (anim3rd->animGroup->groupID & 0xFF) != (anim->animGroup->groupID & 0xFF))
				{
					anim3rd = anim->Get3rdPersonCounterpart();
				}
				if (!anim3rd || !anim3rd->animGroup)
				{
					erase();
					continue;
				}
				respectEndKeyData.anim3rdCounterpart = anim3rd;
			}
			// In 3rd person nothing applies the times; RespectEndKey::EndFrame gives the sequence its own times back.
			if (!g_thePlayer->IsThirdPerson())
			{
				// A sequence that has ended keeps its times on the 3rd person sequence until that changes, but does not
				// take them back from another 1st person animation of the group that started since.
				if (anim->m_eState != kAnimState_Inactive)
					RespectEndKey::Apply(respectEndKeyData.anim3rdCounterpart, anim);
				else
					RespectEndKey::Keep(respectEndKeyData.anim3rdCounterpart);
			}
		}
		if (!isAnimPlaying() && animTime.respectEndKey)
		{
			++it;
			continue; // we don't want text keys to apply on the pollCondition anim here but we need to track it until 3rd person has changed anim
		}
		// An aim transition plays backwards. Its time would look like the animation restarting on every frame, so the
		// keys below would fire again each frame.
		const bool bTimedKeys = !AimTransitions::IsPlayedBack(anim);
		if (bTimedKeys && animTime.callbacks.Exists())
		{
			animTime.callbacks.Update(time, animData, [](const std::function<void()>& callback)
			{
				callback();
			});
		}
		if (bTimedKeys && animTime.scriptCalls.Exists())
		{
			animTime.scriptCalls.Update(time, animData, _L(Script* script, g_script->CallFunction(script, actor, nullptr, nullptr, 0)));
		}
		if (bTimedKeys && animTime.soundPaths.Exists())
		{
			animTime.soundPaths.Update(time, animData, [&](Sounds& sound)
			{
				if (!IsPlayersOtherAnimData(animData))
				{
					const auto is3D = animData != g_thePlayer->firstPersonAnimData;
					sound.Play(actor, is3D);
				}
			});
		}
		if (bTimedKeys && animTime.scriptLines.Exists())
		{
			const auto callback = [&](Script* script)
			{
				ThisStdCall<bool>(0x5AC1E0, script, actor, actor->GetEventList(), nullptr, true);
			};
			if (animTime.useLegacyTime) // prevent getting stuck in b42 interact 
				animTime.scriptLines.UpdateLegacy(time, animData, callback);	
			else
				animTime.scriptLines.Update(time, animData, callback);	
		}

		if (animTime.hasCustomAnimGroups)
		{
			const auto basePath = GetAnimBasePath(animTime.anim->m_kName.Str());
			{
				// pooled paths
				for (const std::string_view animPath : Overrides::GetCustomGroupPaths(basePath))
				{
					const auto name = ExtractCustomAnimGroupName(animPath);
					if (name.empty())
						continue;
					if (auto scriptsIter = g_customAnimGroups.find(ToLower(name)); scriptsIter != g_customAnimGroups.end())
					{
						auto& scripts = scriptsIter->second;
						for (auto& script : scripts)
						{
							g_script->CallFunctionAlt(*script.script, actor, 2, animPath.data(), animTime.firstPerson);
							if (*script.cleanUpScript)
							{
								animTime.cleanUpScripts.emplace(std::make_pair(*script.cleanUpScript, std::string(animPath)));
							}
						}
					}
				}
			}
		}

		if (animTime.isOverlayAdditiveAnim)
		{
			const auto refGroupId = animTime.referenceAnimGroupId;
			auto* currentAnim = animData->animSequence[kSequence_Weapon];
			if (!currentAnim || !currentAnim->animGroup || currentAnim->animGroup->groupID != refGroupId)
			{
				erase();
				continue;
			}
			const auto baseGroupId = static_cast<AnimGroupID>(refGroupId);
			if (baseGroupId == kAnimGroup_AttackLoop || baseGroupId == kAnimGroup_AttackLoopIS)
			{
				if (!GameFuncs::GetControlState(ControlCode::Attack, Decoding::IsDXKeyState::IsHeld))
				{
					erase();
					continue;
				}

				const auto* ammoInfo = actor->baseProcess->GetAmmoInfo();
				if (!ammoInfo && !IsGodMode())
				{
					erase();
					continue;
				}

				if (ammoInfo && ammoInfo->count == 0 && !IsGodMode())
				{
					erase();
					continue;
				}
			}
		}
		
		// respectEndKey has a special case for calling erase() so rapidly firing variants where 1st person anim is shorter than 3rdp will work
		// moved below so that keys on end frame are handled correctly (functional backpack by mrshersh)
		if (!animTime.respectEndKey && !isAnimPlaying())
		{
			erase();
			continue;
		}
		if (animTime.trackEndTime && time >= animTime.anim->m_fEndKeyTime && anim->m_eState != NiControllerSequence::EASEIN && anim->m_eState != NiControllerSequence::TRANSDEST)
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

void HandleAnimTimes()
{
	HandleCustomTextKeys();
	Overrides::UpdatePolled();
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

void HandleBurstFire()
{
	auto iter = g_burstFireQueue.begin();
	while (iter != g_burstFireQueue.end())
	{
		auto& [firstPerson, anim, index, hitKeys, _, shouldEject, lastNiTime, actorId, ejectKeys, ejectIdx, reloading, burstAnimData] = *iter;
		const auto erase = [&]()
		{
			iter = g_burstFireQueue.erase(iter);
		};
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(actorId), TESForm, Actor);
		if (!actor || actor->IsDeleted() || actor->IsDead(true) || !anim || !anim->animGroup || anim->m_fLastScaledTime - lastNiTime < -0.01f && anim->m_eCycleType != NiControllerSequence::LOOP)
		{
			erase();
			continue;
		}
		auto* animData = firstPerson ? g_thePlayer->firstPersonAnimData : actor->baseProcess->GetAnimData();
		if (!animData)
		{
			erase();
			continue;
		}
		lastNiTime = anim->m_fLastScaledTime;
		auto* currentAnim = animData->animSequence[kSequence_Weapon];
		auto* weapon = actor->GetWeaponForm();
		if (currentAnim != anim)
		{
			erase();
			continue;
		}
		//timePassed += GetTimePassed(animData, anim->animGroup->groupID);
		const auto timePassed = anim->m_fLastScaledTime;
		if (timePassed <= anim->animGroup->keyTimes[kSeqState_HitOrDetach])
		{
			// first hit handled by engine
			// don't want duplicated shootings
			++iter;
			continue;
		}
		const auto passedHitKey = index < hitKeys.size() && timePassed > hitKeys.at(index)->m_fTime;
		const auto passedEjectKey = ejectIdx < ejectKeys.size() && !ejectKeys.empty() && ejectIdx < ejectKeys.size() && timePassed > ejectKeys.at(ejectIdx)->m_fTime;
		if (passedHitKey || passedEjectKey)
		{
			if (auto* ammoInfo = actor->baseProcess->GetAmmoInfo()) // static_cast<Decoding::MiddleHighProcess*>(animData->actor->baseProcess)->ammoInfo
			{
				if (!IsGodMode())
				{
					//const auto clipSize = GetWeaponInfoClipSize(actor);
					if (ammoInfo->count == 0 || actor->IsAnimActionReload())
					{
						// reloaded
						reloading = true;
					}
#if 0
					const auto ammoCount = ammoInfo->countDelta;
					if (weapon && (ammoCount == 0 || ammoCount == clipSize))
					{
						erase();
						continue;
					}
#endif
				}
			}
			if (passedHitKey)
				++index;
			if (!IsPlayersOtherAnimData(animData))
			{
				bool ejected = false;
				if (passedHitKey)
				{
					if (!reloading)
						actor->FireWeapon();
					if (!passedEjectKey && ejectKeys.empty() || ejectIdx == ejectKeys.size())
					{
						if (!reloading)
							actor->EjectFromWeapon(weapon);
						ejected = true;
					}
				}
				if (!ejected && passedEjectKey)
				{
					actor->EjectFromWeapon(weapon);
					++ejectIdx;
				}
			}
		}
		
		if (index < hitKeys.size() || !ejectKeys.empty() && ejectIdx < ejectKeys.size())
			++iter;
		else
			erase();
	}
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

void HandleMisc()
{
	OnReloadHandler::Update();
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
		g_thePlayer = *reinterpret_cast<PlayerCharacter**>(0x011DEA3C);
		FileAnimations::StartLoading();
		CheckAndWarnForFixPack();
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
			HandleBurstFire();
			HandleAnimTimes();
			HandleMisc();
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
		//g_thePlayer->baseProcess->GetWeaponBone();
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
