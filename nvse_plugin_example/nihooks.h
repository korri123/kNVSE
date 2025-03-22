#pragma once
#include <unordered_map>

#include "NiNodes.h"

void ApplyNiHooks();

extern std::unordered_map<NiControllerManager*, NiControllerSequence*> g_lastTempBlendSequence;

namespace NiHooks
{
    void WriteDelayedHooks();
}
