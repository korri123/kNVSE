#pragma once
#include <functional>
#include <deque>
#include <PluginAPI.h>
#include <thread>

#include "commands_animation.h"

struct CustomAnimGroupScript
{
	LambdaVariableContext script;
	LambdaVariableContext cleanUpScript;

	friend bool operator==(const CustomAnimGroupScript& lhs, const CustomAnimGroupScript& rhs)
	{
		return lhs.script == rhs.script
			&& lhs.cleanUpScript == rhs.cleanUpScript;
	}

	friend bool operator!=(const CustomAnimGroupScript& lhs, const CustomAnimGroupScript& rhs)
	{
		return !(lhs == rhs);
	}
};

extern NVSEScriptInterface* g_script;

extern std::deque<std::function<void()>> g_executionQueue;
extern ICriticalSection g_executionQueueCS;
extern NVSEArrayVarInterface* g_arrayVarInterface;
extern NVSEStringVarInterface* g_stringVarInterface;
extern std::unordered_map<std::string, std::vector<CustomAnimGroupScript>> g_customAnimGroups;
using AnimGroupPathsMap = std::unordered_map<std::string, std::unordered_set<std::string>, transparent_string_hash, std::equal_to<>>;
extern AnimGroupPathsMap g_customAnimGroupPaths;
extern std::map<std::pair<FullAnimGroupID, AnimData*>, std::deque<BSAnimGroupSequence*>> g_queuedReplaceAnims;
extern std::vector<std::string> g_eachFrameScriptLines;
extern std::thread g_animFileThread;

void Revert3rdPersonAnimTimes(BSAnimGroupSequence* anim3rd, BSAnimGroupSequence* anim1st);
void Set3rdPersonAnimTimes(BSAnimGroupSequence* anim3rd, BSAnimGroupSequence* anim1st);
void Save3rdPersonAnimGroupData(BSAnimGroupSequence* anim3rd);

bool IsGodMode();

float GetAnimTime(const AnimData* animData, const NiControllerSequence* anim);

bool CallFunction(Script* funcScript, TESObjectREFR* callingObj, TESObjectREFR* container,
		NVSEArrayVarInterface::Element* result);



