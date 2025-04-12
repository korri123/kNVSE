#pragma once
#include "NiObjects.h"

struct kBlendInterpItem
{
    NiPointer<NiInterpolator> interpolator = nullptr;
    float lastSmoothedWeight = -NI_INFINITY;
    NiControllerSequence* sequence = nullptr;
    bool detached = false;
    unsigned char blendIndex = INVALID_INDEX;

    void ClearValues()
    {
        interpolator = nullptr;
        lastSmoothedWeight = -NI_INFINITY;
        sequence = nullptr;
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

    kBlendInterpItem& GetItem(NiInterpolator* interpolator);
};

namespace BlendSmoothing
{
    void Apply(NiBlendInterpolator* blendInterp, NiObjectNET* target);
}