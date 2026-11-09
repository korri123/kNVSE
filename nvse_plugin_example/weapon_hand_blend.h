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

// The Weapon node is always parented to Bip01 R Hand. Animators who want the weapon held by the left hand counter-animate
// the Weapon track against the right hand and bake densely so that the two cancel out. Two things break that cancellation:
// blending local transforms during transitions, and interpolating between sparse keys, where the weapon's keys (relative
// to the right hand) and the left hand's keys follow different paths. Either way the weapon swims away from its hand.
//
// Each animation places the weapon exactly relative to the right hand, the left hand and the body (the hands' closest
// common ancestor) at once; the space the weapon lives in only decides which relation to keep. This module measures, per
// animation and time, which hands the weapon is held by (rigid to, and for the left hand also touching), or reads it from
// "weaponSpace: right|left|body|auto" text keys. It then
// - blends the weapon relative to the hand that every blended animation accepts: the right hand (the regular blend) if
//   they all accept it, otherwise the left hand; where they disagree, relative to the body, which moves it smoothly
//   between the animations' poses;
// - for left hand stretches, interpolates the weapon relative to the left hand between keys instead of relative to the
//   right hand, so sparse keys no longer make it swim.
// "weaponSpace: off" opts an animation out: its share of the blend weight keeps the regular blend.
namespace WeaponHandBlend
{
    constexpr unsigned int kMaxChainLength = 8;

    // Nodes from just below the closest common ancestor of both hands down to each hand, top first.
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

    // What a detached item (sequence deactivated, weight still fading out under blend smoothing) contributes: it no longer
    // has its sequence and its update time stopped advancing on detach, so this is captured once on detach.
    struct ItemPose
    {
        Acceptance acceptance;
        // the sequence's hands relative to their closest common ancestor
        NiTransform sequenceRight;
        NiTransform sequenceLeft;
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

    // A sequence that animates the Weapon node, keyed by its Weapon interpolator. The reference keeps that address from
    // being reused by another sequence while the entry lives; entries are dropped once only this reference is left.
    struct SequenceInfo
    {
        NiPointer<NiInterpolator> weaponInterpolator = nullptr;
        // the sequence's hand chain interpolators, null where it does not animate a node. Only read while the sequence has
        // weaponInterpolator attached, which keeps them alive.
        NiInterpolator* rightInterpolators[kMaxChainLength] = {};
        NiInterpolator* leftInterpolators[kMaxChainLength] = {};
        // acceptance sampled over the key time range; empty: the right hand only throughout
        float beginTime = 0.0f;
        float step = 0.0f;
        std::vector<Acceptance> acceptance;
        // The weapon relative to the left hand at the key times of the weapon and hand tracks, for sequences that hold it in
        // the left hand and have keys sparse enough to swim; empty otherwise.
        std::vector<LeftSpaceKey> leftSpaceKeys;
        // "weaponSpace: off" stretches, sorted
        std::vector<TimeRange> offRanges;
        // the weapon model under the Weapon node when measured, compared by identity only: the left hand contact test
        // depends on the model, so another one is measured again
        const NiAVObject* weaponModel = nullptr;
        bool hasDetachedPose = false;
        ItemPose detachedPose;
        // whether a detached item is opted out, captured on detach like detachedPose but for every sequence
        bool detachedOff = false;

        Acceptance Sample(float fTime) const;
        bool IsOff(float fTime) const;
        // Catmull-Rom through leftSpaceKeys; false if there are none
        bool SampleLeftSpace(float fTime, NiTransform& relativeToLeft) const;
    };

    // State of one Weapon node, kept on its kBlendInterpolatorExtraData. Like the rest of that extra data it needs no
    // locks: an actor's skeleton is blended by one thread at a time, and sequences detach in the sequential animation
    // update or on the main thread, never while the same actor blends.
    struct NodeData
    {
        HandChains chains;
        bool chainsBuilt = false;
        bool chainsValid = false;
        std::vector<SequenceInfo> sequences;
#if _DEBUG
        const NiNode* loggedParent = nullptr; // bWeaponHandBlendDebugLog
#endif
    };

    // The weapon model's bounding sphere in Weapon node space; radius 0 when there is no model
    struct WeaponBound
    {
        NiPoint3 center = NiPoint3::ZERO;
        float radius = 0.0f;
    };

    // Lives for one evaluation of the Weapon node's blend interpolator.
    class Context
    {
    public:
        Context(NiObjectNET* target, kBlendInterpolatorExtraData* extraData);
        explicit operator bool() const { return m_weapon != nullptr; }

        // Collects one blended item; key identifies it for the weight setters. fTime is the update time of the
        // interpolator that produced kTransform (sequence-local time). Weights start at 0.
        void AddItem(const void* key, kBlendInterpItem* extraItem, float fTime, const NiQuatTransform& kTransform);
        void SetTranslateWeight(const void* key, float fWeight);
        void SetRotateWeight(const void* key, float fWeight);

        // Replaces kValue's valid translation and rotation with the blend of the collected items relative to the hand
        // they agree on. Leaves kValue alone when every item accepts the right hand: the regular blend already keeps the
        // weapon on it.
        void Blend(NiQuatTransform& kValue);

        // A lone item, weighted 1.
        void BlendSingle(kBlendInterpItem* extraItem, float fTime, NiQuatTransform& kValue);

        // The sequence detaches the item from the Weapon node while it keeps fading out; fTime is the item's update time,
        // which stops advancing from now on. Keeps the pose the item is blended with until it is removed.
        void CacheDetachedPose(const kBlendInterpItem& extraItem, const NiControllerSequence& sequence, float fTime);

    private:
        bool EnsureChains();
        WeaponBound GetWeaponBound() const;
        const NiAVObject* GetWeaponModel() const;
        // indices into NodeData::sequences, -1 if none; measuring a new sequence may reallocate the vector
        int FindSequence(const NiInterpolator* weaponInterpolator) const;
        int FindOrMeasureSequence(NiInterpolator* weaponInterpolator, const NiControllerSequence& sequence);
        void SampleHands(const SequenceInfo& info, float fTime, bool bInvalidateCache, NiTransform& right, NiTransform& left) const;

        NiAVObject* m_weapon = nullptr;
        NodeData* m_data = nullptr;
        bool m_anyHandOff = false;
        bool m_collecting = false;
    };
}
