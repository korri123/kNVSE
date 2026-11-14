#pragma once
#include <string_view>
#include <vector>

#include "GameProcess.h"
#include "NiNodes.h"

struct AnimPath;
struct SavedAnims;

// A start or stop animation: an AnimGroupOverride file with _start or _stop in its name, of a group that has them
enum class TransitionKind : UInt8
{
    None,
    Start,
    Stop,
};

// Start and stop animations play into and out of a group's animation instead of the blend, and are never picked as the
// group's animation (GetAnimPath). The groups that have them, and the feature that plays them:
// - a walk or run (Forward ... FastRight): MovementTransitions, in the movement section
// - aimIS: AimTransitions, in the weapon section
// In any other group a file named like that is an ordinary animation of its set.
// Here is what the features share: which files are start and stop animations, the override set a group's come from, and
// what they are loaded with. The mark of a sequence playing as one is SequenceExtraData::transition, set by the feature.
namespace TransitionAnims
{
    // Forward ... FastRight, any weapon type and variant: the groups Actor::PickAnimations plays while the actor walks
    // or runs
    bool IsGait(UInt16 groupId);
    // aimIS, any weapon type and variant
    bool IsAimIS(UInt16 groupId);

    // What the file is by its name in an override set of groupId
    TransitionKind GetPathKind(std::string_view path, UInt16 groupId);
    // A file added to an override set of groupId (SetOverrideAnimation): marks it and the set when it is a start or stop
    // animation
    void TagPath(SavedAnims& set, AnimPath& animPath, UInt16 groupId);
    // Whether any override set has a stop animation of a walk or run
    bool HasGaitStops();
    // Whether the set has a file of that kind: a start or stop animation, or (None) an animation of the group itself
    bool HasPaths(const SavedAnims& set, TransitionKind kind);
    // The override set a group's start or stop animations come from: the one HandleAnimationChange takes the group's
    // animation from, or one without an animation of the group (start and stop animations for whatever plays). Those of
    // a set whose animation of the group another set overrides stay with that animation
    SavedAnims* FindSet(AnimData* animData, UInt16 groupId, TransitionKind kind);
    std::vector<const AnimPath*> GetPaths(SavedAnims& set, TransitionKind kind);

    // A start or stop animation as loaded (LoadCustomAnimation).
    // - The engine goes on from one at its End key (Animation::Update). BSAnimGroupSequence::InitAnimGroup leaves End as
    //   it is when the KF has none or it is not after Start (it only warns), which would be at once: then it is the
    //   animation's end.
    // - One of aiming is Clamp whatever the KF says: it plays once, forward or back, and holds at either end
    //   (NiControllerSequence::ComputeScaledTime wraps the time of a Loop). A walk's or run's keeps its cycle type: the
    //   engine goes on from it at the End key either way.
    void Prepare(BSAnimGroupSequence* sequence);

    // Whether a feature plays the sequence as a start or stop animation, whichever feature
    TransitionKind Of(const NiControllerSequence* sequence);
}
