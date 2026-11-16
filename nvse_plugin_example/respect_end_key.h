#pragma once

#include "NiNodes.h"

// respectEndKey / respectTextKeys text keys of the player's 1st person animations: the animation decides when the
// player's action happens and ends. The engine runs the player's actions from the 3rd person Animation in either view
// (Actor::PickAnimations reads its weapon section's action, and Animation::Update advances that action and ends the
// section by the key times of the sequence's TESAnimGroup). While the player is in 1st person, the 3rd person sequence
// therefore takes the 1st person animation's key times and blend times.
// - A TESAnimGroup belongs to the KF, and every sequence made from it, for every actor, shares it
//   (Animation::AddAnimation, Overrides' CreateSequence). The 3rd person sequence gets a copy of its group with the
//   1st person times instead, and gets its own group back once nothing applies them (EndFrame): in 3rd person, or when
//   the 1st person animation is no longer tracked. Writing the shared group would give NPCs playing the same KF the
//   player's times.
// - Main thread: Apply, Keep, EndFrame. Any thread: TimesSource and OriginalGroup.
namespace RespectEndKey
{
    // The 3rd person sequence takes anim1st's key times and blend times until the next EndFrame. Call this or Keep
    // before each EndFrame to continue.
    void Apply(BSAnimGroupSequence* anim3rd, BSAnimGroupSequence* anim1st);
    // The 3rd person sequence keeps the times it has until the next EndFrame
    void Keep(BSAnimGroupSequence* anim3rd);
    // Once per frame (kMessage_MainGameLoop), after the time tracked animations applied theirs. Sequences that nothing
    // applied times to since the last call get their own anim group back.
    void EndFrame();

    // Returns the 1st person animation whose times the 3rd person sequence has, or null
    BSAnimGroupSequence* TimesSource(BSAnimGroupSequence* anim3rd);
    // Returns the sequence's anim group without a copy that Apply gave it
    TESAnimGroup* OriginalGroup(BSAnimGroupSequence* sequence);

    void WriteDelayedHooks();
}
