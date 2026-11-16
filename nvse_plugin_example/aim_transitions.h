#pragma once

#include "GameProcess.h"
#include "NiNodes.h"
#include "sequence_extradata.h"

// Animations that raise the weapon to the sights and lower it again (bAimTransitions, needs bBlendSmoothing and
// bFixSpineBlendBug). Player only.
// These are files of an aimIS group (any weapon type, sneak variant) in an AnimGroupOverride folder whose name contains
// _start (hip to sights: 2haaimis_start.kf) or _stop (sights to hip: 2haaimis_stop.kf; see TransitionAnims). They come
// from the folder the aimIS animation comes from, or from one that has only those. They are never picked as the
// group's animation.
// - The engine switches between aim and aimIS in the weapon section (Actor::SetIronSights calls Animation::BlendOut on
//   the section, and Actor::StartWeaponGroup plays the other group at once). Here a start animation plays instead
//   (aim to aimIS) or a stop animation (aimIS to aim), under the group the engine asked for. At the animation's End key
//   the engine continues into that aim (Animation::Update ends the weapon section's Clamp sequence there, via the
//   same BlendOut) and the animation blends out, ending in that aim's pose.
// - If the key is released or pressed again partway, the animation for the other direction plays from the point the
//   weapon has reached (progress towards the sights: a start animation's fraction of its length, one minus that for a
//   stop animation). With no such animation, the same one plays back to its beginning, where that aim takes over.
//   A direction without an animation uses the game's blend.
// - The engine's request decides the direction (Switch): it asks for the aim it wants whenever that is not the group
//   of the section's sequence. An aim transition always belongs to the aimIS group, so Update makes the request in the
//   engine's place where the engine would not.
// - Animation::Update ends a sequence by its time since it started (offset + the Animation's time,
//   Animation::GetDuration), not its frequency. A played back animation runs its offset backwards (RunClock), and its
//   text keys that fire at a given time (sounds, scripts) are skipped meanwhile (IsPlayedBack).
// - Before firing, the engine checks that the aim and its up and down variants have blended in, and counts an aim
//   transition as not yet in (Actor::FiresWeapon). Stewie's Tweaks bFireWhileAiming skips that check, so an attack then
//   cuts the animation as it cuts the aim.
// - Third person: the animations have no up/down variants. They turn with the aim pitch as with an aimPitch text key
//   without arguments (AimPitch).
// - The camera zoom keeps its own timing (fIronSightsFOVTimeChange, 0.25 s by default).
namespace AimTransitions
{
    // Returns whether AimTransitions plays the sequence as a start or stop animation of aiming
    TransitionKind Of(const NiControllerSequence* sequence);
    // Returns true if the sequence is an aim transition playing back. Its time runs backwards, which code that acts on
    // text keys as time passes them would mistake for the animation restarting every frame.
    bool IsPlayedBack(const NiControllerSequence* sequence);

    // Returns true if the engine's pre-fire check (Actor::FiresWeapon) counts the sequence as not yet aimed in, i.e. it
    // is an aim transition
    bool HoldsFire(const NiControllerSequence* sequence);

    // HandleAnimationChange's switch of the weapon section to an aim or aimIS. Plays an aim transition, turns a
    // playing one around, or keeps it. Returns the sequence that plays instead of `to`, or null for the regular switch
    // (no aim transition, or one handing over at its end).
    BSAnimGroupSequence* Switch(AnimData* animData, BSAnimGroupSequence* to);

    // Called every frame (kMessage_MainGameLoop). Makes the engine request the aim it wants (Switch) in cases where it
    // would not: aim pressed while the player's aim transition heads to the hip, and a transition played back to its
    // beginning.
    void Update();

    // Animation::Update's clock of a section's current sequence (SectionClockHook). Runs the aim transitions that are
    // played back with their time going backwards.
    void RunClock(AnimData* animData, const NiControllerSequence* sequence, float fStep);

    void WriteHooks();
    // Hooks on calls that Stewie's NVAC inlines in its NVSEPlugin_Load
    void WriteDelayedHooks();
}
