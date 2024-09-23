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
    bool ignorePriorities = false;

    auto operator<=>(const AdditiveAnimMetadata&) const = default;
};

namespace AdditiveManager
{
    bool IsAdditiveSequence(BSAnimGroupSequence* sequence);
    void EraseAdditiveSequence(BSAnimGroupSequence* sequence);
    bool IsAdditiveInterpolator(NiInterpolator* interpolator);
    bool StopManagedAdditiveSequenceFromParent(BSAnimGroupSequence* parentSequence, float afEaseOutTime = INVALID_TIME);
    std::pair<BSAnimGroupSequence*, NiQuatTransform>* GetSequenceAndRefFrameTransform(NiInterpolator* interpolator);
    void InitAdditiveSequence(Actor* actor, BSAnimGroupSequence* additiveSequence, AdditiveAnimMetadata metadata);
    void PlayManagedAdditiveAnim(AnimData* animData, BSAnimGroupSequence* referenceAnim, BSAnimGroupSequence* additiveAnim);
    void MarkInterpolatorsAsAdditive(BSAnimGroupSequence* additiveSequence);
#if _DEBUG
    const char* GetTargetNodeName(NiInterpolator* interpolator);
#endif
    void WriteHooks();
}
