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

#include "knvse_version.h"
#include "LambdaVariableContext.h"
#include "nihooks.h"
#include "NiObjects.h"
#include "SimpleINILibrary.h"
#include "blend_fixes.h"
#include "knvse_events.h"

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
std::deque<std::function<void()>> g_executionQueue;
ExpressionEvaluatorUtils s_expEvalUtils;

std::unordered_map<std::string, std::vector<CustomAnimGroupScript>> g_customAnimGroups;

#if RUNTIME
NVSEScriptInterface* g_script;
NVSEArrayVarInterface* g_arrayVarInterface = nullptr;
NVSEStringVarInterface* g_stringVarInterface = nullptr;
#endif

_CaptureLambdaVars CaptureLambdaVars;
_UncaptureLambdaVars UncaptureLambdaVars;

bool isEditor = false;

float GetAnimTime(const AnimData* animData, const NiControllerSequence* anim)
{
	auto time = anim->offset + animData->timePassed;
	if (anim->startTime != -FLT_MAX)
	{
		if (anim->state == NiControllerSequence::kAnimState_TransSource || anim->state == NiControllerSequence::kAnimState_EaseOut)
		{
			time = anim->endTime - animData->timePassed;
		}
		else if (anim->state == NiControllerSequence::kAnimState_TransDest || anim->state == NiControllerSequence::kAnimState_EaseIn)
		{
			time = animData->timePassed - anim->startTime;
		}
	}
	return time;
}

bool g_fixHolster = false;
BSAnimGroupSequence* g_fixHolsterUnequipAnim3rd = nullptr;

static std::unordered_map<std::string, std::pair<float*, int>> s_anim3rdKeys;

void Revert3rdPersonAnimTimes(AnimTime& animTime, BSAnimGroupSequence* anim)
{
	if (!animTime.anim3rdCounterpart)
		return;
	if (const auto iter3rd = s_anim3rdKeys.find(animTime.anim3rdCounterpart->sequenceName); iter3rd != s_anim3rdKeys.end())
	{
		auto [keys, numKeys] = iter3rd->second;
		if (animTime.anim3rdCounterpart)
		{
			animTime.anim3rdCounterpart->animGroup->numKeys = numKeys;
			animTime.anim3rdCounterpart->animGroup->keyTimes = keys;
		}
	}
	const auto* animGroup = anim->animGroup;
	if (animGroup && (animGroup->groupID & 0xFF) == kAnimGroup_Unequip && !g_fixHolster)
	{
		g_fixHolster = true;
		g_fixHolsterUnequipAnim3rd = animTime.anim3rdCounterpart;
	}
	animTime.povState = POVSwitchState::POV3rd;
}

