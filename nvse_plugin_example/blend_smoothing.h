#pragma once
#include <cstddef>
#include <memory>

#include "anchor.h"
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
    // Weight the item was blended with when its blend interpolator was last evaluated
    float lastSmoothedWeight = -NI_INFINITY;
    // Weight ComputeNormalizedWeights computed at that evaluation
    float calculatedNormalizedWeight = -NI_INFINITY;
    // Remaining part of the weight changes absorbed so far (BlendSmoothing::ApplyForItems); added to the computed weight
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
    // Fields read for every item on every evaluation of the bone's blend (NiBlendTransformInterpolator,
    // BlendSmoothing) come first, so they share a cache line. The flags of the optional features below guard their data.
    NiPointer<NiInterpolator> interpolator = nullptr;
    // Sequence that has this item's interpolator attached. Null once the sequence detaches it, including while the
    // detached item still fades out. Every path that removes or destroys a sequence detaches it first (retail
    // NiControllerSequence::ClearTargets and the destructor hook both Deactivate), so it never dangles.
    NiControllerSequence* sequence = nullptr;
    kWeightState translateWeight;
    kWeightState rotateWeight;
    kWeightState scaleWeight;
    // m_fWeight and m_fEaseSpinner of the blend item at the last evaluation of the blend interpolator
    float lastWeight = -NI_INFINITY;
    float lastEaseSpinner = -NI_INFINITY;
    // Set if the sequence has an aimPitch text key: share of the actor's pitch applied to its pose of the bone (AimPitch)
    float aimPitchShare = 0.0f;
    bool detached = false;
    // Set if the sequence has a noBlendSmoothing or noBlend text key: while attached, the node is blended as computed
    bool noSmoothing = false;
    // additiveMetadata is in use
    bool isAdditive = false;
    // parentSwitch is in use
    bool hasParentSwitch = false;
    // Weapon node items only: the Weapon track is relative to the right hand (WeaponParentSwitch::Counts)
    bool inHandSpace = false;
    // The sequence anchors its pose of the bone to anchorReference (Anchors)
    bool anchored = false;

    // Used when the item is attached or detached, or by few items
    NiBlendInterpolator* blendInterp = nullptr;
    // Index in the blend interpolator while the item is detached
    unsigned char blendIndex = INVALID_INDEX;
    // Diagnostics only: how the item reached its current state
    kInterpDebugState debugState = kInterpDebugState::NotSet;
    kAdditiveInterpMetadata additiveMetadata;
    // Weapon node items of equip/unequip animations: where the track switches parent space (WeaponParentSwitch)
    WeaponParentSwitch::Window parentSwitch{};
    // An ancestor of the bone, set while anchored. Compared, never followed: the bone's chain is walked up to it.
    NiAVObject* anchorReference = nullptr;

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
        hasParentSwitch = false;
        inHandSpace = false;
        anchored = false;
        anchorReference = nullptr;
    }
};

// Everything read on every evaluation fits in one cache line
static_assert(offsetof(kBlendInterpItem, blendInterp) <= 64);

class kBlendInterpolatorExtraData : public NiExtraData
{
public:
    NiAVObject* target = nullptr;
    std::vector<kBlendInterpItem> items;
    NiControllerManager* owner = nullptr;
    // Set only while the engine snaps the bones that bone LOD switched off to the end of an (un)equip animation
    // (UpdateLODBonesHook). Their weights are used as computed and the smoothed state follows them.
    bool unsmoothedUpdate = false;
    // Items attached and their priorities at the last evaluation of the blend interpolator (BlendSmoothing::Step)
    UInt32 attachedItems = 0;
    // The node is a child of its sequences' accum root (Bip01 NonAccum), so it is posed relative to the actor.
    // Set on attach; kept by ClearValues like weaponHandBlend.
    bool accumRootChild = false;
    // Weapon nodes only. Kept by ClearValues because it describes the skeleton and its sequences, not the current blend.
    std::unique_ptr<WeaponHandBlend::NodeData> weaponHandBlend;
    // Weapon nodes only, created by the holster ease (WeaponParentSwitch::EaseHolster). Kept by ClearValues except
    // for the ease in progress.
    std::unique_ptr<WeaponParentSwitch::NodeState> parentSwitch;
    // Bones that sequences anchor (Anchors). Kept by ClearValues like weaponHandBlend.
    std::unique_ptr<Anchors::NodeData> anchor;
    // Rotation from the node's computed weights (before blend smoothing) and the rotation last written to it, valid
    // while the two differ (FireDirection: blend smoothing, additive animations, WeaponHandBlend)
    bool hasComputedRotate = false;
    NiQuaternion computedRotate;
    NiQuaternion writtenRotate;
    
    void ClearValues()
    {
        target = nullptr;
        // Capacity is kept: every weapon attach clears every bone's items (ReloadTargets) and they then refill
        items.clear();
        owner = nullptr;
        attachedItems = 0;
        hasComputedRotate = false;
        if (parentSwitch)
            parentSwitch->bEasing = false;
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
    // Describes what changed since a blend interpolator's last evaluation; separates a jump of the normalized weights
    // from a blend in progress (ApplyForItems)
    class Step
    {
    public:
        // fDeltaTime: animation time since the last evaluation (GetElapsedTime), not frame time. Bone LOD stops
        // evaluating bones of actors the camera can't see, so a jump absorbed earlier must have faded by then.
        explicit Step(float fDeltaTime) : fDeltaTime(fDeltaTime) {}

        // Call AddItem for every item the evaluation blends, then End
        void AddItem(const NiBlendInterpolator::InterpArrayItem& item, kBlendInterpItem* extraItem, const NiQuatTransform& kTransform);
        void End(kBlendInterpolatorExtraData* extraData);

        float fDeltaTime;
        // Largest change of weight or ease spinner among the attached items
        float fInputChange = 0.0f;
        // Other items are attached, or items at other priorities
        bool bItemsChanged = false;
        // An attached item disables smoothing (kBlendInterpItem::noSmoothing), or the node is posed relative to an actor
        // the engine turns instantly. Nothing is absorbed and existing offsets are dropped.
        bool bDisabled = false;
        // fBlendSmoothness absorbs this evaluation's blend in progress: no item is in an ease with a named curve and
        // nothing moved faster than a blend of kMinSmoothedBlendTime
        bool bSmoothBlend = false;
    private:
        UInt32 m_uiAttachedItems = 0;
        bool m_bNamedCurve = false;
    };
    void ApplyForItems(kBlendInterpolatorExtraData* extraData, const std::vector<std::pair<NiBlendInterpolator::InterpArrayItem*, kBlendInterpItem*>>& items, kWeightType type, const Step& step);
    float GetElapsedTime(float fLastTime, float fTime);
    void DetachZeroWeightItems(kBlendInterpolatorExtraData* extraData, NiBlendInterpolator* blendInterp);
    void WriteHooks();
}