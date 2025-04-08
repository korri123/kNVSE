#pragma once
#include "NiObjects.h"

struct kBlendInterpItem
{
    NiControllerSequencePtr sequence = nullptr;
    NiPointer<NiInterpolator> interpolator = nullptr;
    float lastSmoothedWeight = -NI_INFINITY;
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
    static kBlendInterpolatorExtraData* Acquire(NiAVObject* obj);
};