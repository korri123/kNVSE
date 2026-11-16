#pragma once

#include <vector>

#include "NiNodes.h"
#include "NiTypes.h"

class NiAVObject;
class NiNode;
class NiObjectNET;
class NiControllerSequence;
class NiInterpolator;
struct NiQuatTransform;
struct kBlendInterpItem;
class kBlendInterpolatorExtraData;

// The Weapon node is always parented to Bip01 R Hand. To have the left hand hold the weapon, animators counter-animate
// the Weapon track against the right hand and bake it densely so the two cancel out. Two things break that
// cancellation: blending local transforms during transitions, and interpolating between sparse keys, where the
// weapon's keys (relative to the right hand) and the left hand's keys follow different paths. In both cases the weapon
// drifts away from its hand.
//
// Each animation places the weapon consistently relative to the right hand, the left hand and the body (the hands'
// closest common ancestor). This module measures, per animation and time, which hands hold the weapon (rigid to, and
// for the left hand also touching), or reads it from "weaponSpace: right|left|body|auto" text keys. It then:
// - blends the weapon relative to the hand that every blended animation accepts: the right hand (the regular blend) if
//   they all accept it, otherwise the left hand. Where they disagree it blends relative to the body, which moves the
//   weapon smoothly between the animations' poses.
// - for left hand stretches, interpolates the weapon relative to the left hand between keys instead of relative to the
//   right hand, so sparse keys do not make it drift.
// "weaponSpace: off" opts an animation out: its share of the blend weight uses the regular blend.
//
// When the weapon stays on the right hand and the blended animations also hold it with the left hand (two-handed aim
// and aimIS), the left hand is kept on it during blends (bKeepLeftHandOnWeapon). Each arm is blended joint by joint,
// so the hand can drift off the weapon between two poses that both hold it. That error is measured using the Weapon
// node's animations weighted as each arm bone's own blend weights them, and removed from the hand with two-bone IK on
// the upper arm and forearm while the Weapon node is evaluated. The arms are evaluated first in the same pass
// (RequireChains, BoneEvaluation).
namespace WeaponHandBlend
{
    constexpr unsigned int kMaxChainLength = 8;

    // Nodes from just below the closest common ancestor of both hands down to each hand, top first
    struct HandChains
    {
        NiAVObject* right[kMaxChainLength] = {};
        NiAVObject* left[kMaxChainLength] = {};
        unsigned int rightCount = 0;
        unsigned int leftCount = 0;
    };

    bool IsWeaponNode(const NiObjectNET* node);

    // How firmly an animation's weapon is held by each hand at some time, 0..1. A weapon held with both hands is accepted
    // by both; one moving relative to both hands by neither.
    struct Acceptance
    {
        float right = 1.0f;
        float left = 0.0f;

        bool IsRightOnly() const;
    };

    // Local transforms of the hand chains' nodes (HandChains::right, left), top first
    struct ChainLocals
    {
        NiTransform right[kMaxChainLength];
        NiTransform left[kMaxChainLength];
    };

    // What a detached item contributes to the blend. A detached item (sequence deactivated, weight still fading out
    // under blend smoothing) has no sequence and its update time no longer advances, so its pose is captured once on
    // detach. Also kept for items the Weapon node drops at once while the arms still fade the sequence out
    // (SequenceInfo::fading).
    struct ItemPose
    {
        Acceptance acceptance;
        // the sequence's arm chains
        ChainLocals locals;
        // the sequence's Weapon track, as the Weapon node's blend evaluated it
        NiQuatTransform weapon;
        // the weapon relative to the left hand, from the left hand keys (SequenceInfo::SampleLeftSpace)
        NiTransform relativeToLeft;
        bool hasRelativeToLeft = false;
    };

    // A stretch of sequence time, end exclusive
    struct TimeRange
    {
        float begin;
        float end;
    };

    // The weapon relative to the left hand at one key time
    struct LeftSpaceKey
    {
        float time;
        NiPoint3 translate;
        float rotate[4]; // quaternion w, x, y, z; consecutive keys lie in the same hemisphere
        float scale;
    };

