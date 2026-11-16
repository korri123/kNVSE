#pragma once

#include <string_view>

#include "NiNodes.h"
#include "NiTypes.h"

struct AnimData;
class Actor;
class NiControllerSequence;
class kBlendInterpolatorExtraData;
struct kBlendInterpItem;

// An animation that keys part of a skeleton, played beside others (ActivateAnim of a KF with only the left arm, over
// the weapon's animations), is posed relative to bones it does not key: the left arm hangs from Bip01 Neck, and every
// weapon's stance bends the spine and neck differently, so the same arm keys put the hand somewhere else with each
// weapon. Text keys of such a KF:
//   "anchor: <bone>, <bone>, ..."   the bones whose pose keeps its place relative to the reference (the top of each
//                                   keyed part, e.g. Bip01 L Clavicle; the bones below it follow)
//   "anchorTo: <bone>"              the reference, an ancestor of the anchored bones. Default Bip01 Rotate (first
//                                   person: everything that moves it, the camera, walk and run bob, still moves the
//                                   anchored bones; the stance's spine and neck do not)
// The animation is taken to be authored on the skeleton at rest: the local transforms in the skeleton's NIF. The
// sequence's pose of an anchored bone, local L, becomes B^-1 * B0 * L, where B is the chain of local transforms from
// below the reference down to the bone's parent as this pass blended them, and B0 the same chain at rest. A track
// without translation uses the bone's rest translation. Applied to the sequence's item before the bone is blended, so a
// blend with other animations goes from their pose to the anchored one; the other animations are not changed. Bones
// above an anchored bone that the KF keys play as usual. Requires bBlendSmoothing.
namespace Anchors
{
    // The local transforms of a skeleton NIF's nodes by name, loaded once per file
    struct RestPose;

    // The anchor keys of a sequence, parsed when it attaches its interpolators. Holds views into its text keys.
    class Keys
    {
    public:
        explicit Keys(const NiControllerSequence& sequence);
        // The ancestor of target that the sequence anchors target to, or null
        NiAVObject* GetReference(const NiAVObject& target) const;

    private:
        static constexpr unsigned int kMaxBones = 16;
        const NiControllerSequence& m_sequence;
        std::string_view m_bones[kMaxBones];
        unsigned int m_count = 0;
        std::string_view m_reference;
    };

    // Kept on the kBlendInterpolatorExtraData of a bone that sequences anchor
    struct NodeData
    {
        // The chain at rest (B0) and the bone's rest transform, for this reference and parent
        const NiAVObject* reference = nullptr;
        const NiAVObject* parent = nullptr;
        const RestPose* restPose = nullptr;
        bool restValid = false;
        NiTransform restChain;
        NiTransform restLocal;
        // B^-1 * B0 of the evaluation in progress (Prepare)
        bool correctionValid = false;
        NiTransform correction;
    };

    // Marks the item as anchored to reference (null: not anchored), and the skeleton as needing its rest pose. Called
    // when the item's sequence attaches it.
    void Attach(kBlendInterpolatorExtraData& extraData, kBlendInterpItem& extraItem, NiAVObject* reference);

    // Finds the rest pose of a skeleton with anchored sequences, loading its NIF the first time. Runs before the
    // actor's bones are blended (with AimPitch::Update).
    void Update(AnimData* animData, Actor* actor);

    // Called when the bone's blend starts: evaluates the bones between it and the reference first
    // (BoneEvaluation::Require) and computes the correction of this evaluation
    void Prepare(kBlendInterpolatorExtraData* extraData, NiObjectNET* target);

    // Anchors an item's pose of its bone. Runs before the item is blended.
    void Apply(const kBlendInterpolatorExtraData* extraData, const kBlendInterpItem* extraItem, NiQuatTransform& kTransform);
}