void HandleAnimTimes()
{
	constexpr auto shouldErase = [](Actor* actor)
	{
		return !actor || actor->IsDying(true) || actor->IsDeleted() || !actor->baseProcess;
	};
	const auto timeTrackedAnims = std::map(g_timeTrackedAnims);
	for (const auto& timeTrackedAnim : timeTrackedAnims)
	{
		auto& animTime = *timeTrackedAnim.second;
		auto* anim = animTime.anim;
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(animTime.actorId), TESForm, Actor);

		const auto erase = [&]
		{
			if (!animTime.cleanUpScripts.empty())
			{
				for (const auto& [cleanUpScript, path] : animTime.cleanUpScripts)
				{
					g_script->CallFunctionAlt(cleanUpScript, actor, 2, path.c_str(), animTime.firstPerson);
				}
			}
			g_timeTrackedAnims.erase(timeTrackedAnim.first);
		};

		if (!anim || !anim->animGroup)
		{
			erase();
			continue;
		}
		auto* groupInfo = GetGroupInfo(anim->animGroup->groupID);
#if _DEBUG
		auto animTimeDupl = TempObject(animTime); // see vals in debugger after erase
#endif
		if (!actor)
		{
			erase();
			continue;
		}
		auto* animData = animTime.GetAnimData(actor);
		
		if (shouldErase(actor) || !animData
			// respectEndKey has a special case for calling erase()
			||
			// !animTime.respectEndKey && 
			(anim->state == NiControllerSequence::kAnimState_Inactive || animData->animSequence[groupInfo->sequenceType] != anim))
		{
			erase();
			continue;
		}

		const auto time = GetAnimTime(animData, anim);
	
		if (animTime.respectEndKey)
		{
			const auto revert3rdPersonAnimTimes = [&]()
			{
				Revert3rdPersonAnimTimes(animTime, anim);
			};
			const auto* current3rdPersonAnim = g_thePlayer->Get3rdPersonAnimData()->animSequence[groupInfo->sequenceType];

			const auto current3rdPersonAnimHasChanged = _L(, current3rdPersonAnim != animTime.anim3rdCounterpart);
			const auto animHasEnded = _L(, anim->state == NiControllerSequence::kAnimState_Inactive);
			if (current3rdPersonAnimHasChanged() && animHasEnded())
			{
				erase();
				continue;
			}
			if (animTime.povState == POVSwitchState::NotSet)
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
				animTime.anim3rdCounterpart = anim3rd;
				s_anim3rdKeys.emplace(anim3rd->sequenceName, std::make_pair(anim3rd->animGroup->keyTimes, anim3rd->animGroup->numKeys));
			}
			if (!animTime.anim3rdCounterpart)
			{
				erase();
				continue;
			}
			if (g_thePlayer->IsThirdPerson() && animTime.povState != POVSwitchState::POV3rd)
			{
				revert3rdPersonAnimTimes();
			}
			else if (!g_thePlayer->IsThirdPerson() && animTime.povState != POVSwitchState::POV1st)
			{
				animTime.anim3rdCounterpart->animGroup->numKeys = anim->animGroup->numKeys;
				animTime.anim3rdCounterpart->animGroup->keyTimes = anim->animGroup->keyTimes;
				animTime.povState = POVSwitchState::POV1st;
			}
		}
		if (animTime.callbacks.Exists())
		{
			animTime.callbacks.Update(time, animData, [](const std::function<void()>& callback)
			{
				callback();
			});
		}
		if (animTime.scriptCalls.Exists())
		{
			animTime.scriptCalls.Update(time, animData, _L(Script* script, g_script->CallFunction(script, actor, nullptr, nullptr, 0)));
		}
		if (animTime.soundPaths.Exists())
		{
			animTime.soundPaths.Update(time, animData, [&](Sound& sound)
			{
				if (!IsPlayersOtherAnimData(animData))
				{
					sound.Set3D(actor);
					sound.Play();
				}
			});
		}
		if (animTime.scriptLines.Exists())
		{
			animTime.scriptLines.Update(time, animData, [&](Script* script)
			{
				ThisStdCall<bool>(0x5AC1E0, script, actor, actor->GetEventList(), nullptr, true);
			});
		}

		if (animTime.hasCustomAnimGroups)
		{
			const auto basePath = GetAnimBasePath(animTime.anim->sequenceName);
			if (auto iter = g_customAnimGroupPaths.find(basePath); iter != g_customAnimGroupPaths.end())
			{
				const auto& animPaths = iter->second;
				for (const auto& animPath : animPaths)
				{
					const auto name = ExtractCustomAnimGroupName(animPath);
					if (name.empty())
						continue;
					if (auto scriptsIter = g_customAnimGroups.find(ToLower(name)); scriptsIter != g_customAnimGroups.end())
					{
						auto& scripts = scriptsIter->second;
						for (auto& script : scripts)
						{
							g_script->CallFunctionAlt(*script.script, actor, 2, animPath.c_str(), animTime.firstPerson);
							if (*script.cleanUpScript)
							{
								animTime.cleanUpScripts.emplace(std::make_pair(*script.cleanUpScript, animPath));
							}
						}
					}
				}
			}
		}
	}

	const auto timeTrackedGroups = std::map(g_timeTrackedGroups); // copy to avoid iterator invalidation
	for (auto& timeTrackedGroup : timeTrackedGroups)
	{
		const auto erase = [&]
		{
			g_timeTrackedGroups.erase(timeTrackedGroup.first);
		};
		const auto& animTime = *timeTrackedGroup.second;
		auto& [conditionScript, groupId, realGroupId, anim, actorId, animData] = animTime;
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(actorId), TESForm, Actor);
		if (!actor)
		{
			erase();
			continue;
		}
		
		if (shouldErase(actor) || !animData || anim && anim->state == NiControllerSequence::kAnimState_Inactive && anim->cycleType != NiControllerSequence::LOOP || !conditionScript)
		{
			erase();
			continue;
		}
		
		if (!animData)
		{
			erase();
			continue;
		}

		const auto* animInfo = GetGroupInfo(groupId);
		const auto* curAnim = animData->animSequence[animInfo->sequenceType];
		if (!curAnim || !curAnim->animGroup)
		{
			erase();
			continue;
		}

		const auto isCurrentlyFirstPerson = animData == g_thePlayer->firstPersonAnimData;

		// check if current anim is running at sequence type
		if ((curAnim->animGroup->groupID & 0xFF) != (groupId & 0xFF))
		{
			erase();
			continue;
		}
		
		if (conditionScript)
		{
			const auto currentRealAnimGroupId = GetActorRealAnimGroup(actor, curAnim->animGroup->GetBaseGroupID());
			const auto curHandType = (curAnim->animGroup->groupID & 0xf00) >> 8;
			const auto handType = (groupId & 0xf00) >> 8;
			if (
				handType >= curHandType &&
				(realGroupId == currentRealAnimGroupId || realGroupId == curAnim->animGroup->groupID))
			{
				NVSEArrayVarInterface::Element arrResult;
				if (g_script->CallFunction(conditionScript, actor, nullptr, &arrResult, 0))
				{
					const auto result = static_cast<bool>(arrResult.GetNumber());
					const auto customAnimState = anim ? anim->state : kAnimState_Inactive;
					if (customAnimState == kAnimState_Inactive && result && curAnim != anim
						|| customAnimState != kAnimState_Inactive && !result && curAnim == anim)
					{
						// group id may have changed now that udf returns false
						const auto groupIdFit = GameFuncs::GetActorAnimGroupId(actor, groupId & 0xFF, nullptr, false, animData);
						GameFuncs::PlayAnimGroup(animData, groupIdFit, 1, -1, -1);
					}
				}
			}
			else
			{
				erase();
			}
		}
	}
}

