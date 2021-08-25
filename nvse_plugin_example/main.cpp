#include "main.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "commands_animation.h"
#include "hooks.h"
#include "utility.h"
#include <filesystem>
#include <span>

#include "file_animations.h"
#include "GameData.h"
#include "GameRTTI.h"
#include "SafeWrite.h"
#include "game_types.h"
#include <set>

#include "NiObjects.h"
#include "SimpleINILibrary.h"

#define RegisterScriptCommand(name) 	nvse->RegisterCommand(&kCommandInfo_ ##name)

IDebugLog		gLog("kNVSE.log");

PluginHandle	g_pluginHandle = kPluginHandle_Invalid;

NVSEMessagingInterface* g_messagingInterface;
NVSEInterface* g_nvseInterface;
NVSECommandTableInterface* g_cmdTable;
const CommandInfo* g_TFC;
PlayerCharacter* g_player;
std::deque<std::function<void()>> g_executionQueue;

#if RUNTIME
NVSEScriptInterface* g_script;
#endif

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

void HandleAnimTimes()
{
	constexpr auto shouldErase = [](Actor* actor)
	{
		return !actor || actor->IsDying(true) || actor->IsDeleted();
	};
	for (auto iter = g_timeTrackedAnims.begin(); iter != g_timeTrackedAnims.end();)
	{
		const auto erase = [&]() {iter = g_timeTrackedAnims.erase(iter); };
		auto& [anim, animTime] = *iter;
		//auto& time = animTime.time;

		auto* actor = DYNAMIC_CAST(LookupFormByRefID(animTime.actorId), TESForm, Actor);

		if (shouldErase(actor))
		{
			erase();
			continue;
		}
		auto* animData = animTime.GetAnimData(actor);
		if (!animData)
		{
			erase();
			continue;
		}

		const auto time = GetAnimTime(animData, anim);
		
		// allow code below to clean up
		bool deferredErase = false;

		if (anim->lastScaledTime - animTime.lastNiTime < -0.01f && anim->cycleType != NiControllerSequence::LOOP || anim->state == NiControllerSequence::kAnimState_Inactive)
		{
			deferredErase = true;
		}
		animTime.lastNiTime = anim->lastScaledTime;
		
		if (animTime.respectEndKey && !animTime.finishedEndKey)
		{
			static std::unordered_map<BSAnimGroupSequence*, std::pair<float*, int>> s_anim3rdKeys;
			const auto revert3rdPersonAnimTimes = [&]()
			{
				if (const auto iter3rd = s_anim3rdKeys.find(animTime.anim3rdCounterpart); iter3rd != s_anim3rdKeys.end())
				{
					auto [keys, numKeys] = iter3rd->second;
					animTime.anim3rdCounterpart->animGroup->numKeys = numKeys;
					animTime.anim3rdCounterpart->animGroup->keyTimes = keys;
				}
				animTime.povState = POVSwitchState::POV3rd;
			};
			if (time >= anim->endKeyTime || actor->GetWeaponForm() != animTime.actorWeapon || deferredErase)
			{
				revert3rdPersonAnimTimes();
				animTime.finishedEndKey = true;
			}
			else
			{
				if (animTime.povState == POVSwitchState::NotSet)
				{
					const auto sequenceId = anim->animGroup->GetGroupInfo()->sequenceType;
					auto* anim3rd = g_thePlayer->baseProcess->GetAnimData()->animSequence[sequenceId];
					if (!anim3rd || (anim3rd->animGroup->groupID & 0xFF) != (anim->animGroup->groupID & 0xFF))
						anim3rd = anim->Get3rdPersonCounterpart();
					if (!anim3rd)
						continue;
					animTime.anim3rdCounterpart = anim3rd;
					s_anim3rdKeys.emplace(anim3rd, std::make_pair(anim3rd->animGroup->keyTimes, anim3rd->animGroup->numKeys));
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
		}
		if (!deferredErase)
		{
			if (animTime.scriptCalls.Exists())
			{
				animTime.scriptCalls.Update(time, animData, _L(Script* script, g_script->CallFunction(script, actor, nullptr, nullptr, 0)));
			}
			if (animTime.soundPaths.Exists())
			{
				animTime.soundPaths.Update(time, animData, [&](Sound& sound)
				{
					sound.Set3D(actor);
					sound.Play();
				});
			}
			if (animTime.scriptLines.Exists())
			{
				animTime.scriptLines.Update(time, animData, _L(Script* script, ThisStdCall(0x5AC1E0, script, actor, actor->GetEventList(), nullptr, false)));
			}
		}
		else
		{
			erase();
			continue;
		}
		++iter;
	}
	for (auto iter = g_timeTrackedGroups.begin(); iter != g_timeTrackedGroups.end();)
	{
		auto& [savedAnim, animTime] = *iter;
		auto& [_, groupId, anim, actorId, lastNiTime, firstPerson] = animTime;
		auto* actor = DYNAMIC_CAST(LookupFormByRefID(actorId), TESForm, Actor);

		if (shouldErase(actor) || anim && anim->lastScaledTime - animTime.lastNiTime < -0.01f && anim->cycleType != NiControllerSequence::LOOP || anim && anim->state == NiControllerSequence::kAnimState_Inactive)
		{
			iter = g_timeTrackedGroups.erase(iter);
			continue;
		}
		auto* animData = firstPerson ? g_thePlayer->firstPersonAnimData : actor->baseProcess->GetAnimData();
		if (!animData)
		{
			iter = g_timeTrackedGroups.erase(iter);
			continue;
		}

		if (anim)
			lastNiTime = anim->weightedLastTime;
		if (savedAnim->conditionScript)
		{
			auto* animInfo = GetGroupInfo(groupId);
			auto* curAnim = animData->animSequence[animInfo->sequenceType];
			if (curAnim && curAnim->animGroup->groupID == groupId)
			{
				NVSEArrayVarInterface::Element arrResult;
				if (g_script->CallFunction(savedAnim->conditionScript, actor, nullptr, &arrResult, 0))
				{
					const auto result = static_cast<bool>(arrResult.Number());
					const auto customAnimState = anim ? anim->state : kAnimState_Inactive;
					if (customAnimState == kAnimState_Inactive && result && curAnim != anim
						|| customAnimState != kAnimState_Inactive && !result && curAnim == anim)
					{
						GameFuncs::PlayAnimGroup(animData, groupId, 1, -1, -1);
					}
				}
			}
		}
		++iter;
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
		if (!actor || actor->IsDeleted() || actor->IsDying(true) || anim->lastScaledTime - lastNiTime < -0.01f && anim->cycleType != NiControllerSequence::LOOP)
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
constexpr auto kNVSEVersion = 12;

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
			if (auto* overrideAnim = GetActorAnimation(groupId, animData == animData1st, animData, anim ? anim->sequenceName : nullptr))
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

void HandleEmptyAttack()
{
	auto* ammoInfo = g_thePlayer->baseProcess->GetAmmoInfo();
	if (!ammoInfo)
		return;
	if (ammoInfo->count != 0)
		return;
	auto* animData = g_thePlayer->GetAnimData();
	auto* currentWeaponAnim = animData->animSequence[kSequence_Weapon];
	if (!currentWeaponAnim || !currentWeaponAnim->animGroup->IsAim())
		return;
	bool firstPerson = animData == g_thePlayer->firstPersonAnimData;
	const auto groupID = (currentWeaponAnim->animGroup->groupID & 0xFF00) + kAnimGroup_Aim;
}

void HandleExecutionQueue()
{
	while (!g_executionQueue.empty())
	{
		g_executionQueue.front()();
		g_executionQueue.pop_front();
	}
}

void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		g_thePlayer = *(PlayerCharacter **)0x011DEA3C;
		LoadFileAnimPaths();
		Console_Print("kNVSE version %d", kNVSEVersion);
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
#if _DEBUG || IS_TRANSITION_FIX
			HandleProlongedAim();
#endif
			//auto* niBlock = GetNifBlock(g_thePlayer, 2, "Bip01 L Thumb12");
			//static auto lastZ = niBlock->m_localTranslate.z;
			//if (lastZ != niBlock->m_localTranslate.z)
				//Console_Print("%g", niBlock->m_localTranslate.z);
		}
	}

}

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	_MESSAGE("query");

	// fill out the info structure
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "kNVSE";
	info->version = kNVSEVersion;

	// version checks
	if (!nvse->isEditor && nvse->nvseVersion < PACKED_NVSE_VERSION)
	{
		const auto str = FormatString("kNVSE: NVSE version too old (got %X expected at least %X). Plugin will NOT load! Install the latest version here: https://github.com/xNVSE/NVSE/releases/", nvse->nvseVersion, PACKED_NVSE_VERSION);
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
#if _DEBUG
	if (false)
	{
		PatchPause(0);
		g_thePlayer->baseProcess->GetCurrentSequence();
	}
#endif
	g_pluginHandle = nvse->GetPluginHandle();
	g_nvseInterface = (NVSEInterface*)nvse;
	g_messagingInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	g_messagingInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);
	g_script = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
	
	nvse->SetOpcodeBase(0x3920);
	
	RegisterScriptCommand(ForcePlayIdle);
	RegisterScriptCommand(SetWeaponAnimationPath);
	RegisterScriptCommand(SetActorAnimationPath);
	RegisterScriptCommand(PlayAnimationPath);
	RegisterScriptCommand(ForceStopIdle);
	RegisterScriptCommand(kNVSEReset);
#if _DEBUG
	RegisterScriptCommand(kNVSETest);
#endif
	
	if (isEditor)
	{
		return true;
	}

	ApplyHooks();
	return true;
}
