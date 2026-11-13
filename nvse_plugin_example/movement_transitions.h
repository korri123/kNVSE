#pragma once
#include <string_view>

#include "GameProcess.h"
#include "NiNodes.h"
#include "sequence_extradata.h"

struct AnimPath;
struct SavedAnims;

// Start and stop animations of walks and runs (bMovementTransitions, needs bBlendSmoothing): files of a walk or run group
// (Forward ... FastRight, any weapon type, sneak and power armor variant) in an AnimGroupOverride folder whose name contains
// _start or _stop (mtforward_start.kf, 2hafastforward_stop_a.kf). They are never picked as the group's animation.
// - Start: when the actor starts walking or running from standing, the start animation of the group the engine picks
//   plays in the movement section under that group. At its End text key the engine goes on to the group's walk or run
//   (Animation::Update with the group queued), which starts at the step the legs are at.
// - Stop, the player: when the engine ends the walk or run (Actor::PickAnimations), the stop animation of that group whose
//   first frame matches the legs best plays instead and stays until its End key, where the engine blends to the idle.
//   Walking again before it is halfway through blends back to the walk, later it starts with the start animation.
// - Stop, NPCs walking a path: before they get to where the engine stops them, once the stop animation's root motion
//   reaches that point (StartArrival), scaled to land there; see the NPC arrivals in the source.
// Both move the actor by their root motion. Clips are taken from the folder the group's animation comes from, or from
// one that has only start/stop animations of the group (they go with whatever walk plays, another folder's or the
// vanilla one), so weapon, actor and condition scoping and _1stperson work as for any override.
// In 1st person the player's 3rd person skeleton plays only start and stop animations with an allowFirstPerson text
// key: while only that skeleton plays one, the player moves by its root motion, which the 1st person view doesn't show.
// Start and stop animations in _1stperson folders play on the 1st person skeleton and move the player by theirs.
namespace MovementTransitions
{
    // _start or _stop in the file name of a walk or run group's file: in other groups those are ordinary animations
    MovementTransition GetPathTransition(std::string_view path, UInt16 groupId);
    // A file added to an override set of groupId (SetOverrideAnimation): marks it and the set when it is a start or stop
    // animation
    void TagPath(SavedAnims& set, AnimPath& animPath, UInt16 groupId);
    // Whether the set has a file of that kind: a start or stop animation, or (None) an animation of the group itself
    bool HasPaths(const SavedAnims& set, MovementTransition transition);
    // The engine goes on from a start or stop animation at its End key (Animation::Update). BSAnimGroupSequence::
    // InitAnimGroup leaves End as it is when the KF has none or it is not after Start (it only warns), which would be at
    // once: then it is the animation's end. Applied when one is loaded (LoadCustomAnimation)
    void FixEndKey(BSAnimGroupSequence* sequence);
    // Whether MovementTransitions plays the sequence as a start or stop animation
    MovementTransition Of(const NiControllerSequence* sequence);

    // HandleAnimationChange's play of a movement group with bBlendSmoothing: the group's start animation when the actor
    // starts moving from standing, else destAnim. Returns the sequence played
    BSAnimGroupSequence* PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* destAnim, UInt16 groupId);

    void WriteHooks();
    // hooks on calls that Stewie's NVAC inlines in its NVSEPlugin_Load
    void WriteDelayedHooks();
}
