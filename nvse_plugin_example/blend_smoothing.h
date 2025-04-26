#pragma once
#include "GameAPI.h"

enum class kInterpDebugState: UInt8
{
    NotSet, RemovedInBlendSmoothing, RemovedInDetachInterpolators, AttachedNormally, DetachedButSmoothing, ReattachedWhileSmoothing
};

enum class kInterpState: UInt8
{
    NotSet, Activating, Deactivating
};

struct kAdditiveInterpMetadata
{
    NiQuatTransform refTransform;
    bool ignorePriorities = false;
    float weightMultiplier = 1.0f;

    void ClearValues()
    {
        refTransform = NiQuatTransform();
        ignorePriorities = false;
        weightMultiplier = 1.0f;
    }
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
    bool isAdditive = false;
    kAdditiveInterpMetadata additiveMetadata;

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
        isAdditive = false;
        additiveMetadata.ClearValues();
    }
};

class kBlendInterpolatorExtraData : public NiExtraData
{
public:
    std::vector<kBlendInterpItem> items;
    NiPointer<NiTransformInterpolator> poseInterp = nullptr;
    float poseInterpUpdatedTime = -NI_INFINITY;

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

    NiTransformInterpolator* ObtainPoseInterp(NiAVObject* target);
};

namespace BlendSmoothing
{
    void Apply(NiBlendInterpolator* blendInterp, kBlendInterpolatorExtraData* extraData);
    void ApplyForItems(kBlendInterpolatorExtraData* extraData, std::span<NiBlendInterpolator::InterpArrayItem*> items);

    void WriteHooks();
}