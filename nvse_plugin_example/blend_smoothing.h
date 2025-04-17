#pragma once
#include "NiObjects.h"

enum class kInterpDebugState: UInt8
{
    NotSet, RemovedInBlendSmoothing, RemovedInDetachInterpolators, AttachedNormally, DetachedButSmoothing, ReattachedWhileSmoothing
};

enum class kInterpState: UInt8
{
    NotSet, Activating, Deactivating
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
    kInterpDebugState debugState = kInterpDebugState::NotSet;
    kInterpState state = kInterpState::NotSet;
    unsigned char poseInterpIndex = INVALID_INDEX;
    bool isPoseInterp = false;

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
        state = kInterpState::NotSet;
        poseInterpIndex = INVALID_INDEX;
        isPoseInterp = false;
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

    kBlendInterpItem& ObtainItem(NiInterpolator* interpolator);
    kBlendInterpItem& CreatePoseInterpItem(NiBlendInterpolator* blendInterp, NiControllerSequence* sequence, NiAVObject* target);
    kBlendInterpItem* GetItem(NiInterpolator* interpolator);

    kBlendInterpItem* GetPoseInterpItem();
};

namespace BlendSmoothing
{
    void Apply(NiBlendInterpolator* blendInterp, NiObjectNET* target);
    void WriteHooks();
}