bool IsGodMode()
{
	const bool* godMode = reinterpret_cast<bool*>(0x11E07BA);
	return *godMode;
}

void HandleBurstFire()
{
	auto iter = g_burstFireQueue.begin();
	while (iter != g_burstFireQueue.end())
	{
		auto& [firstPerson, anim, index, hitKeys, _, shouldEject, lastNiTime, actorId, ejectKeys, ejectIdx, reloading] = *iter;
		const auto erase = [&]()
		{
			iter = g_burstFireQueue.erase(iter);
		};
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(actorId), TESForm, Actor);
		if (!actor || actor->IsDeleted() || actor->IsDying(true) || !anim || !anim->animGroup || anim->lastScaledTime - lastNiTime < -0.01f && anim->cycleType != NiControllerSequence::LOOP)
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
		lastNiTime = anim->lastScaledTime;
		auto* currentAnim = animData->animSequence[kSequence_Weapon];
		auto* weapon = actor->GetWeaponForm();
		if (currentAnim != anim)
		{
			erase();
			continue;
		}
		//timePassed += GetTimePassed(animData, anim->animGroup->groupID);
		const auto timePassed = GetAnimTime(animData, anim);
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
					if (DidActorReload(actor, ReloadSubscriber::BurstFire) || ammoInfo->count == 0 || actor->IsAnimActionReload())
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

void HandleExecutionQueue()
{
	while (!g_executionQueue.empty())
	{
		g_executionQueue.front()();
		g_executionQueue.pop_front();
	}
}

void HandleMisc()
{
	if (g_fixHolster && g_thePlayer->IsThirdPerson())
	{
		auto* anim = g_thePlayer->GetHighProcess()->animData->animSequence[kSequence_Weapon];
		if (!anim)
		{
			if (g_fixHolsterUnequipAnim3rd->state == NiControllerSequence::kAnimState_Inactive)
			{
				g_fixHolster = false;
				ThisStdCall(0x9231D0, g_thePlayer->baseProcess, false, g_thePlayer->validBip01Names, g_thePlayer->baseProcess->GetAnimData(), g_thePlayer);
			}
		}
		else if (anim->animGroup->GetBaseGroupID() != kAnimGroup_Unequip)
			g_fixHolster = false;
	}
}


void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		g_thePlayer = *(PlayerCharacter **)0x011DEA3C;
		LoadFileAnimPaths();
		Console_Print("kNVSE version %d", VERSION_MAJOR);
	}
	else if (msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
		const auto isMenuMode = CdeclCall<bool>(0x702360);
		if (!isMenuMode)
		{
			HandleOnActorReload();
			HandleBurstFire();
			HandleAnimTimes();
			HandleExecutionQueue();
			HandleMisc();
#if 0 // experimental fixes
			if (g_fixAttackISTransition)
			{
				BlendFixes::ApplyAttackISToAttackFix();
				BlendFixes::ApplyAttackToAttackISFix();
			}
#endif
		}
	}
	else if (msg->type == NVSEMessagingInterface::kMessage_PostLoadGame)
	{
		// HandleGarbageCollection();
		g_partialLoopReloadState = PartialLoopingReloadState::NotSet;
		g_reloadTracker.clear();
		g_timeTrackedAnims.clear();
		g_timeTrackedGroups.clear();
	}
}


bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	_MESSAGE("query");

	// fill out the info structure
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "kNVSE";
	info->version = VERSION_MAJOR;
	Log("kNVSE version " + std::to_string(VERSION_MAJOR));

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

bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
	g_pluginHandle = nvse->GetPluginHandle();
	g_nvseInterface = (NVSEInterface*)nvse;
	g_messagingInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	g_messagingInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);
	g_script = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
	g_arrayVarInterface = (NVSEArrayVarInterface*)nvse->QueryInterface(kInterface_ArrayVar);
	g_stringVarInterface = (NVSEStringVarInterface*)nvse->QueryInterface(kInterface_StringVar);

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

	NVSEDataInterface* nvseData = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data);
	CaptureLambdaVars = (_CaptureLambdaVars)nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList);
	UncaptureLambdaVars = (_UncaptureLambdaVars)nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList);

	ApplyHooks();
	Events::RegisterEvents();
	return true;
}
