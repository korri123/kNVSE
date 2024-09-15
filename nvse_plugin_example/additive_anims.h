#pragma once
#include "GameProcess.h"
#include "NiNodes.h"

enum class AdditiveTransformType
{
    ReferenceFrame,
    EachFrame
};

namespace AdditiveManager
{
    BSAnimGroupSequence* GetSequenceByInterpolator(const NiInterpolator* interpolator);
    void AddAdditiveSequence(BSAnimGroupSequence* sequence);
    void PlayAdditiveAnim(AnimData* animData, const BSAnimGroupSequence* anim, std::string_view additiveAnimPath);
    bool IsAdditiveSequence(const BSAnimGroupSequence* sequence);
    bool IsAdditiveInterpolator(const NiInterpolator* interpolator);
    AdditiveTransformType GetAdditiveTransformType(const NiInterpolator* interpolator);
    bool StopAdditiveSequenceFromParent(const BSAnimGroupSequence* parentSequence, float afEaseOutTime = INVALID_TIME);
    std::optional<NiQuatTransform> GetRefFrameTransform(const NiInterpolator* interpolator);
    
    void WriteHooks();
}
