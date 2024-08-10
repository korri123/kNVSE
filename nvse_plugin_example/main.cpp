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
#include "commands_misc.h"
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
NVSEConsoleInterface* g_consoleInterface = nullptr;
#endif

_CaptureLambdaVars CaptureLambdaVars;
_UncaptureLambdaVars UncaptureLambdaVars;
std::vector<std::string> g_eachFrameScriptLines;

bool isEditor = false;

float GetAnimTime(const AnimData* animData, const NiControllerSequence* anim)
{
	if (anim->m_fEndTime != -FLT_MAX && (anim->m_eState == kAnimState_TransSource || anim->m_eState == kAnimState_EaseOut))
	{
		return anim->m_fEndTime - animData->timePassed;
	}
	if (anim->m_fStartTime != -FLT_MAX && (anim->m_eState == kAnimState_TransDest || anim->m_eState == kAnimState_EaseIn))
	{
		return animData->timePassed - anim->m_fStartTime;
	}
	return anim->m_fOffset + animData->timePassed;
}

bool CallFunction(Script* funcScript, TESObjectREFR* callingObj, TESObjectREFR* container,
	NVSEArrayVarInterface::Element* result)
{
	return g_script->CallFunction(
		funcScript,
		callingObj,
		container,
		result,
		0
	);
}

bool g_fixHolster = false;
BSAnimGroupSequence* g_fixHolsterUnequipAnim3rd = nullptr;

struct ThirdPersonSavedData
{
	UInt32 numKeys{};
	float* keyTimes{};
	UInt8 blendIn{};
	UInt8 blendOut{};
	UInt8 blend{};
};
std::unordered_map<BSAnimGroupSequence*, ThirdPersonSavedData> g_thirdPersonSavedData;

void Save3rdPersonAnimGroupData(BSAnimGroupSequence* anim3rd)
{
	ThirdPersonSavedData thirdPersonSavedData{
		.numKeys = anim3rd->animGroup->numKeys,
		.keyTimes = anim3rd->animGroup->keyTimes,
		.blendIn = anim3rd->animGroup->blendIn,
		.blendOut = anim3rd->animGroup->blendOut,
		.blend = anim3rd->animGroup->blend,
	};
	g_thirdPersonSavedData.try_emplace(anim3rd, thirdPersonSavedData);
}

void Set3rdPersonAnimTimes(BSAnimGroupSequence* anim3rd, BSAnimGroupSequence* anim1st)
{
	auto* animGroup3rd = anim3rd->animGroup;
	auto* animGroup1st = anim1st->animGroup;
	if (!animGroup3rd || !animGroup1st)
		return;
	animGroup3rd->numKeys = animGroup1st->numKeys;
	animGroup3rd->keyTimes = animGroup1st->keyTimes;
	animGroup3rd->blend = animGroup1st->blend;
	animGroup3rd->blendIn = animGroup1st->blendIn;
	animGroup3rd->blendOut = animGroup1st->blendOut;
}

void Revert3rdPersonAnimTimes(BSAnimGroupSequence* anim3rd, BSAnimGroupSequence* anim1st)
{
	if (!anim3rd || !anim3rd->animGroup)
		return;
	if (const auto iter = g_thirdPersonSavedData.find(anim3rd); iter != g_thirdPersonSavedData.end())
	{
		const auto& savedData = iter->second;
		anim3rd->animGroup->numKeys = savedData.numKeys;
		anim3rd->animGroup->keyTimes = savedData.keyTimes;
		anim3rd->animGroup->blend = savedData.blend;
		anim3rd->animGroup->blendIn = savedData.blendIn;
		anim3rd->animGroup->blendOut = savedData.blendOut;
	}
	const auto* animGroup = anim1st->animGroup;
	if (animGroup && (animGroup->groupID & 0xFF) == kAnimGroup_Unequip && !g_fixHolster)
	{
		g_fixHolster = true;
		g_fixHolsterUnequipAnim3rd = anim3rd;
	}
}

