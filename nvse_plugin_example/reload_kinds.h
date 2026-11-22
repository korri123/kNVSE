#pragma once

#include "reload_state.h"

// The kind of an actor's reload, which decides whether an AnimGroupOverride set's "_partial" or "_ammo_swap" files play
// for its reload groups. Decided where the engine starts every reload, Actor::Reload (NPCs), PlayerCharacter::Reload
// and PlayerCharacter::ReloadLoopingWeapon, before they change the clip or play the reload group: rounds of the ammo it
// loads left in the clip make it Partial. The player's ammo swaps (PlayerCharacter::SelectDefaultAmmo: the ammo swap
// key; an ammo clicked in the Pip-Boy) replace the clip by an empty one of the new ammo and then call Reload: AmmoSwap.
// Reloads that fill the clip at once (type 0: equipping, loading) play nothing and leave the kind as it was. Kept in
// ReloadState until the next reload, so the rounds of a looping reload and a reload queued until an attack ends play
// the kind it started with.
namespace ReloadKinds
{
	// The kind of the actor's last reload; Full for an actor that hasn't reloaded
	ReloadKind Get(const Actor* actor);

	void WriteHooks();
}
