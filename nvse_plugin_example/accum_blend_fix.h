#pragma once

// Actor movement is the translation of the accum root (Bip01) from one update to the next, blended from every animation
// that animates it by NiBlendAccumTransformInterpolator. Gamebryo 2.2's version (2.3 fixed part of it) loses motion
// when the animations in that blend change:
// - a second animation joins the one playing: the playing one restarts its accumulation (its last value is cleared),
//   so its next delta is zero;
// - one of two animations leaves: the remaining one restarts the same way;
// - a joining animation's first update gives a valid zero delta instead of none, which counts at its weight.
// Each case stalls the actor for an update, or slows it by the new animation's weight, at the start and end of a blend
// that moves it by root motion. Vanilla walk and run switches hide this because Animation::Update moves the actor at
// the group's average speed while the new movement animation eases in. Start and stop animations replace that with
// their root motion (MovementTransitions). The fix keeps the accumulation going through both changes (as Gamebryo 2.3
// does when a second animation joins) and leaves the first update's delta out of the blend (bFixAccumRootBlend).
namespace AccumBlendFix
{
    void WriteHooks();
}