bool IsAnimNextInSelection(AnimData* animData, UInt16 nearestGroupId, BSAnimGroupSequence* anim)
{
	const auto ctx = GetActorAnimation(nearestGroupId, animData);
	if (!ctx)
	{
		auto* animBase = animData->mapAnimSequenceBase->Lookup(nearestGroupId);
		if (!animBase)
			return false;
		return animBase->Contains(anim);
	}
	return ctx->parent->linkedSequences.contains(anim);
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

		const auto isAnimPlaying = _L(, anim->m_eState != kAnimState_Inactive && animData->animSequence[groupInfo->sequenceType] == anim);
		
		if (shouldErase(actor) || !animData
			// respectEndKey has a special case for calling erase() so rapidly firing variants where 1st person anim is shorter than 3rdp will work
			|| (!animTime.respectEndKey && !isAnimPlaying()))
		{
			erase();
			continue;
		}

		const auto time = GetAnimTime(animData, anim);
	
		if (animTime.respectEndKey)
		{
			const auto* current3rdPersonAnim = g_thePlayer->Get3rdPersonAnimData()->animSequence[groupInfo->sequenceType];

			const auto current3rdPersonAnimHasChanged = _L(, current3rdPersonAnim != animTime.respectEndKeyData.anim3rdCounterpart);
			const auto animHasEnded = _L(, anim->m_eState == kAnimState_Inactive);
			if (current3rdPersonAnimHasChanged() && animHasEnded())
			{
				erase();
				continue;
			}
			auto& respectEndKeyData = animTime.respectEndKeyData;
			if (respectEndKeyData.povState == POVSwitchState::NotSet)
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
				Save3rdPersonAnimGroupData(anim3rd);
			}
			if (!respectEndKeyData.anim3rdCounterpart)
			{
				erase();
				continue;
			}
			if (g_thePlayer->IsThirdPerson() && respectEndKeyData.povState != POVSwitchState::POV3rd)
			{
				Revert3rdPersonAnimTimes(respectEndKeyData.anim3rdCounterpart, anim);
				respectEndKeyData.povState = POVSwitchState::POV3rd;
			}
			else if (!g_thePlayer->IsThirdPerson() && respectEndKeyData.povState != POVSwitchState::POV1st)
			{
				Set3rdPersonAnimTimes(respectEndKeyData.anim3rdCounterpart, anim);
				respectEndKeyData.povState = POVSwitchState::POV1st;
			}
		}
		if (!isAnimPlaying())
			continue; // we don't want text keys to apply on the pollCondition anim here but we need to track it until 3rd person has changed anim
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
			const auto basePath = GetAnimBasePath(animTime.anim->m_kName);
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
		auto& [conditionScript, groupId, anim, actorId, animData] = animTime;
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(actorId), TESForm, Actor);
		if (!actor)
		{
			erase();
			continue;
		}
		
		if (shouldErase(actor) || !animData || !anim || anim->m_eState == kAnimState_Inactive && anim->m_eCycleType != NiControllerSequence::LOOP || !conditionScript)
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
		auto* curAnim = animData->animSequence[animInfo->sequenceType];
		if (!curAnim || !curAnim->animGroup)
		{
			erase();
			continue;
		}

		// check if current anim is running at sequence type
		if ((curAnim->animGroup->groupID & 0xFF) != (groupId & 0xFF))
		{
			erase();
			continue;
		}
		
		if (conditionScript)
		{
			NVSEArrayVarInterface::Element arrResult;
			if (CallFunction(conditionScript, actor, nullptr, &arrResult))
			{
				const auto result = static_cast<bool>(arrResult.GetNumber());
				const auto animGroupId = static_cast<AnimGroupID>(groupId & 0xFF);
				const auto nextGroupId = GetNearestGroupID(animData, animGroupId);
				const auto isAnimNextAnim = IsAnimNextInSelection(animData, nextGroupId, anim);
				const auto isCurAnimNextAnim = IsAnimNextInSelection(animData, nextGroupId, curAnim);
				
				const auto shouldPlayAnim = _L(, result && isAnimNextAnim && !isCurAnimNextAnim);
				const auto shouldStopAnim = _L(, !result && !isAnimNextAnim && curAnim == anim);
				if (shouldPlayAnim() || shouldStopAnim())
				{
					GameFuncs::PlayAnimGroup(animData, nextGroupId, 1, -1, -1);
				}
			}
#if 0
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
						//if (groupIdFit != curAnim->animGroup->groupID)
							GameFuncs::PlayAnimGroup(animData, groupIdFit, 1, -1, -1);
					}
				}
			}
			else
			{
				erase();
			}
#endif
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
		if (!actor || actor->IsDeleted() || actor->IsDying(true) || !anim || !anim->animGroup || anim->m_fLastScaledTime - lastNiTime < -0.01f && anim->m_eCycleType != NiControllerSequence::LOOP)
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
			if (g_fixHolsterUnequipAnim3rd->m_eState == kAnimState_Inactive)
			{
				g_fixHolster = false;
				ThisStdCall(0x9231D0, g_thePlayer->baseProcess, false, g_thePlayer->validBip01Names, g_thePlayer->baseProcess->GetAnimData(), g_thePlayer);
			}
		}
		else if (anim->animGroup->GetBaseGroupID() != kAnimGroup_Unequip)
			g_fixHolster = false;
	}
	g_animationResultCache.clear();
	g_animPathFrameCache.clear();
	for (const auto& line : g_eachFrameScriptLines)
	{
		g_consoleInterface->RunScriptLine(line.c_str(), nullptr);
	}
}


void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		//WIN32_FIND_DATA data;
		//std::string path = "Data\\Meshes\\AnimGroupOverride\\*.kf";
		//auto result = FindFirstFileA(path.c_str(), &data);
		//auto* list = CdeclCall<tList<const char>*>(0xAFE420, path.c_str(), "Data\\Meshes\\AnimGroupOverride\\IdleAnim", 1, 0);
		NiHooks::WriteDelayedHooks();
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
	g_consoleInterface = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);

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

	NVSEDataInterface* nvseData = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data);
	CaptureLambdaVars = (_CaptureLambdaVars)nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaSaveVariableList);
	UncaptureLambdaVars = (_UncaptureLambdaVars)nvseData->GetFunc(NVSEDataInterface::kNVSEData_LambdaUnsaveVariableList);

	ApplyHooks();
	Events::RegisterEvents();
	return true;
}
