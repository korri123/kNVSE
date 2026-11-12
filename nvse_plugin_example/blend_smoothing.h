#pragma once
#include <memory>
#include <optional>

#include "GameAPI.h"
#include "weapon_hand_blend.h"
#include "weapon_parent_switch.h"

enum class kInterpDebugState: UInt8
{
    NotSet, RemovedInBlendSmoothing, RemovedInDetachInterpolators, AttachedNormally, DetachedButSmoothing, ReattachedWhileSmoothing, RemovedInStoreSingle,
    WasDetachedButNowAdditive
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
    // diagnostics only: how the item got into its current state
    kInterpDebugState debugState = kInterpDebugState::NotSet;
    // its sequence has a noBlendSmoothing or noBlend text key: while it is attached, the node is blended as computed
    bool noSmoothing = false;
    // its sequence has an aimPitch text key: the share of the actor's pitch its pose of the bone is turned by (AimPitch)
    float aimPitchShare = 0.0f;
    bool isAdditive = false;
    kAdditiveInterpMetadata additiveMetadata;
    // Weapon node items of equip/unequip animations: where the track switches parent space (WeaponParentSwitch)
    std::optional<WeaponParentSwitch::Window> parentSwitch;

    bool IsSmoothedWeightZero() const
    {
        return (translateWeight.lastSmoothedWeight == 0.0f || translateWeight.lastSmoothedWeight == -NI_INFINITY) && 
            (rotateWeight.lastSmoothedWeight == 0.0f || rotateWeight.lastSmoothedWeight ==  -NI_INFINITY) && 
            (scaleWeight.lastSmoothedWeight == 0.0f || scaleWeight.lastSmoothedWeight == -NI_INFINITY);
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
        noSmoothing = false;
        aimPitchShare = 0.0f;
        isAdditive = false;
        additiveMetadata.ClearValues();
        parentSwitch.reset();
    }
};

class kBlendInterpolatorExtraData : public NiExtraData
{
public:
    NiAVObject* target = nullptr;
    std::vector<kBlendInterpItem> items;
    NiControllerManager* owner = nullptr;
    // set only while the engine snaps the bones bone LOD switched off to an ending (un)equip (UpdateLODBonesHook): their
    // weights are taken as computed, and the smoothed state follows them
    bool unsmoothedUpdate = false;
    // which items were attached, at which priorities, when the blend interpolator was last evaluated (BlendSmoothing::Step)
    UInt32 attachedItems = 0;
    // the node is a child of its sequences' accum root (Bip01 NonAccum): it is posed relative to the actor. Set on attach,
    // survives ClearValues like weaponHandBlend
    bool accumRootChild = false;
    // Weapon nodes only; survives ClearValues, as it describes the skeleton and its sequences, not the current blend
    std::unique_ptr<WeaponHandBlend::NodeData> weaponHandBlend;
    
    void ClearValues()
    {
        target = nullptr;
        items.clear();
        items.shrink_to_fit();
        owner = nullptr;
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
    kBlendInterpItem* GetItem(NiInterpolator* interpolator);
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
        // an attached item wants no smoothing (kBlendInterpItem::noSmoothing), or the node is posed relative to an actor the
        // engine is turning at once: nothing is taken up, and what was is dropped
        bool bDisabled = false;
    private:
        UInt32 m_uiAttachedItems = 0;
    };
    void ApplyForItems(kBlendInterpolatorExtraData* extraData, const std::vector<std::pair<NiBlendInterpolator::InterpArrayItem*, kBlendInterpItem*>>& items, kWeightType type, const Step& step);
    float GetElapsedTime(float fLastTime, float fTime);
    void DetachZeroWeightItems(kBlendInterpolatorExtraData* extraData, NiBlendInterpolator* blendInterp);
    void WriteHooks();
}