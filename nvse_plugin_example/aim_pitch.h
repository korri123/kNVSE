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
// variants by the actor's pitch (MiddleHighProcess::UpdateAimSequences: Actor::GetLooking + the aim chase angle, all of
// the up variant at -fAimUpDegrees, 90 by default); the vanilla rifle ones (2haaimup, 2hraimup, ...) turn Bip01 Spine,
// Spine1 and Spine2 by 30 degrees each and nothing else.
//
// An "aimPitch" text key on a KF does that in code: its pose of the spine bones is turned by the pitch, about the axis
// that points to the actor's right, so whatever the animation points forward points where the actor looks. Without
// arguments the pitch is split evenly over Bip01 Spine, Spine1 and Spine2; "aimPitch: <bone> <share>, <bone> <share>, ..."
// names the bones and their share instead (shares below a total of 1 turn the upper body less than the pitch).
// The turn is part of the animation's own pose, so it blends in and out with the animation's weight on each bone. While
// weight 0. Third person skeletons only (first person turns the camera); needs bBlendSmoothing. Aim transitions
// (AimTransitions) turn as with the key without arguments unless they have one.
namespace AimPitch
{
    bool HasKey(const NiControllerSequence& sequence);
    // Whether the sequence turns by the pitch: it has the key or is an aim transition
    bool Applies(const NiControllerSequence& sequence);

    // The shares an aimPitch key gives the bones of a sequence, parsed once per AttachInterpolators. Views into the
    // sequence's text key: valid while the sequence is.
    class Shares
    {
    public:
        explicit Shares(const NiControllerSequence& sequence);
        // empty: 0. Pass a NiFixedString directly: its string_view conversion maps null to empty
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

    // Before the actor's bones are blended (Animation::UpdateMovementNoWorldUpdate, from Actor::UpdateAnimationMovement
    // for the player, Actor::UpdateAnimMovement for the others): stores the pitch for its skeleton
    void Update(AnimData* animData, Actor* actor);

    // Turns an item's pose of its bone by its share of the pitch. Before the item is blended
    void Apply(const kBlendInterpolatorExtraData* extraData, const kBlendInterpItem* extraItem, NiObjectNET* target,
        NiQuatTransform& kTransform);

    void WriteHooks();
}
