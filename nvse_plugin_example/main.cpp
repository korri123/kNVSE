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

#define RegisterScriptCommand(name) 	nvse->RegisterCommand(&kCommandInfo_ ##name)

IDebugLog		gLog("kNVSE.log");

PluginHandle	g_pluginHandle = kPluginHandle_Invalid;

NVSEMessagingInterface* g_messagingInterface;
NVSEEventManagerInterface* g_eventManagerInterface;
NVSEInterface* g_nvseInterface;
NVSECommandTableInterface* g_cmdTable;
const CommandInfo* g_TFC;
PlayerCharacter* g_player;
std::deque<std::function<void()>> g_executionQueue;

#if RUNTIME
NVSEScriptInterface* g_script;
NVSEArrayVarInterface* g_arrayVarInterface = nullptr;
NVSEStringVarInterface* g_stringVarInterface = nullptr;
#endif

_CaptureLambdaVars CaptureLambdaVars;
_UncaptureLambdaVars UncaptureLambdaVars;

bool isEditor = false;

float GetAnimTime(const AnimData* animData, const BSAnimGroupSequence* anim)
{
	auto time = anim->offset + animData->timePassed;
	if (anim->state == NiControllerSequence::kAnimState_TransDest)
	{
		time = anim->endTime - animData->timePassed;
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
		const auto erase = [&]()
		{
			g_timeTrackedAnims.erase(timeTrackedAnim.first);
		};

		auto& animTime = *timeTrackedAnim.second;
		auto* anim = animTime.anim;
		if (!anim || !anim->animGroup)
		{
			erase();
			continue;
		}
		auto* groupInfo = GetGroupInfo(anim->animGroup->groupID);
#if _DEBUG
		auto animTimeDupl = TempObject(animTime); // see vals in debugger after erase
#endif
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(animTime.actorId), TESForm, Actor);
		if (!actor)
		{
			erase();
			continue;
		}
		auto* animData = animTime.GetAnimData(actor);
		
		if (shouldErase(actor) || !animData
			// respectEndKey has a special case for calling erase()
			|| !animTime.respectEndKey && (anim->state == NiControllerSequence::kAnimState_Inactive || animData->animSequence[groupInfo->sequenceType] != anim))
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
			const auto* currentWeapon = actor->GetWeaponForm();
			
			if (currentWeapon != animTime.actorWeapon
				|| !current3rdPersonAnim || !current3rdPersonAnim->animGroup
				|| animTime.anim3rdCounterpart && current3rdPersonAnim->animGroup->groupID != animTime.anim3rdCounterpart->animGroup->groupID)
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
			animTime.scriptLines.Update(time, animData, _L(Script* script, ThisStdCall(0x5AC1E0, script, actor, actor->GetEventList(), nullptr, false)));
		}
	}


	const auto timeTrackedGroups = std::map(g_timeTrackedGroups); // copy to avoid iterator invalidation
	for (const auto& timeTrackedGroup : timeTrackedGroups)
	{
		const auto erase = [&]
		{
			g_timeTrackedGroups.erase(timeTrackedGroup.first);
		};
		auto& [seqType, animTime] = timeTrackedGroup;
		auto& [conditionScript, groupId, realGroupId, anim, actorId, lastNiTime, firstPerson] = animTime;
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(actorId), TESForm, Actor);
		if (!actor)
		{
			erase();
			continue;
		}
		auto* animData = firstPerson ? g_thePlayer->firstPersonAnimData : actor->baseProcess->GetAnimData();
		if (shouldErase(actor) || anim && anim->state == NiControllerSequence::kAnimState_Inactive && anim->cycleType != NiControllerSequence::LOOP || !conditionScript || !animData)
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
		if (conditionScript)
		{
			const auto currentRealAnimGroupId = GetActorRealAnimGroup(actor, curAnim->animGroup->GetBaseGroupID());
			const auto curHandType = (curAnim->animGroup->groupID & 0xf00) >> 8;
			const auto handType = (groupId & 0xf00) >> 8;
			if (handType >= curHandType && (realGroupId == currentRealAnimGroupId || realGroupId == curAnim->animGroup->groupID))
			{
				NVSEArrayVarInterface::Element arrResult;
				if (g_script->CallFunction(conditionScript, actor, nullptr, &arrResult, 0))
				{
					const auto result = static_cast<bool>(arrResult.Number());
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

float g_destFrame = -FLT_MAX;
void HandleProlongedAim()
{
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	auto* animData1st = g_thePlayer->firstPersonAnimData;
	auto* highProcess = g_thePlayer->GetHighProcess();
	auto* curWeaponAnim = animData3rd->animSequence[kSequence_Weapon];
	/*static bool waitUntilReload = false;
	static bool doOnce = false;
	if (!doOnce)
	{
		SubscribeOnActorReload(g_thePlayer, ReloadSubscriber::AimTransition);
		doOnce = true;
	}
	if (waitUntilReload)
	{
		if (!g_thePlayer->IsAnimActionReload())
			return;
		waitUntilReload = false;
	}*/
	if (!curWeaponAnim)
		return;
	const auto curGroupId = curWeaponAnim->animGroup->groupID;
	UInt16 destId;
	bool toIS = false;
	if (curWeaponAnim->animGroup->IsAttackIS() && !highProcess->isAiming)
	{
		destId = curGroupId - 3; // hipfire
		ThisStdCall(0x8BB650, g_thePlayer, false, false, false);
	}
	else if (curWeaponAnim->animGroup->IsAttackNonIS() && GameFuncs::GetControlState(ControlCode::Aim, Decoding::IsDXKeyState::IsHeld))
	{
		destId = curGroupId + 3;
		ThisStdCall(0x8BB650, g_thePlayer, true, false, false);
		toIS = true;
		//g_test = true;
		//ThisStdCall(0xA24280, *g_OSInputGlobals, ControlCode::Aim);
	}
	else
		return;
	auto* weapon = g_thePlayer->GetWeaponForm();
	if (!weapon)
		return;
	auto* ammoInfo = g_thePlayer->baseProcess->GetAmmoInfo();
	if (!ammoInfo)
		return;
	
	/*if (DidActorReload(g_thePlayer, ReloadSubscriber::AimTransition))
	{
		waitUntilReload = true;
		return;
	}*/
	
	for (auto* animData : {animData3rd, animData1st})
	{
		auto* base = animData->mapAnimSequenceBase->Lookup(destId);
		auto* mult = DYNAMIC_CAST(base, AnimSequenceBase, AnimSequenceMultiple);

		const auto getAnim = [&](UInt16 groupId)
		{
			auto* anim = GetGameAnimation(animData, groupId);
			BSAnimGroupSequence* overrideAnim;
			std::optional<AnimationResult> overrideAnimPath;
			if ((overrideAnimPath = GetActorAnimation(groupId, animData == animData1st, animData)) && (overrideAnim = LoadAnimationPath(*overrideAnimPath, animData, groupId)))
				anim = overrideAnim;
			return anim;
		};
		auto* destAnim = getAnim(destId);
		auto* sourceAnim = animData->animSequence[kSequence_Weapon];

		if (!destAnim || !sourceAnim)
			return;

		const auto applyStartTime = [&](BSAnimGroupSequence* seq)
		{
			//seq->destFrame = sourceAnim->lastScaledTime / destAnim->frequency;
			g_destFrame = sourceAnim->lastScaledTime / destAnim->frequency;
			seq->kNVSEFlags |= NiControllerSequence::kFlag_DestframeStartTime; // start at the destFrameTime
		};

		//applyStartTime(destAnim);
		std::span blocks{ destAnim->controlledBlocks, destAnim->numControlledBlocks };
		
		const auto increasePriority = _L(NiControllerSequence::ControlledBlock & block, block.blendInterpolator ? block.blendInterpolator->m_cHighPriority += 1 : 0);
		const auto decreasePriority = _L(NiControllerSequence::ControlledBlock & block, block.blendInterpolator ? block.blendInterpolator->m_cHighPriority -= 1 : 0);

		const auto oldBlend = destAnim->animGroup->blendIn;
		if (!toIS)
			destAnim->animGroup->blendIn = 8;
		else
			destAnim->animGroup->blendIn = 8;

		if (toIS)
		{
			animData->timePassed = 0;
			GameFuncs::DeactivateSequence(animData->controllerManager, destAnim, 0.0f);
		}
		/*/if (toIS)
		{
			PatchMemoryNop(0xA350AC, 6);
			PatchMemoryNop(0xA350B4, 3);
		}*/
		//animData->noBlend120 = true;
		const auto applyStartTimesForAll = [&]()
		{
			if (mult)
			{
				mult->anims->ForEach([&](BSAnimGroupSequence* seq)
				{
					/*std::span b{seq->controlledBlocks, seq->numControlledBlocks};
					if (!toIS)
						ra::for_each(b, increasePriority);
					else
						ra::for_each(b, decreasePriority);*/
					applyStartTime(seq);
				});
			}
			else
			{
				/*if (!toIS)
					ra::for_each(blocks, increasePriority);
				else
					ra::for_each(blocks, decreasePriority);*/
				applyStartTime(destAnim);
			}
		};
		
		
		//GameFuncs::PlayAnimGroup(animData, hipfireId, 1, -1, -1);
		/*if (animData == animData3rd)
		{
			auto* upAnim = getAnim(hipfireId + 1);
			auto* downAnim = getAnim(hipfireId + 2);
			highProcess->animSequence[0] = hipfireAnim;
			highProcess->animSequence[1] = upAnim;
			highProcess->animSequence[2] = downAnim;
			applyStartTime(upAnim);
			applyStartTime(downAnim);
			GameFuncs::MorphToSequence(animData, upAnim, hipfireId, -1);
			GameFuncs::MorphToSequence(animData, downAnim, hipfireId, -1);
		}*/
		//GameFuncs::MorphToSequence(animData, hipfireAnim, hipfireId, -1);
		//ThisStdCall(0x49BCA0, animData, animData->timePassed, 0, 1);

		if (animData == animData1st)
		{
			applyStartTimesForAll();
			ThisStdCall(0x9520F0, g_thePlayer, destId, 1);
		}
		else
		{
			applyStartTimesForAll();
			auto id = highProcess->isAiming ? destId - 3 : destId; // function already increments this
			ThisStdCall(0x8B28C0, g_thePlayer, (id & 0xFF), animData3rd);
			/*if (destAnim->destFrame < destAnim->animGroup->keyTimes[kSeqState_EjectOrUnequipEnd])
			{
				animData->actor->baseProcess->SetQueuedIdleFlag(kIdleFlag_AttackEjectEaseInFollowThrough);
			
				GameFuncs::HandleQueuedAnimFlags(animData->actor);
			}*/
		}

		/*if (toIS)
		{
			SafeWriteBuf(0xA350AC, "\xD9\x05\x5C\x5F\x01\x01", 6);
			SafeWriteBuf(0xA350B4, "\xD9\x5E\x54", 3);
		}*/
		//GameFuncs::PlayAnimGroup(animData, hipfireId, 1, -1, -1);
		
		//ra::for_each(blocks, _L(NiControllerSequence::ControlledBlock & block, block.blendInterpolator ? block.blendInterpolator->m_cHighPriority -= 10 : 0));
		/*if (mult)
		{
			mult->anims->ForEach([&](BSAnimGroupSequence* seq)
			{
				std::span b{ seq->controlledBlocks, seq->numControlledBlocks };
				if (!toIS)
					ra::for_each(b, decreasePriority);
				else
					ra::for_each(b, increasePriority);
			});
		}
		else
		{
			if (!toIS)
				ra::for_each(blocks, decreasePriority);
			else
				ra::for_each(blocks, increasePriority);
		}*/

		destAnim->animGroup->blendIn = oldBlend;
	}
	highProcess->SetCurrentActionAndSequence(destId, GetGameAnimation(animData3rd, destId));
}

void HandleExecutionQueue()
{
	while (!g_executionQueue.empty())
	{
		g_executionQueue.front()();
		g_executionQueue.pop_front();
	}
}

NiNode* g_weaponBone = nullptr;

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

#if _DEBUG

std::ofstream out("_upperarm.csv");
bool g_init = false;
std::string g_cur;

#if 0
void LogNodes(NiNode* node)
{
	for (auto iter = node->m_children.Begin(); iter; ++iter)
	{
		std::span s{ reinterpret_cast<float*>(&iter->m_localRotate), 13 };
		if (g_init)
		{
			g_cur += FormatString("%g,", std::accumulate(s.begin(), s.end(), 0.0));
		}
		else
		{
			g_cur += std::string(iter->m_pcName) + ',';
		}
		if (auto* niNode = iter->GetAsNiNode())
			LogNodes(niNode);
	}
}

#endif

void LogNodes(NiNode* node)
{
	for (auto iter = node->m_children.Begin(); iter; ++iter)
	{
		if (_stricmp(iter->m_pcName, "Bip01 L UpperArm") == 0)
		{
			std::span s{ reinterpret_cast<float*>(&iter->m_localRotate), 13 };
			for (auto f : s)
			{
				g_cur += FormatString("%g,", f);
			}
		}
		if (auto* niNode = iter->GetAsNiNode())
			LogNodes(niNode);
	}
}

void HandleDebug()
{
	if (!g_weaponBone)
	{
		g_weaponBone = g_thePlayer->baseProcess->GetWeaponBone(g_thePlayer->validBip01Names);
	}
}
#endif

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
			// HandleGarbageCollection(); this causes FileIO on each knvse anim play, no perf impact noticed on SSD
#if _DEBUG
			HandleDebug();
#endif
#if IS_TRANSITION_FIX
			HandleProlongedAim();
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

	nvse->SetOpcodeBase(0x3920);
	
	RegisterScriptCommand(ForcePlayIdle);
	RegisterScriptCommand(SetWeaponAnimationPath);
	RegisterScriptCommand(SetActorAnimationPath);
	RegisterScriptCommand(PlayAnimationPath);
	RegisterScriptCommand(ForceStopIdle);
	RegisterScriptCommand(kNVSEReset);
	RegisterScriptCommand(PlayGroupAlt);
	RegisterScriptCommand(CreateIdleAnimForm);
	RegisterScriptCommand(EjectWeapon);
	RegisterScriptCommand(SetAnimationTraitNumeric);
	RegisterScriptCommand(GetAnimationTraitNumeric);
	nvse->RegisterTypedCommand(&kCommandInfo_GetAnimationByPath, kRetnType_Array);
	nvse->RegisterTypedCommand(&kCommandInfo_GetActorAnimation, kRetnType_String);

#if _DEBUG
	RegisterScriptCommand(kNVSETest);
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
	ApplyNiHooks();
	return true;
}
