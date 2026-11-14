#pragma once

#include "GameProcess.h"
#include "NiNodes.h"
#include "sequence_extradata.h"

// Animations of raising the weapon to the sights and lowering it (bAimTransitions, needs bBlendSmoothing and
// bFixSpineBlendBug), the player only: files of an aimIS group (any weapon type, sneak variant) in an AnimGroupOverride
// folder whose name contains _start (hip to sights: 2haaimis_start.kf) or _stop (sights to hip: 2haaimis_stop.kf;
// TransitionAnims), taken from the folder the aimIS animation comes from or from one with only those. They are never
// picked as the group's animation.
// - The engine switches aim and aimIS in the weapon section (Actor::SetIronSights: Animation::BlendOut of the section,
//   which has Actor::StartWeaponGroup play the other at once). From aim to aimIS a start animation plays instead, from
//   aimIS to aim a stop animation, under the group the engine asked for, and on to it at the animation's End key
//   (Animation::Update ends the weapon section's Clamp sequence there, by the same BlendOut), with the animation's blend
//   out: it ends in that aim's pose.
// - Released or pressed again partway, the animation of the other way plays from the point the weapon has reached
//   (progress towards the sights: a start animation's fraction of its length, one minus a stop animation's), or, when
//   there is none, the same one plays back to its beginning, where that aim takes over. From an aim that is in, a way
//   without an animation is the game's blend: lowering the weapon from the sights with only a start animation.
// - The engine's request decides the way (Switch): it asks for the aim it wants whenever that is not the group of the
//   section's sequence. An aim transition is of the aimIS group whichever way it heads, so Update asks in the engine's
//   place where it would not.
// - Animation::Update ends a sequence by its time since it started (offset + the Animation's time,
//   Animation::GetDuration), not its frequency: an animation played back runs its offset backwards (RunClock). Its
//   text keys that act at a time (sounds, scripts) don't act meanwhile (IsPlayedBack).
// - The engine's check before firing that aim and its up and down variants have blended in counts an aim transition as
//   not yet in (Actor::FiresWeapon). Stewie's Tweaks bFireWhileAiming skips that check: the attack then cuts the animation
//   as it cuts the aim.
// - Third person: the animations have no up/down variants; they turn with the aim pitch as with an aimPitch text key
//   without arguments (AimPitch).
// - The camera zoom keeps its own time (fIronSightsFOVTimeChange, 0.25 s by default).
namespace AimTransitions
{
    // Whether AimTransitions plays the sequence as a start or stop animation of aiming
    TransitionKind Of(const NiControllerSequence* sequence);
    // Whether the sequence is an aim transition playing back: its time runs backwards, which whatever acts on text keys
    // as the time passes them would take for the animation starting over on every frame
    bool IsPlayedBack(const NiControllerSequence* sequence);

    // Whether the engine's check before firing counts the sequence as not yet aimed in (Actor::FiresWeapon): an aim
    // transition
    bool HoldsFire(const NiControllerSequence* sequence);

    // HandleAnimationChange's switch of the weapon section to an aim or aimIS: plays an aim transition, turns the one
    // that plays around or keeps it. Returns the sequence that plays instead of to, or null for the regular switch (no
    // aim transition, or one at its end handing over)
    BSAnimGroupSequence* Switch(AnimData* animData, BSAnimGroupSequence* to);

    // Every frame (kMessage_MainGameLoop): has the engine ask for the aim it wants (Switch) where it would not by
    // itself: aim pressed while the player's aim transition heads to the hip, and one played back to its beginning
    void Update();

    // Animation::Update's clock of a section's current sequence (SectionClockHook): runs the time of the aim
    // transitions played back backwards
    void RunClock(AnimData* animData, const NiControllerSequence* sequence, float fStep);

    void WriteHooks();
    // hooks on calls that Stewie's NVAC inlines in its NVSEPlugin_Load
    void WriteDelayedHooks();
}
