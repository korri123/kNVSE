
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

#include "SimpleINILibrary.h"

#define RegisterScriptCommand(name) 	nvse->RegisterCommand(&kCommandInfo_ ##name);

IDebugLog		gLog("kNVSE.log");

PluginHandle	g_pluginHandle = kPluginHandle_Invalid;

NVSEMessagingInterface* g_messagingInterface;
NVSEInterface* g_nvseInterface;
NVSECommandTableInterface* g_cmdTable;
const CommandInfo* g_TFC;
PlayerCharacter* g_player;


#if RUNTIME
NVSEScriptInterface* g_script;
#endif

bool isEditor = false;
extern std::list<BurstFireData> g_burstFireQueue;

void HandleAnimTimes()
{
	for (auto iter = g_timeTrackedAnims.begin(); iter != g_timeTrackedAnims.end(); ++iter)
	{
		auto& animTime = iter->second;
		auto* anim = iter->first;
		auto& time = animTime.time;
		time += GetTimePassed(animTime.animData, anim->animGroup->groupID);

		if (animTime.respectEndKey)
		{
			const auto revert3rdPersonAnimTimes = [&]()
			{
				animTime.anim3rdCounterpart->animGroup->numKeys = animTime.numThirdPersonKeys;
				animTime.anim3rdCounterpart->animGroup->keyTimes = animTime.thirdPersonKeys;
				animTime.povState = POVSwitchState::POV3rd;
			};
			if (time >= anim->endKeyTime && !animTime.finishedEndKey)
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
					animTime.numThirdPersonKeys = anim3rd->animGroup->numKeys;
					animTime.thirdPersonKeys = anim3rd->animGroup->keyTimes;
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
		if (animTime.callScript && animTime.scriptStage < animTime.scripts.size())
		{
			auto& p = animTime.scripts.at(animTime.scriptStage);
			if (time > p.second)
			{
				g_script->CallFunction(p.first, animTime.animData->actor, nullptr, nullptr, 0);
				++animTime.scriptStage;
			}
		}
	}
}

void HandleBurstFire()
{
	auto iter = g_burstFireQueue.begin();
	while (iter != g_burstFireQueue.end())
	{
		auto& [animData, anim, index, hitKeys, timePassed, shouldEject] = *iter;
		auto* currentAnim = animData->animSequence[kSequence_Weapon];
		auto* weap = animData->actor->baseProcess->GetWeaponInfo();
		auto* weapon = weap ? weap->weapon : nullptr;
		const auto erase = [&]()
		{
			iter = g_burstFireQueue.erase(iter);
		};
		timePassed += GetTimePassed(animData, anim->animGroup->groupID);
		if (currentAnim != anim)
		{
			erase();
			continue;
		}
		if (timePassed <= anim->animGroup->keyTimes[kSeqState_HitOrDetach])
		{
			// first hit handled by engine
			// don't want duplicated shootings
			++iter;
			continue;
		}
		if (timePassed > hitKeys.at(index)->m_fTime)
		{

			if (auto* ammoInfo = static_cast<Decoding::MiddleHighProcess*>(animData->actor->baseProcess)->ammoInfo)
			{
				const auto ammoCount = ammoInfo->countDelta;
				const bool* godMode = reinterpret_cast<bool*>(0x11E07BA);
				if (weapon && (ammoCount == 0 || ammoCount == weapon->clipRounds.clipRounds) && !godMode)
				{
					erase();
					continue;
				}
			}
			++index;
			if (!IsPlayersOtherAnimData(animData))
			{
				animData->actor->FireWeapon();
				if (weapon && GameFuncs::IsDoingAttackAnimation(animData->actor) && !weapon->IsMeleeWeapon() && !weapon->IsAutomatic())
				{
					// eject
					animData->actor->baseProcess->SetQueuedIdleFlag(kIdleFlag_AttackEjectEaseInFollowThrough);
					GameFuncs::HandleQueuedAnimFlags(animData->actor);
				}
			}
		}
		
		if (index < hitKeys.size())
			++iter;
		else
			erase();
	}
	
}

void HandleMorph()
{
	
	auto* animData3rd = g_thePlayer->baseProcess->GetAnimData();
	auto* animData1st = g_thePlayer->firstPersonAnimData;
	auto* highProcess = g_thePlayer->GetHighProcess();
	auto* curWeaponAnim = animData3rd->animSequence[kSequence_Weapon];
	if (!curWeaponAnim || !curWeaponAnim->animGroup->IsAttackIS() || highProcess->isAiming)
		return;
	auto* weapon = g_thePlayer->GetWeaponForm();
	if (!weapon)
		return;
	const auto curGroupId = curWeaponAnim->animGroup->groupID;
	const UInt16 hipfireId = curGroupId - 3;
	for (auto* animData : {animData3rd, animData1st})
	{
		auto* hipfireAnim = GetGameAnimation(animData, hipfireId);
		auto* sourceAnim = animData->animSequence[kSequence_Weapon];
		animData->animSequence[kSequence_Weapon] = hipfireAnim;
		animData->groupIDs[kSequence_Weapon] = hipfireId;
		const auto duration = sourceAnim->endKeyTime - sourceAnim->startTime;
		const auto result = GameFuncs::ActivateSequence(nullptr, sourceAnim, hipfireAnim, duration, 0, true, hipfireAnim->seqWeight, nullptr);
		int i = 0;
	}
	highProcess->SetCurrentActionAndSequence(hipfireId, animData3rd->animSequence[kSequence_Weapon]);
}

void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		g_thePlayer = *(PlayerCharacter **)0x011DEA3C;
		LoadFileAnimPaths();
	}
	else if (msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
		const auto isMenuMode = CdeclCall<bool>(0x702360);
		if (!isMenuMode)
		{
			HandleBurstFire();
			HandleAnimTimes();
			//HandleMorph();
		}
	}

}

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	_MESSAGE("query");

	// fill out the info structure
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "kNVSE";
	info->version = 6;

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
	
	RegisterScriptCommand(ForcePlayIdle)
	RegisterScriptCommand(SetWeaponAnimationPath)
	RegisterScriptCommand(SetActorAnimationPath)
	RegisterScriptCommand(PlayAnimationPath)
	RegisterScriptCommand(ForceStopIdle)
	
	if (isEditor)
	{
		return true;
	}

	ApplyHooks();
	return true;
}
