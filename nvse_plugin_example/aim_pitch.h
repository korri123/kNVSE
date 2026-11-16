#pragma once

#include <string_view>

struct AnimData;
class Actor;
class NiControllerSequence;
class NiObjectNET;
struct NiQuatTransform;
class kBlendInterpolatorExtraData;
struct kBlendInterpItem;

// Third person aiming up and down without aimup/aimdown animations. The game weights a weapon group's up and down
// variants by the actor's pitch (MiddleHighProcess::UpdateAimSequences: Actor::GetLooking plus the aim chase angle;
// the up variant is fully weighted at -fAimUpDegrees, 90 by default). The vanilla rifle variants only turn
// Bip01 Spine, Spine1 and Spine2 by 30 degrees each.
//
// An "aimPitch" text key on a KF does the same in code: the sequence's pose of the spine bones is rotated by the
// pitch about the actor's right axis, so whatever the animation points forward points where the actor looks.
// Without arguments the pitch is split evenly over Bip01 Spine, Spine1 and Spine2.
// "aimPitch: <bone> <share>, <bone> <share>, ..." names the bones and their shares instead (shares totalling less
// than 1 turn the upper body less than the pitch).
// The rotation is part of the sequence's pose, so it fades with the sequence's weight on each bone.
// Third person skeletons only (first person turns the camera). Requires bBlendSmoothing. Aim transitions
// (AimTransitions) behave like the key without arguments unless they have their own key.
namespace AimPitch
{
    // Returns true if the sequence has an aimPitch key.
    bool HasKey(const NiControllerSequence& sequence);
    // Returns true if the sequence is rotated by the pitch: it has the key or is an aim transition.
    bool Applies(const NiControllerSequence& sequence);

    // The shares an aimPitch key gives the bones of a sequence, parsed once per AttachInterpolators. Holds views into
    // the sequence's text key, so it is valid only as long as the sequence.
    class Shares
    {
    public:
        explicit Shares(const NiControllerSequence& sequence);
        // Returns the bone's share, 0 if it has none. Pass a NiFixedString directly (its string_view conversion maps
        // null to empty).
        float Get(std::string_view bone) const;

    private:
        struct Entry
        {
            std::string_view bone;
            float share;
        };
        static constexpr unsigned int kMaxBones = 16;
        Entry m_entries[kMaxBones];
        unsigned int m_count = 0;
        bool m_default = false;
    };

    // Stores the pitch for the actor's skeleton. Runs before the bones are blended
    // (Animation::UpdateMovementNoWorldUpdate, called from Actor::UpdateAnimationMovement for the player and
    // Actor::UpdateAnimMovement for other actors).
    void Update(AnimData* animData, Actor* actor);

    // Rotates an item's pose of its bone by the bone's share of the pitch. Runs before the item is blended.
    void Apply(const kBlendInterpolatorExtraData* extraData, const kBlendInterpItem* extraItem, NiObjectNET* target,
        NiQuatTransform& kTransform);

    void WriteHooks();
}
