#pragma once

#include "NiNodes.h"
#include "timed_execution.h"

struct AnimData;

// Text keys that set the rounds in an actor's weapon at a time in an animation:
//   "loadAmmo"           fills the clip (the clip size, or the ammo carried if that is less and the weapon uses ammo)
//   "loadAmmo: <n>"      loads n rounds, up to that
//   "unloadAmmo"         empties the clip
//   "unloadAmmo: <n>"    takes n rounds out of it
// Rounds in the clip are part of the ammo carried, so unloaded rounds stay in the inventory.
// The player and NPCs; for the player only the keys of the skeleton of the view the animation started in (both views'
// KFs may have them).
//
// A reload KF with a loadAmmo key also keeps the engine from filling the clip at the start of the reload
// (PlayerCharacter::Reload / Actor::Reload fill it before the animation plays): the clip keeps the rounds it had until
// the keys change them. A reload cut short before its loadAmmo key leaves the clip as it was.
// In a looping reload (shotguns, lever-action rifles) the engine loads a round when the next one starts (the round
// animation's Hit key, where it restarts) or when the player's attack ends the loop. A round whose KF loaded rounds by
// key before that takes the place of the engine's load; a round whose key comes later or never loads as without keys.
// An attack that ends the loop before the round's loadAmmo keys loads what they would have (the round in the hand goes
// in), and the rest of that round's animation loads nothing more. So "loadAmmo: 3" makes each shell 3 rounds, for a
// weapon whose Ammo Use (rounds per shot, which Actor::UseAmmo takes from the clip) is 3. NPCs loop with
// bFixNPCLoopingReloads; without it they play one round as a plain reload, which fills the clip at once as before.
// Prevent Reload Cancelling (Stewie's Tweaks with PreventReloadCancelling.dll) shows the rounds from before a reload in
// the ammo counter until 70 % of it; during a reload whose fill is held the counter shows the clip (WriteLateHooks).
// A polled AnimGroupOverride set does not restart a reload whose keys change the clip: Overrides::Poll plays only
// looping groups again.
namespace AmmoKeys
{
    struct Key
    {
        bool load = false;
        // 0: all (fill or empty)
        SInt32 rounds = 0;
    };

    // The keys among the sequence's text keys (TextKeys::Table reads them once per KF)
    TimedExecution<Key> Parse(const NiControllerSequence& sequence);

    // A sequence starting on one of an actor's skeletons (HandleExtraOperations): whether its keys act
    bool OnSequenceStart(AnimData* animData, BSAnimGroupSequence* sequence);

    // A key of the sequence reached by the animation time; sequence null for keys acting out of turn
    void Fire(Actor* actor, const Key& key, const BSAnimGroupSequence* sequence);

    // A save is loading: the player's reload state is the save's (NPCs' Animations are made again)
    void Reset();
    // Whether the engine's load of the looping reload round now ending adds a round: not if keys loaded the round
    bool EngineLoadsRound(const Actor* actor);
    void WriteDelayedHooks();
    // Once, at the first frame of play: after other plugins' deferred init hooks
    void WriteLateHooks();
}
