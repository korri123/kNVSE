#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "commands_animation.h"
#include "hooks.h"

#define RegisterScriptCommand(name) 	nvse->RegisterCommand(&kCommandInfo_ ##name);

IDebugLog		gLog("kNVSE.log");

PluginHandle	g_pluginHandle = kPluginHandle_Invalid;

NVSEMessagingInterface* g_messagingInterface;
NVSEInterface* g_nvseInterface;
NVSECommandTableInterface* g_cmdTable;
const CommandInfo* g_TFC;

#if RUNTIME
NVSEScriptInterface* g_script;
#endif

void MessageHandler(NVSEMessagingInterface::Message* msg)
{
}

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	_MESSAGE("query");

	// fill out the info structure
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "kNVSE";
	info->version = 1;

	// version checks
	if (nvse->nvseVersion < NVSE_VERSION_INTEGER)
	{
		_ERROR("NVSE version too old (got %08X expected at least %08X)", nvse->nvseVersion, NVSE_VERSION_INTEGER);
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
		if (nvse->editorVersion < CS_VERSION_1_4_0_518)
		{
			_ERROR("incorrect editor version (got %08X need at least %08X)", nvse->editorVersion, CS_VERSION_1_4_0_518);
			return false;
		}
	}
	return true;
}

bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
	g_pluginHandle = nvse->GetPluginHandle();
	g_nvseInterface = (NVSEInterface*)nvse;
	g_messagingInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	g_messagingInterface->RegisterListener(g_pluginHandle, "NVSE", MessageHandler);
#if RUNTIME
	g_script = (NVSEScriptInterface*)nvse->QueryInterface(kInterface_Script);
#endif
	nvse->SetOpcodeBase(0x3920);

	RegisterScriptCommand(ForcePlayIdle);
	RegisterScriptCommand(SetWeaponAnimationPath);
	RegisterScriptCommand(SetActorAnimationPath);
	ApplyHooks();
	return true;
}
