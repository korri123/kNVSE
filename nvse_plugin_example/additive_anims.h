#pragma once
#include "GameProcess.h"
#include "NiNodes.h"

enum class AdditiveTransformType
{
    ReferenceFrame,
    EachFrame
};

struct AdditiveAnimMetadata
{
    BSAnimGroupSequence* referencePoseSequence;
    float referenceTimePoint;
};

namespace AdditiveManager
{
    bool IsAdditiveSequence(BSAnimGroupSequence* sequence);
    void EraseAdditiveSequence(BSAnimGroupSequence* sequence);
    bool IsAdditiveInterpolator(NiInterpolator* interpolator);
    bool StopManagedAdditiveSequenceFromParent(BSAnimGroupSequence* parentSequence, float afEaseOutTime = INVALID_TIME);
    NiQuatTransform* GetRefFrameTransform(NiInterpolator* interpolator);
    void SetAdditiveReferencePose(BSAnimGroupSequence* baseSequence, BSAnimGroupSequence* additiveSequence, float timePoint = 0.0f);
    void PlayManagedAdditiveAnim(AnimData* animData, BSAnimGroupSequence* referenceAnim, BSAnimGroupSequence* additiveAnim);
#if _DEBUG
    const char* GetTargetNodeName(NiInterpolator* interpolator);
#endif
    void WriteHooks();
}