    // A sequence that animates the Weapon node, keyed by its Weapon interpolator. The interpolator references keep the
    // addresses from being reused by another sequence while the entry lives. An entry is dropped once nothing else holds
    // any of them: the sequence is gone and so are its items in the blends (the arms' items may outlive the Weapon node's).
    struct SequenceInfo
    {
        NiPointer<NiInterpolator> weaponInterpolator = nullptr;
        // The sequence's hand chain interpolators, null where it does not animate a node. Held because a detached item's
        // are looked up by address in the arms' blends while it fades out (HoldLeftHand), when its sequence may be gone.
        NiPointer<NiInterpolator> rightInterpolators[kMaxChainLength];
        NiPointer<NiInterpolator> leftInterpolators[kMaxChainLength];
        // Acceptance sampled over the key time range. Empty means the right hand only throughout.
        float beginTime = 0.0f;
        float step = 0.0f;
        std::vector<Acceptance> acceptance;
        // The weapon relative to the left hand at the key times of the weapon and hand tracks. Filled for sequences that
        // hold it in the left hand and have keys sparse enough to drift; empty otherwise.
        std::vector<LeftSpaceKey> leftSpaceKeys;
        // "weaponSpace: off" stretches, sorted
        std::vector<TimeRange> offRanges;
        // The weapon model under the Weapon node when measured. Compared by identity only: the left hand contact test
        // depends on the model, so a different one is measured again.
        const NiAVObject* weaponModel = nullptr;
        bool hasDetachedPose = false;
        ItemPose detachedPose;
        // Whether a detached item is opted out. Captured on detach like detachedPose, but for every sequence.
        bool detachedOff = false;
        // Detached, while its items on the arms may still be fading out after the Weapon node's item is gone. The left
        // hand correction keeps it (with detachedPose) in its model of the arms until their blends no longer have it.
        // Cleared when it attaches again, or when an evaluation that measured the arms' weights found none.
        bool fading = false;

        Acceptance Sample(float fTime) const;
        bool IsOff(float fTime) const;
        // Catmull-Rom through leftSpaceKeys. Returns false if there are none.
        bool SampleLeftSpace(float fTime, NiTransform& relativeToLeft) const;
    };

    // What keeping the left hand on the weapon last did to the left arm (upper arm, forearm, hand): the rotations their
    // blends gave the bones and the rotations they were given instead. A bone that still has the latter has not been
    // blended since (bone LOD, an update of the Weapon node alone), so it gets the former back before the arm is read
    // again. Otherwise the hand's correction, a step from where the arm is, would be added to itself.
    struct ArmReach
    {
        bool applied = false;
        NiMatrix33 blended[3];
        NiMatrix33 reached[3];
    };

    // State of one Weapon node, kept on its kBlendInterpolatorExtraData. Like the rest of that extra data it needs no
    // locks: an actor's skeleton is blended by one thread at a time, and sequences detach in the sequential animation
    // update or on the main thread, never while the same actor blends.
    struct NodeData
    {
        HandChains chains;
        bool chainsBuilt = false;
        bool chainsValid = false;
        // Every node of the chains has this pass's local transform (RequireChains, before each evaluation of the node)
        bool chainsCurrent = false;
        std::vector<SequenceInfo> sequences;
        ArmReach leftReach;
#if _DEBUG
        const NiNode* loggedParent = nullptr; // bWeaponHandBlendDebugLog
#endif
    };

    // The weapon model's bounding sphere in Weapon node space. Radius is 0 when there is no model.
    struct WeaponBound
    {
        NiPoint3 center = NiPoint3::ZERO;
        float radius = 0.0f;
    };

    // Lives for one evaluation of the Weapon node's blend interpolator
    class Context
    {
    public:
        Context(NiObjectNET* target, kBlendInterpolatorExtraData* extraData);
        explicit operator bool() const { return m_weapon != nullptr; }

        // Call before the Weapon node's blend starts: evaluates the hand chains' nodes first if the pass has not yet
        // (BoneEvaluation::Require), so Blend reads this pass's arms
        void RequireChains();

        // Collects one blended item. key identifies it for the weight setters. fTime is the update time of the
        // interpolator that produced kTransform (sequence-local time). Weights start at 0.
        void AddItem(const void* key, kBlendInterpItem* extraItem, float fTime, const NiQuatTransform& kTransform);
        void SetTranslateWeight(const void* key, float fWeight);
        void SetRotateWeight(const void* key, float fWeight);

        // Replaces kValue's valid translation and rotation with the blend of the collected items relative to the hand
        // they agree on. Leaves kValue alone when every item accepts the right hand, since the regular blend already
        // keeps the weapon on it. Then rotates the left arm so its hand stays where the items holding the weapon with
        // it hold it.
        void Blend(NiQuatTransform& kValue);

        // Blends a lone item, weighted 1
        void BlendSingle(kBlendInterpItem* extraItem, float fTime, NiQuatTransform& kValue);

        // Called when the sequence detaches the item from the Weapon node while it keeps fading out. fTime is the
        // item's update time, which stops advancing from now on. Keeps the pose the item is blended with until removed.
        void CacheDetachedPose(const kBlendInterpItem& extraItem, const NiControllerSequence& sequence, float fTime);

    private:
        bool EnsureChains();
        WeaponBound GetWeaponBound() const;
        const NiAVObject* GetWeaponModel() const;
        // Indices into NodeData::sequences, -1 if none. Measuring a new sequence may reallocate the vector.
        int FindSequence(const NiInterpolator* weaponInterpolator) const;
        int FindOrMeasureSequence(NiInterpolator* weaponInterpolator, const NiControllerSequence& sequence);

        NiAVObject* m_weapon = nullptr;
        NodeData* m_data = nullptr;
        bool m_anyHandOff = false;
        bool m_collecting = false;
    };
}
