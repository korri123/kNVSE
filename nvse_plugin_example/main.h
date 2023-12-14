#pragma once
#include <functional>
#include <deque>
#include <PluginAPI.h>

#include "commands_animation.h"

extern std::deque<std::function<void()>> g_executionQueue;
extern NVSEArrayVarInterface* g_arrayVarInterface;
extern NVSEStringVarInterface* g_stringVarInterface;
extern std::unordered_map<std::string, std::vector<LambdaVariableContext>> g_customAnimGroups;
extern std::unordered_map<std::string, std::unordered_set<std::string>> g_customAnimGroupPaths;
extern std::map<std::pair<FullAnimGroupID, AnimData*>, std::deque<BSAnimGroupSequence*>> g_queuedReplaceAnims;
#define IS_TRANSITION_FIX 0

void Revert3rdPersonAnimTimes(AnimTime& animTime, BSAnimGroupSequence* anim);

bool IsGodMode();

float GetAnimTime(const AnimData* animData, const NiControllerSequence* anim);