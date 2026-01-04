#pragma once
#include "GameAPI.h"

enum class kInterpDebugState: UInt8
{
    NotSet, RemovedInBlendSmoothing, RemovedInDetachInterpolators, AttachedNormally, DetachedButSmoothing, ReattachedWhileSmoothing, RemovedInStoreSingle
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

struct kWeightState
{
    float lastSmoothedWeight = -NI_INFINITY;
    float calculatedNormalizedWeight = -NI_INFINITY;
    float lastCalculatedNormalizedWeight = -NI_INFINITY;

    void ClearValues()
    {
        lastSmoothedWeight = -NI_INFINITY;
        calculatedNormalizedWeight = -NI_INFINITY;
        lastCalculatedNormalizedWeight = -NI_INFINITY;
    }
};

enum class kWeightType: UInt8
{
    Translate, Rotate, Scale
};

struct kBlendInterpItem
{
    NiPointer<NiInterpolator> interpolator = nullptr;
    NiControllerSequence* sequence = nullptr;
    NiBlendInterpolator* blendInterp = nullptr;
    kWeightState translateWeight;
    kWeightState rotateWeight;
    kWeightState scaleWeight;
    bool detached = false;
    unsigned char blendIndex = INVALID_INDEX;
    kInterpDebugState debugState = kInterpDebugState::NotSet;
    kInterpState state = kInterpState::NotSet;
    unsigned char poseInterpIndex = INVALID_INDEX;
    bool isPoseInterp = false;
    bool isAdditive = false;
    kAdditiveInterpMetadata additiveMetadata;

    bool IsSmoothedWeightZero() const
    {
        return (translateWeight.lastSmoothedWeight == 0.0f || translateWeight.lastSmoothedWeight == -NI_INFINITY) && 
            (rotateWeight.lastSmoothedWeight == 0.0f || rotateWeight.lastSmoothedWeight ==  -NI_INFINITY) && 
            (scaleWeight.lastSmoothedWeight == 0.0f || scaleWeight.lastSmoothedWeight == -NI_INFINITY);
    }

    bool IsSmoothedWeightValid() const
    {
        return translateWeight.lastSmoothedWeight != -NI_INFINITY && rotateWeight.lastSmoothedWeight != -NI_INFINITY && scaleWeight.lastSmoothedWeight != -NI_INFINITY;
    }

    kWeightState* GetWeightState(kWeightType type)
    {
        switch (type)
        {
        case kWeightType::Translate:
            return &translateWeight;
        case kWeightType::Rotate:
            return &rotateWeight;
        case kWeightType::Scale:
            return &scaleWeight;
        }
        return nullptr;
    }

    void ClearValues()
    {
        interpolator = nullptr;
        sequence = nullptr;
        translateWeight.ClearValues();
        rotateWeight.ClearValues();
        scaleWeight.ClearValues();
        blendInterp = nullptr;
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
    NiControllerManager* owner = nullptr;
    UInt32 noBlendSmoothRequesterCount = 0;

    NiNewRTTI(kBlendInterpolatorExtraData, NiExtraData)

    void Destroy(bool freeMem);
    bool IsEqualEx(const kBlendInterpolatorExtraData* other) const;
    static void EraseSequence(NiControllerSequence* anim);

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
    void ApplyForItems(kBlendInterpolatorExtraData* extraData, const std::vector<NiBlendInterpolator::InterpArrayItem*>& items, kWeightType type);
    void DetachZeroWeightItems(kBlendInterpolatorExtraData* extraData, NiBlendInterpolator* blendInterp);
    void WriteHooks();
}