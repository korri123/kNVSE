#pragma once

// NPCs loaded the whole clip of a weapon with a looping reload (ReloadW-Z: Cowboy Repeater, lever-action and hunting
// shotguns, revolvers) in one round. Actor::ReloadAlt (0x8A83C0) tests IsReloadLoop and calls Actor::Reload (0x8A8420)
// on both branches; only the player has a looping version (PlayerCharacter::ReloadLoopingWeapon 0x95D450). Actor::Reload
// fills the clip as the reload starts and queues ReloadXStart, and Actor::HandlePostAnimationActions (0x8BA600) continues
// the loop while rounds + 1 < clip: it ended after the first round, then replayed the round animation as a plain reload.
// A reload started at once (reload type 2: the combat AI in cover, use weapon packages) played the round animation once
// as a plain reload. The loop loads its rounds with QueueEquipObject, which leaves an NPC's clip as it is.
// NPCs now reload like the player: the clip keeps its rounds when the reload starts and each round adds one, up to what
// Actor::Reload would have filled (the ammo the actor carries or, if its weapon uses none, the whole clip), and the last
// round's animation plays to its end.
namespace NPCLoopingReloadFix
{
	// modReloadAnimsFixed: bFixModReloadAnims is applied (the engine's looping path follows the reload anim that plays)
	void ApplyHooks(bool modReloadAnimsFixed);
	void WriteDelayedHooks();
}
