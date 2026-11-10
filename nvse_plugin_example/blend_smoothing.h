#pragma once
#include <memory>

#include "GameAPI.h"
#include "weapon_hand_blend.h"

enum class kInterpDebugState: UInt8
{
    NotSet, RemovedInBlendSmoothing, RemovedInDetachInterpolators, AttachedNormally, DetachedButSmoothing, ReattachedWhileSmoothing, RemovedInStoreSingle,
    WasDetachedButNowAdditive
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
    // the weight the item was blended with when its blend interpolator was last evaluated
    float lastSmoothedWeight = -NI_INFINITY;
    // the weight ComputeNormalizedWeights gave it then
    float calculatedNormalizedWeight = -NI_INFINITY;
    // what is left of the weight jumps taken up so far (BlendSmoothing::ApplyForItems): added to the computed weight
    float offset = 0.0f;

    void ClearValues()
    {
        lastSmoothedWeight = -NI_INFINITY;
        calculatedNormalizedWeight = -NI_INFINITY;
        offset = 0.0f;
    }
};

enum class kWeightType: UInt8
{
    Translate, Rotate, Scale
};

struct kBlendInterpItem
{
    NiPointer<NiInterpolator> interpolator = nullptr;
    // The sequence that has this item's interpolator attached; null once the sequence detaches it, including while a
    // detached item is still fading out. Every path that removes or destroys a sequence detaches it first (retail
    // NiControllerSequence::ClearTargets and the destructor hook both Deactivate), so this never points at a dead sequence.
    NiControllerSequence* sequence = nullptr;
    NiBlendInterpolator* blendInterp = nullptr;
    kWeightState translateWeight;
    kWeightState rotateWeight;
    kWeightState scaleWeight;
    // m_fWeight and m_fEaseSpinner of its blend item when the blend interpolator was last evaluated
    float lastWeight = -NI_INFINITY;
    float lastEaseSpinner = -NI_INFINITY;
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
        lastWeight = -NI_INFINITY;
        lastEaseSpinner = -NI_INFINITY;
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
    NiAVObject* target = nullptr;
    std::vector<kBlendInterpItem> items;
    NiPointer<NiTransformInterpolator> poseInterp = nullptr;
    float poseInterpUpdatedTime = -NI_INFINITY;
    NiControllerManager* owner = nullptr;
    UInt32 noBlendSmoothRequesterCount = 0;
    // set only while the engine snaps the bones bone LOD switched off to an ending (un)equip (UpdateLODBonesHook): their
    // weights are taken as computed, and the smoothed state follows them
    bool unsmoothedUpdate = false;
    // which items were attached, at which priorities, when the blend interpolator was last evaluated (BlendSmoothing::Step)
    UInt32 attachedItems = 0;
    // Weapon nodes only; survives ClearValues, as it describes the skeleton and its sequences, not the current blend
    std::unique_ptr<WeaponHandBlend::NodeData> weaponHandBlend;
    
    void ClearValues()
    {
        target = nullptr;
        items.clear();
        items.shrink_to_fit();
        poseInterp = nullptr;
        poseInterpUpdatedTime = -NI_INFINITY;
        owner = nullptr;
        noBlendSmoothRequesterCount = 0;
        attachedItems = 0;
    }

    NiNewRTTI(kBlendInterpolatorExtraData, NiExtraData)

    void Destroy(bool freeMem);
    bool IsEqualEx(const kBlendInterpolatorExtraData* other) const;

    static kBlendInterpolatorExtraData* Create();
    static const NiFixedString& GetKey();
    static kBlendInterpolatorExtraData* GetOrCreate(NiObjectNET* obj);
    static kBlendInterpolatorExtraData* GetExtraData(NiObjectNET* obj);

    kBlendInterpItem& GetOrCreateItem(NiInterpolator* interpolator);
    kBlendInterpItem& CreatePoseInterpItem(NiBlendInterpolator* blendInterp, NiControllerSequence* sequence, NiAVObject* target);
    kBlendInterpItem* GetItem(NiInterpolator* interpolator);
    kBlendInterpItem* GetPoseInterpItem();

    NiTransformInterpolator* ObtainPoseInterp(NiAVObject* target);
};

namespace BlendSmoothing
{
    // What changed since a blend interpolator's last evaluation: tells a jump of the normalized weights from a blend in
    // progress (ApplyForItems)
    class Step
    {
    public:
        // fDeltaTime: animation time since the last evaluation (GetElapsedTime), not the frame time: bone LOD stops
        // evaluating the bones of actors the camera can't see, and a jump taken up before must have faded by then
        explicit Step(float fDeltaTime) : fDeltaTime(fDeltaTime) {}

        // every item the evaluation blends, then End
        void AddItem(const NiBlendInterpolator::InterpArrayItem& item, kBlendInterpItem* extraItem, const NiQuatTransform& kTransform);
        void End(kBlendInterpolatorExtraData* extraData);

        float fDeltaTime;
        // how far the weight and ease spinner of the attached item that moved most have moved
        float fInputChange = 0.0f;
        // other items are attached, or at other priorities
        bool bItemsChanged = false;
    private:
        UInt32 m_uiAttachedItems = 0;
    };
    void ApplyForItems(kBlendInterpolatorExtraData* extraData, const std::vector<std::pair<NiBlendInterpolator::InterpArrayItem*, kBlendInterpItem*>>& items, kWeightType type, const Step& step);
    float GetElapsedTime(float fLastTime, float fTime);
    void DetachZeroWeightItems(kBlendInterpolatorExtraData* extraData, NiBlendInterpolator* blendInterp);
    void WriteHooks();
}