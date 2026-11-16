#pragma once

#include "GameProcess.h"
#include "NiNodes.h"
#include "sequence_extradata.h"

// Start and stop animations of walks and runs (bMovementTransitions, needs bBlendSmoothing).
// These are files of a walk or run group (Forward ... FastRight, any weapon type, sneak and power armor variant) in an
// AnimGroupOverride folder whose name contains _start or _stop (mtforward_start.kf, 2hafastforward_stop_a.kf; see
// TransitionAnims). They are never picked as the group's animation.
// - Start: when the actor starts walking or running from standing, the start animation of the group the engine picks
//   plays in the movement section. At its End text key the engine continues with the group's walk or run
//   (Animation::Update with the group queued), which starts at the step the legs are at.
// - Stop, player: when the engine ends the walk or run (Actor::PickAnimations), the stop animation whose first frame
//   best matches the legs plays instead and holds until its End key, where the engine blends to the idle.
//   Walking again before the halfway point blends back to the walk; later it restarts with the start animation.
// - Stop, NPCs walking a path: the stop animation starts once its root motion reaches the arrival point (StartArrival),
//   scaled to land there. See the NPC arrival code in the source.
// Both move the actor by their root motion. Clips come from the folder the group's animation comes from, or from a
// folder that has only start/stop animations of the group (they go with whatever walk plays). Weapon, actor and
// condition scoping and _1stperson therefore work as for any override.
// In 1st person the player's 3rd person skeleton plays only start and stop animations with an allowFirstPerson text
// key, and the player moves by their root motion, which the 1st person view doesn't show.
// Animations in _1stperson folders play on the 1st person skeleton and move the player by their motion.
namespace MovementTransitions
{
    // Returns whether MovementTransitions plays the sequence as a start or stop animation
    TransitionKind Of(const NiControllerSequence* sequence);

    // HandleAnimationChange's play of a movement group with bBlendSmoothing. Plays the group's start animation when
    // the actor starts moving from standing, otherwise destAnim. Returns the sequence played.
    BSAnimGroupSequence* PlayMovementAnim(AnimData* animData, BSAnimGroupSequence* destAnim, UInt16 groupId);

    // Returns true if a play of groupId is the walk or run that the playing start animation continues with at its End
    // key (the engine starting the queued group), not a new play of the group.
    bool ContinuesStart(const AnimData* animData, UInt16 groupId);

    // Animation::Update's clock of a section's current sequence (SectionClockHook). While a start or stop animation
    // plays or fades in the movement section, everything blending there runs at fMoveSpeed. Returns true when the
    // sequence counts as animating for the engine's own clock (the group's speed) even though it is easing in.
    bool RunClock(AnimData* animData, NiControllerSequence* sequence, float fStep);

    void WriteHooks();
    // Hooks on calls that Stewie's NVAC inlines in its NVSEPlugin_Load
    void WriteDelayedHooks();
}
