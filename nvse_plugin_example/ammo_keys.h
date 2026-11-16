#pragma once

#include "NiNodes.h"

struct AnimData;
template <typename T>
class TimedExecution;

// Text keys that set the rounds in the player's weapon at a time in an animation:
//   "loadAmmo"           fills the clip (the clip size, or the ammo carried if that is less)
//   "loadAmmo: <n>"      loads n rounds, up to that
//   "unloadAmmo"         empties the clip
//   "unloadAmmo: <n>"    takes n rounds out of it
// Rounds in the clip are part of the ammo carried, so unloaded rounds stay in the inventory.
// Player only, and only the keys of the skeleton of the view the animation started in (both views' KFs may have them).
//
// A reload KF with a loadAmmo key also keeps the engine from filling the clip at the start of the reload
// (PlayerCharacter::Reload fills it before the animation plays): the clip keeps the rounds it had until the keys change
// them. A reload cut short before its loadAmmo key leaves the clip as it was.
// In a looping reload (shotguns, lever-action rifles) the engine loads a round when the next one starts (the round
// animation's Hit key, where it restarts) or when attacking ends the loop. A round whose KF loaded rounds by key before
// that takes the place of the engine's load; a round whose key comes later or never loads as without keys. After an
// attack ended the loop, the rest of that round's animation loads nothing more.
// Prevent Reload Cancelling (Stewie's Tweaks with PreventReloadCancelling.dll) holds the clip back until 70 % of every
// reload in the ammo counter's update, so by its source a loadAmmo key earlier than that shows only then (not tested).
namespace AmmoKeys
{
    struct Key
    {
        bool load = false;
        // 0: all (fill or empty)
        SInt32 rounds = 0;
    };

    // The parsed keys of the sequence's KF, nullptr without any. Kept per KF path for the session
    TimedExecution<Key>* Find(const BSAnimGroupSequence& sequence);

    // A sequence starting on one of the player's skeletons (HandleExtraOperations): whether its keys act
    bool OnSequenceStart(AnimData* animData, BSAnimGroupSequence* sequence);

    // A key of the sequence reached by the animation time
    void Fire(const Key& key, const BSAnimGroupSequence* sequence);

    void Reset();
    void WriteDelayedHooks();
}
