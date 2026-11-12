#pragma once

// Actor movement is the translation of the accum root (Bip01) from one update to the next, blended from every animation
// animating it by NiBlendAccumTransformInterpolator. Gamebryo 2.2's version (2.3 fixed part of it) loses motion when the
// animations in that blend change:
// - a second animation joining the one playing: the one playing restarts its accumulation (its last value is cleared), so
//   its next delta is zero;
// - one of two leaving: the remaining one restarts the same way;
// - a joining animation's first update: a valid zero delta instead of none, counted at its weight.
// Each stalled the actor for an update, or slowed it by the new animation's weight, at the start and end of a blend that
// moves it by root motion. Vanilla walk and run switches hide it: while the new movement animation eases in,
// Animation::Update moves the actor at the group's average speed instead (measured: no stalled update either way), which
// start and stop animations replace by their root motion (MovementTransitions). The fix keeps the accumulation going
// through both changes (as Gamebryo 2.3 does on the way in) and leaves the first update's delta out of the blend
// (bFixAccumRootBlend).
namespace AccumBlendFix
{
    void WriteHooks();
}
