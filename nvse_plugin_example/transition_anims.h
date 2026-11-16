#pragma once
#include <string_view>
#include <vector>

#include "GameProcess.h"
#include "NiNodes.h"

struct AnimPath;
struct SavedAnims;

// A start or stop animation: an AnimGroupOverride file with _start or _stop in its name, in a group that has them
enum class TransitionKind : UInt8
{
    None,
    Start,
    Stop,
};

// Start and stop animations play into and out of a group's animation instead of the blend, and are never picked as the
// group's animation (GetAnimPath). The groups that have them and the feature that plays them:
// - walk or run (Forward ... FastRight): MovementTransitions, in the movement section
// - aimIS: AimTransitions, in the weapon section
// In any other group a file named like that is an ordinary animation of its set.
// This namespace holds what the features share: which files are start and stop animations, which override set a
// group's come from, and how they are loaded. A sequence playing as one is marked by SequenceExtraData::transition,
// which the feature sets.
namespace TransitionAnims
{
    // Forward ... FastRight, any weapon type and variant: the groups Actor::PickAnimations plays while the actor
    // walks or runs
    bool IsGait(UInt16 groupId);
    // aimIS, any weapon type and variant
    bool IsAimIS(UInt16 groupId);

    // Returns what kind of file the path is in an override set of groupId
    TransitionKind GetPathKind(std::string_view path, UInt16 groupId);
    // Called for a file added to an override set of groupId (SetOverrideAnimation). Marks the file, and the set, when
    // it is a start or stop animation.
    void TagPath(SavedAnims& set, AnimPath& animPath, UInt16 groupId);
    // Returns true if any override set has a stop animation of a walk or run
    bool HasGaitStops();
    // Returns true if the set has a file of that kind: a start or stop animation, or (None) an animation of the group
    bool HasPaths(const SavedAnims& set, TransitionKind kind);
    // Returns the override set a group's start or stop animations come from. This is the set HandleAnimationChange
    // takes the group's animation from, or a set without an animation of the group (its start and stop animations go
    // with whatever plays). A set whose animation of the group is overridden by another set stays with that animation.
    SavedAnims* FindSet(AnimData* animData, UInt16 groupId, TransitionKind kind);
    std::vector<const AnimPath*> GetPaths(SavedAnims& set, TransitionKind kind);

    // Prepares a start or stop animation after loading (Overrides::Load).
    // - The engine continues from it at its End key (Animation::Update). When the KF has no End key, or it is not after
    //   Start, BSAnimGroupSequence::InitAnimGroup only warns and leaves it, which would end the animation at once.
    //   End is then set to the animation's end.
    // - Aiming animations are forced to Clamp whatever the KF says: they play once, forward or back, and hold at either
    //   end (NiControllerSequence::ComputeScaledTime wraps the time of a Loop). Walk and run animations keep their
    //   cycle type, since the engine continues from them at the End key either way.
    void Prepare(BSAnimGroupSequence* sequence);

    // Returns whether any feature plays the sequence as a start or stop animation
    TransitionKind Of(const NiControllerSequence* sequence);
}
