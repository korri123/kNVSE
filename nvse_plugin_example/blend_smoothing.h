#pragma once
#include "NiObjects.h"

enum class kInterpState: UInt8
{
    NotSet, RemovedInBlendSmoothing, RemovedInDetachInterpolators, AttachedNormally, DetachedButSmoothing, ReattachedWhileSmoothing,
    InterpControllerDestroyed
};

struct kBlendInterpItem
{
    NiPointer<NiInterpolator> interpolator = nullptr;
    float lastSmoothedWeight = -NI_INFINITY;
    NiControllerSequence* sequence = nullptr;
    NiBlendInterpolator* blendInterp = nullptr;
    float lastNormalizedWeight = -NI_INFINITY;
    float calculatedNormalizedWeight = -NI_INFINITY;
    float lastCalculatedNormalizedWeight = -NI_INFINITY;
    bool detached = false;
    unsigned char blendIndex = INVALID_INDEX;
    kInterpState state = kInterpState::NotSet;

    void ClearValues()
    {
        interpolator = nullptr;
        lastSmoothedWeight = -NI_INFINITY;
        sequence = nullptr;
        blendInterp = nullptr;
        lastNormalizedWeight = -NI_INFINITY;
        calculatedNormalizedWeight = -NI_INFINITY;
        lastCalculatedNormalizedWeight = -NI_INFINITY;
        detached = false;
        blendIndex = INVALID_INDEX;
    }
};

class kBlendInterpolatorExtraData : public NiExtraData
{
public:
    std::vector<kBlendInterpItem> items;

    NiNewRTTI(kBlendInterpolatorExtraData, NiExtraData)

    void Destroy(bool freeMem);
    bool IsEqualEx(const kBlendInterpolatorExtraData* other) const;

    static kBlendInterpolatorExtraData* Create();
    static const NiFixedString& GetKey();
    static kBlendInterpolatorExtraData* Obtain(NiObjectNET* obj);
    static kBlendInterpolatorExtraData* GetExtraData(NiObjectNET* obj);

    kBlendInterpItem& GetItem(NiInterpolator* interpolator);
};

namespace BlendSmoothing
{
    void Apply(NiBlendInterpolator* blendInterp, NiObjectNET* target);
    void WriteHooks();
}