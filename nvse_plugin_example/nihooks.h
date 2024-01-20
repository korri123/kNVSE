#pragma once
#include <unordered_map>

#include "NiNodes.h"

void ApplyNiHooks();
void FixConflictingPriorities(NiControllerSequence* pkSource, NiControllerSequence* pkDest);

extern std::unordered_map<NiControllerManager*, NiControllerSequence*> g_lastTempBlendSequence;