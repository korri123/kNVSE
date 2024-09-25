#pragma once
#include "GameProcess.h"
#include "NiNodes.h"

namespace AdditiveManager
{
    bool IsAdditiveSequence(NiControllerSequence* sequence);
    void EraseAdditiveSequence(NiControllerSequence* sequence);
    bool StopManagedAdditiveSequenceFromParent(BSAnimGroupSequence* parentSequence, float afEaseOutTime = INVALID_TIME);
    void InitAdditiveSequence(AnimData* animData, NiControllerSequence* additiveSequence, NiControllerSequence* referencePoseSequence, float
                              timePoint, bool ignorePriorities);
    void PlayManagedAdditiveAnim(AnimData* animData, BSAnimGroupSequence* referenceAnim, BSAnimGroupSequence* additiveAnim);
    void WriteHooks();
}
