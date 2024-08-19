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


struct ScriptPairHash {
	std::size_t operator()(const std::pair<TESObjectREFR*, Script*>& pair) const {
		std::size_t h1 = std::hash<TESObjectREFR*>{}(pair.first);
		std::size_t h2 = 0;

		if (pair.second && pair.second->data) {
			// Hash for Script* part
			h2 = std::hash<void*>{}(pair.second->data);
			h2 ^= std::hash<UInt32>{}(pair.second->info.dataLength) + 0x9e3779b9 + (h2 << 6) + (h2 >> 2);
		}

		// Combine hashes
		return h1 ^ (h2 << 1);
	}
};

struct ScriptPairEqual {
	bool operator()(const std::pair<TESObjectREFR*, Script*>& lhs, 
					const std::pair<TESObjectREFR*, Script*>& rhs) const {
		// Check TESObjectREFR* equality
		if (lhs.first != rhs.first) return false;

		// Check Script* equality
		if (lhs.second == rhs.second) return true;
		if (!lhs.second || !rhs.second) return false;
		if (lhs.second->info.dataLength != rhs.second->info.dataLength) return false;
		if (!lhs.second->data || !rhs.second->data) return false;
		
		// Compare the actual data content of Script
		return std::memcmp(lhs.second->data, rhs.second->data, lhs.second->info.dataLength) == 0;
	}
};


using ScriptCache = std::unordered_map<std::pair<TESObjectREFR*, Script*>, NVSEArrayVarInterface::Element, ScriptPairHash, ScriptPairEqual>;
extern ScriptCache g_scriptCache;
