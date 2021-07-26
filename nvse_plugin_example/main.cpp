
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
extern std::list<BurstFireData> g_callScriptOnKey;

void HandleAnimTimes()
{
	auto iter = g_firstPersonAnimTimes.begin();
	while (iter != g_firstPersonAnimTimes.end())
	{
		iter->second.time += GetTimePassed();
		if (iter->second.time >= iter->first->endKeyTime)
			iter->second.finished = true;
		++iter;
	}
}

void HandleBurstFire()
{
	auto iter = g_burstFireQueue.begin();
	while (iter != g_burstFireQueue.end())
	{
		auto& [animData, anim, index, hitKeys, timePassed] = *iter;
		timePassed += GetTimePassed();
		auto* currentAnim = animData->animSequence[kSequence_Weapon];
		if (currentAnim != anim)
		{
			iter = g_burstFireQueue.erase(iter);
			continue;
		}
		Console_Print("%s", currentAnim->sequenceName);
		if (timePassed <= anim->animGroup->keyTimes[kSeqState_Hit] || IsPlayersOtherAnimData(animData))
		{
			// first hit handled by engine
			// don't want duplicated shootings
			++iter;
			continue;
		}
		if (timePassed > hitKeys.at(index)->m_fTime)
		{
			auto* weap = animData->actor->baseProcess->GetWeaponInfo();
			if (!weap || !weap->weapon)
			{
				iter = g_burstFireQueue.erase(iter);
				continue;
			}

			if (auto* ammoInfo = static_cast<Decoding::MiddleHighProcess*>(animData->actor->baseProcess)->ammoInfo)
			{
				const auto ammoCount = ammoInfo->countDelta;
				if (ammoCount == weap->weapon->clipRounds.clipRounds)
				{
					iter = g_burstFireQueue.erase(iter);
					continue;
				}
			}
			// fires
			animData->actor->baseProcess->SetQueuedIdleFlag(kIdleFlag_FireWeapon);
			ThisStdCall(0x8BA600, animData->actor); //Actor::HandleQueuedIdleFlags
			++index;	
		}
		if (index < hitKeys.size())
			++iter;
		else
			iter = g_burstFireQueue.erase(iter);
	}
	
}

void MessageHandler(NVSEMessagingInterface::Message* msg)
{
	if (msg->type == NVSEMessagingInterface::kMessage_DeferredInit)
	{
		g_thePlayer = *(PlayerCharacter **)0x011DEA3C;
		LoadFileAnimPaths();
	}
#if 1
	else if (msg->type == NVSEMessagingInterface::kMessage_MainGameLoop)
	{
		HandleBurstFire();
		HandleAnimTimes();
	}
#endif
#if 0
	else if (msg->type == NVSEMessagingInterface::kMessage_LoadGame)
	{
		for (auto& pair : customMaps)
		{
			auto& map = pair.second;
			map->~NiTPointerMap<AnimSequenceBase>();
			FormHeap_Free(map);
			map = nullptr;
		}
		customMaps.clear();
	}
#endif
}

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	_MESSAGE("query");

	// fill out the info structure
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "kNVSE";
	info->version = 6;

	// version checks
	if (nvse->nvseVersion < PACKED_NVSE_VERSION)
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
