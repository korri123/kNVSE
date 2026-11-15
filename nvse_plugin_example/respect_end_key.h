#pragma once

#include "NiNodes.h"

// respectEndKey / respectTextKeys text keys of the player's 1st person animations: the animation decides when the
// player's action happens and ends. The engine runs the player's actions by the 3rd person Animation in either view
// (Actor::PickAnimations reads its weapon section's action, Animation::Update advances that action and ends the section
// by the key times of the sequence's TESAnimGroup), so while the player is in 1st person the 3rd person sequence takes
// the 1st person animation's key times and blend times.
// - A TESAnimGroup belongs to the KF: every sequence made from it, every actor's, has the same one
//   (Animation::AddAnimation, Overrides' CreateSequence). The 3rd person sequence gets a copy of its group with the
//   1st person times instead, and its own group back once nothing applies them (EndFrame): in 3rd person, or once the
//   1st person animation is no longer tracked. Writing the shared group gave NPCs playing the same KF the player's times.
// - Main thread: Apply, Keep, EndFrame. TimesSource and OriginalGroup on any thread.
namespace RespectEndKey
{
    // The 3rd person sequence takes anim1st's key times and blend times until the next EndFrame (kept by calling this or
    // Keep before each)
    void Apply(BSAnimGroupSequence* anim3rd, BSAnimGroupSequence* anim1st);
    // The 3rd person sequence keeps the times it has until the next EndFrame
    void Keep(BSAnimGroupSequence* anim3rd);
    // Once per frame (kMessage_MainGameLoop), after the time tracked animations applied theirs: the sequences nothing
    // applied times to since the last call get their own anim group back
    void EndFrame();

    // The 1st person animation whose times the 3rd person sequence has, or null
    BSAnimGroupSequence* TimesSource(BSAnimGroupSequence* anim3rd);
    // The sequence's anim group without a copy Apply gave it
    TESAnimGroup* OriginalGroup(BSAnimGroupSequence* sequence);

    void WriteDelayedHooks();
}
